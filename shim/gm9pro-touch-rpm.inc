/* GLINK RPM transport adapted from ios-sailfish patch 0100.
 * SDM660 addresses: stock sdm660.dtsi glink-rpm-native-xprt.
 * Standalone diagnostic, no concurrent AP RPM client. No SPMI writes.
 */
#include <stdbool.h>
#include <stddef.h>
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
static void tg_delay_us(unsigned us)
{
    uint64_t end = ticks() + (freq() * us + 999999) / 1000000;
    while (ticks() < end) __asm__ volatile("yield");
}
static void tg_copy(void *d, const void *s, size_t n)
{
    uint8_t *to = d; const uint8_t *from = s;
    for (size_t i = 0; i < n; i++) to[i] = from[i];
}
static int tg_compare(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
/* Keep freestanding UART output bounded; numeric results printed separately. */
static void tg_rpm_log(const char *format, ...)
{
    (void)format;
}
#define RPM_MSGRAM_PA   0x00778000u
#define RPM_MSGRAM_SIZE 0x7000u
#define RPM_IPC_PA      0x17911000u     /* SDM660 APCS IPC, +0x08 */
#define RPM_TOC_MAGIC   0x67727430u     /* grt0 */
#define RPM_FIFO_AP2R   0x61703272u
#define RPM_FIFO_R2AP   0x72326170u
#define RPM_LCID        1u

#define CMD_VERSION       0u
#define CMD_VERSION_ACK   1u
#define CMD_OPEN          2u
#define CMD_CLOSE         3u
#define CMD_OPEN_ACK      4u
#define CMD_TX_DATA       9u
#define CMD_CLOSE_ACK     11u
#define CMD_TX_DATA_CONT  12u
#define CMD_READ_NOTIF    13u

struct rpm_fifo {
	volatile uint32_t *tail, *head, *data;
	uint32_t len;
};

static struct {
	struct rpm_fifo tx, rx;
	uintptr_t ipc;
	bool mapped, up, remote_open, open_acked;
	uint32_t rcid, msg_id;
	uint32_t resp[64];              /* last complete TX_DATA payload */
	uint32_t resp_len;
	bool resp_ready;
} rpm;

static uint32_t
fifo_avail_rx(void)
{
	uint32_t head = *rpm.rx.head, tail = *rpm.rx.tail;
	if (head >= rpm.rx.len || tail >= rpm.rx.len || ((head | tail) & 3u)) return 0;
	return head >= tail ? head - tail : rpm.rx.len - tail + head;
}

static void
fifo_peek(uint32_t off, uint32_t *buf, uint32_t n)
{
	uint32_t pos = (*rpm.rx.tail + off) % rpm.rx.len;
	for (uint32_t i = 0; i < n / 4; i++) {
		buf[i] = rpm.rx.data[pos / 4];
		pos = (pos + 4) % rpm.rx.len;
	}
}

static void
fifo_advance(uint32_t n)
{
	__asm__ volatile("dsb sy" ::: "memory");
	*rpm.rx.tail = (*rpm.rx.tail + n) % rpm.rx.len;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
rpm_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(rpm.ipc + 0x08) = 1u;
}

/* n is a multiple of 4; the message is padded to 8 bytes. */
static bool
rpm_send(const uint32_t *w, uint32_t n)
{
	uint32_t total = (n + 7u) & ~7u;
	for (unsigned tries = 0;; tries++) {
		uint32_t head = *rpm.tx.head, tail = *rpm.tx.tail;
		if (head >= rpm.tx.len || tail >= rpm.tx.len || ((head | tail) & 3u)) return false;
		uint32_t avail = tail <= head ? rpm.tx.len - head + tail : tail - head;
		if (avail > total) {
			break;
		}
		if (tries == 100) {
			tg_rpm_log("SWEET RPM: tx fifo full (head %u tail %u)\n", head, tail);
			return false;
		}
		tg_delay_us(1000);
	}
	uint32_t pos = *rpm.tx.head;
	for (uint32_t i = 0; i < total / 4; i++) {
		rpm.tx.data[pos / 4] = i < n / 4 ? w[i] : 0u;
		pos = (pos + 4) % rpm.tx.len;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*rpm.tx.head = pos;
	rpm_kick();
	return true;
}

static bool
rpm_send_cmd(uint32_t cmd, uint32_t param1, uint32_t param2)
{
	uint32_t w[2] = {cmd | (param1 << 16), param2};
	return rpm_send(w, sizeof(w));
}

static bool
rpm_send_open(void)
{
	static const char name[] = "rpm_requests";
	uint32_t w[2 + 4] = {CMD_OPEN | (RPM_LCID << 16), sizeof(name)};
	tg_copy(&w[2], name, sizeof(name));
	return rpm_send(w, 8u + ((sizeof(name) + 3u) & ~3u));
}

/* Handle one incoming message; false when nothing (complete) is there. */
static bool
rpm_rx_one(void)
{
	uint32_t avail = fifo_avail_rx(), h[4];
	if (avail < 8) {
		return false;
	}
	fifo_peek(0, h, 8);
	uint32_t cmd = h[0] & 0xffffu, p1 = h[0] >> 16, p2 = h[1];
	switch (cmd) {
	case CMD_VERSION:
		fifo_advance(8);
		(void)rpm_send_cmd(CMD_VERSION_ACK, 1, 0);
		break;
	case CMD_VERSION_ACK:
		fifo_advance(8);
		if (p1 != 1) {
			tg_rpm_log("SWEET RPM: version ack %u features %#x\n", p1, p2);
		} else if (p2 != 0) {
			(void)rpm_send_cmd(CMD_VERSION, 1, 0);
		} else if (!rpm.up) {
			rpm.up = true;
			(void)rpm_send_open();
		}
		break;
	case CMD_OPEN: {
		if (p2 > 128) return false;
		uint32_t len = 8u + ((p2 + 7u) & ~7u), name[8] = {0};
		if (avail < len) {
			return false;
		}
		fifo_peek(8, name, MIN(p2 + 3u, 31u) & ~3u);
		fifo_advance(len);
		if (tg_compare((const char *)name, "rpm_requests", 13) == 0) {
			rpm.rcid = p1;
			rpm.remote_open = true;
			(void)rpm_send_cmd(CMD_OPEN_ACK, p1, 0);
		} else {
			tg_rpm_log("SWEET RPM: ignoring remote open of '%.24s'\n", (const char *)name);
		}
		break;
	}
	case CMD_OPEN_ACK:
		fifo_advance(8);
		if (p1 == RPM_LCID) {
			rpm.open_acked = true;
		}
		break;
	case CMD_CLOSE:
		fifo_advance(8);
		(void)rpm_send_cmd(CMD_CLOSE_ACK, p1, 0);
		if (p1 == rpm.rcid) {
			rpm.remote_open = false;
		}
		break;
	case CMD_TX_DATA:
	case CMD_TX_DATA_CONT: {
		if (avail < 16) {
			return false;
		}
		fifo_peek(0, h, 16);
		uint32_t chunk = h[2], left = h[3];
		if (p1 != rpm.rcid) return false;
		if (chunk > rpm.rx.len - 16 || chunk > sizeof(rpm.resp) || (chunk & 3u) || left != 0) return false;
		if (avail < ((16 + chunk + 7u) & ~7u)) {
			return false;
		}
		if (cmd == CMD_TX_DATA) {
			rpm.resp_len = 0;
		}
		uint32_t take = MIN(chunk, sizeof(rpm.resp) - rpm.resp_len) & ~3u;
		fifo_peek(16, (uint32_t *)((uint8_t *)rpm.resp + rpm.resp_len), take);
		rpm.resp_len += take;
		fifo_advance((16u + chunk + 7u) & ~7u);
		if (left == 0) {
			rpm.resp_ready = true;
		}
		break;
	}
	case CMD_READ_NOTIF:
		fifo_advance(8);
		rpm_kick();
		break;
	default:
		/* intent/rx-done/signals: not used on this intentless edge */
		tg_rpm_log("SWEET RPM: rx cmd %u p1 %u p2 %#x dropped\n", cmd, p1, p2);
		fifo_advance(8);
		break;
	}
	return true;
}

/* Poll up to ms for cond. */
#define RPM_POLL(ms, cond) ({                                   \
	unsigned _i;                                            \
	for (_i = 0; _i < (ms) && !(cond); _i++) {              \
	        for (unsigned _r = 0; _r < 64 && rpm_rx_one(); _r++) {                          \
	        }                                               \
	        if (!(cond)) {                                  \
	                tg_delay_us(1000);                            \
	        }                                               \
	}                                                       \
	(cond);                                                 \
})

static bool
rpm_open(void)
{
	if (rpm.up && rpm.remote_open && rpm.open_acked) {
		return true;
	}
	if (!rpm.mapped) {
		uintptr_t ram = RPM_MSGRAM_PA;
		rpm.ipc = RPM_IPC_PA;
		if (!ram || !rpm.ipc) {
			tg_rpm_log("SWEET RPM: MMIO mapping failed\n");
			return false;
		}
		volatile uint32_t *toc = (volatile uint32_t *)(ram + RPM_MSGRAM_SIZE - 256);
		uint32_t count = toc[1];
		hex("RPM TOC magic", toc[0]);
		hex("RPM TOC count", count);
		if (toc[0] != RPM_TOC_MAGIC || count > 20) {
			tg_rpm_log("SWEET RPM: bad TOC magic %#x count %u\n", toc[0], count);
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			uint32_t id = toc[2 + 3 * i], off = toc[3 + 3 * i], size = toc[4 + 3 * i];
			struct rpm_fifo *f = id == RPM_FIFO_AP2R ? &rpm.tx : id == RPM_FIFO_R2AP ? &rpm.rx : NULL;
			if (f == NULL || off > RPM_MSGRAM_SIZE - 8 || size > RPM_MSGRAM_SIZE - off - 8 || size < 32 || (off & 3u) || (size & 3u)) {
				continue;
			}
			f->tail = (volatile uint32_t *)(ram + off);
			f->head = f->tail + 1;
			f->data = f->tail + 2;
			f->len = size;
		}
		if (!rpm.tx.data || !rpm.rx.data) {
			tg_rpm_log("SWEET RPM: TOC has no ap2r/r2ap fifo\n");
			return false;
		}
		hex("RPM TX length", rpm.tx.len);
		hex("RPM RX length", rpm.rx.len);
		tg_rpm_log("SWEET RPM: tx len %u head %u tail %u, rx len %u head %u tail %u\n",
		    rpm.tx.len, *rpm.tx.head, *rpm.tx.tail, rpm.rx.len, *rpm.rx.head, *rpm.rx.tail);
		rpm.mapped = true;
	}
	/* As Linux: our write index and read index start over, then negotiate. */
	rpm.up = rpm.remote_open = rpm.open_acked = false;
	*rpm.tx.head = 0;
	*rpm.rx.tail = 0;
	(void)rpm_send_cmd(CMD_VERSION, 1, 0);
	bool ok = RPM_POLL(1000u, rpm.up && rpm.remote_open && rpm.open_acked);
	hex("RPM link version ready", rpm.up);
	hex("RPM link remote open", rpm.remote_open);
	hex("RPM link open ACK", rpm.open_acked);
	tg_rpm_log("SWEET RPM: link %s (version %d, remote open %d rcid %u, open ack %d)\n",
	    ok ? "up" : "FAILED", rpm.up, rpm.remote_open, rpm.rcid, rpm.open_acked);
	return ok;
}

/* Active-set RPM vote; resource is ldoa (PM660) or ldob (PM660L). */
bool
tg_rpm_ldo(uint32_t resource, unsigned ldo, uint32_t uv)
{
	if (!rpm_open()) {
		return false;
	}
	uint32_t id = ++rpm.msg_id;
	uint32_t w[] = {
		CMD_TX_DATA | (RPM_LCID << 16), 0, 0, 0,        /* chunk set below */
		0x00716572u, 20u + 24u,                         /* "req", length */
		id, 0u, resource, ldo, 24u,                  /* active set, "ldoa" n */
		0x6e657773u, 4u, 1u,                            /* swen = 1 */
		0x00007675u, 4u, uv,                            /* uv */
	};
	w[2] = sizeof(w) - 16u;
	rpm.resp_ready = false;
	if (!rpm_send(w, sizeof(w))) {
		return false;
	}
	if (!RPM_POLL(1000u, rpm.resp_ready)) {
		tg_rpm_log("SWEET RPM: L%u: no answer\n", ldo);
		return false;
	}
	/* "req", length, then {type, length, data} records */
	bool acked = false, err = false;
	uint32_t words = MIN(rpm.resp_len / 4, 2u + rpm.resp[1] / 4), i = 2;
	while (i + 2 <= words) {
		uint32_t type = rpm.resp[i], len = rpm.resp[i + 1];
		if (len > (words - i - 2) * 4) return false;
		if (type == 0x2367736du && len == 4 && rpm.resp[i + 2] == id) {
			acked = true;
		} else if (type == 0x00727265u) {
			hex("RPM error response length", len);
			char text[32] = {0};
			tg_copy(text, &rpm.resp[i + 2], MIN(len, sizeof(text) - 1));
			tg_rpm_log("SWEET RPM: L%u: error '%s'\n", ldo, text);
			err = true;
		}
		i += 2 + (len + 3) / 4;
	}
	tg_rpm_log("SWEET RPM: L%u %u uV: %s\n", ldo, uv, acked && !err ? "ok" : "FAILED");
	return acked && !err;
}
