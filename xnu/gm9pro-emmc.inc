/* BSD disk front end for the GM9 Pro's inherited SDCC state.
 * Appended to the hardware-tested PIO reader by prepare-gm9pro.py.
 * Experimental writes require gm9rw=1 and are limited to userdata.
 */
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>

#define GM9_MAX_PARTITIONS 256
static struct gm9_partition {
    uint64_t first, sectors;
    char name[37];
} gm9_parts[GM9_MAX_PARTITIONS + 1];
static unsigned gm9_part_count;
static int gm9_bmajor = -1, gm9_cmajor = -1;
static bool gm9_ready, gm9_failed;
static bool gm9_write_enabled, gm9_write_failed, gm9_write_safe;
/* A bounded sector overlay for diagnostics; never writes the physical card. */
#define GM9_COW_SLOTS 65536U
static bool gm9_cow_enabled;
static uint32_t gm9_cow_keys[GM9_COW_SLOTS]; /* relative sector + 1; 0 = empty */
static uint8_t gm9_cow_data[GM9_COW_SLOTS][512];
static unsigned gm9_cow_count;
static unsigned gm9_userdata;
static LCK_GRP_DECLARE(gm9_emmc_group, "gm9_emmc");
static LCK_MTX_DECLARE(gm9_emmc_lock, &gm9_emmc_group);

/* Called with gm9_emmc_lock held. Userdata is below 2^32 sectors. */
static int
gm9_cow_slot(uint32_t block, bool create)
{
    uint32_t key = block + 1U;
    if (!key) return -1;
    uint32_t slot = (block * 2654435761U) & (GM9_COW_SLOTS - 1U);
    for (unsigned n = 0; n < GM9_COW_SLOTS; n++) {
        if (gm9_cow_keys[slot] == key) return (int)slot;
        if (!gm9_cow_keys[slot]) {
            if (!create) return -1;
            gm9_cow_keys[slot] = key;
            gm9_cow_count++;
            return (int)slot;
        }
        slot = (slot + 1U) & (GM9_COW_SLOTS - 1U);
    }
    return -1;
}

static uint64_t
gm9_le64(const uint8_t *p)
{
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static uint32_t
gm9_crc_update(uint32_t crc, const uint8_t *p, unsigned count)
{
    while (count--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}

static struct gm9_partition *
gm9_partition(dev_t dev)
{
    unsigned index = minor(dev);
    if (!gm9_ready || index > gm9_part_count || !gm9_parts[index].sectors)
        return NULL;
    return &gm9_parts[index];
}

/* The whole primary entry array is checked before any disk node is published. */
static int
gm9_read_gpt(void)
{
    uint8_t sector[512], ext[512];
    uint64_t first_usable, last_usable, table_lba, table_sectors;
    uint32_t entries, entry_size, expected, crc = ~0U;
    if (read32(EMMC + 0x24) & 3) return EBUSY;
    uint32_t fifo = read32(EMMC + 0x250) & ~(1U << 10);
    write32(EMMC + 0x250, fifo);
    __asm__ volatile("dsb sy" ::: "memory");
    if (read32(EMMC + 0x250) != fifo) return EIO;
    if (read_block(8, 0, ext)) return EIO;
    gm9_write_safe = ext[33] == 0 && ext[15] == 0 && ext[171] == 0;
    uint64_t sectors = le32(ext + 212);
    if (ext[192] < 5 || ext[192] > 8 || ext[61] ||
        (ext[179] & 7) || sectors <= 0x400000) return ENXIO;
    if (read_block(17, 0, sector)) return EIO;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return EINVAL;
    if (read_block(17, 1, sector)) return EIO;
    if (memcmp(sector, "EFI PART", 8) || le32(sector + 8) != 0x10000 ||
        le32(sector + 20) || gm9_le64(sector + 24) != 1 ||
        gm9_le64(sector + 32) != sectors - 1) return EINVAL;
    uint32_t header_size = le32(sector + 12);
    expected = le32(sector + 16);
    if (header_size < 92 || header_size > 512) return EINVAL;
    bzero(sector + 16, 4);
    if (gm9_crc32(sector, header_size) != expected) return EINVAL;
    first_usable = gm9_le64(sector + 40);
    last_usable = gm9_le64(sector + 48);
    table_lba = gm9_le64(sector + 72);
    entries = le32(sector + 80);
    entry_size = le32(sector + 84);
    expected = le32(sector + 88);
    /* A bounded, sector-aligned table keeps malformed media out of the PIO path. */
    if (!entries || entries > GM9_MAX_PARTITIONS || entry_size != 128)
        return ENOTSUP;
    table_sectors = ((uint64_t)entries * entry_size + 511) / 512;
    if (table_lba < 2 || first_usable >= last_usable ||
        last_usable >= sectors - 1 || table_lba >= first_usable ||
        table_sectors > first_usable - table_lba) return EINVAL;
    bzero(gm9_parts, sizeof(gm9_parts));
    gm9_parts[0].sectors = sectors;
    strlcpy(gm9_parts[0].name, "user-area", sizeof(gm9_parts[0].name));
    for (uint64_t block = 0; block < table_sectors; block++) {
        if (read_block(17, (uint32_t)(table_lba + block), sector)) return EIO;
        unsigned bytes = (unsigned)MIN(512ULL, (uint64_t)entries * 128 - block * 512);
        crc = gm9_crc_update(crc, sector, bytes);
        for (unsigned offset = 0; offset < bytes; offset += 128) {
            const uint8_t *entry = sector + offset;
            unsigned index = (unsigned)block * 4 + offset / 128 + 1;
            uint8_t used = 0;
            for (unsigned j = 0; j < 16; j++) used |= entry[j];
            if (!used) continue;
            uint64_t first = gm9_le64(entry + 32), last = gm9_le64(entry + 40);
            if (first < first_usable || last > last_usable || first > last)
                return EINVAL;
            for (unsigned j = 1; j < index; j++) {
                struct gm9_partition *other = &gm9_parts[j];
                if (other->sectors && first < other->first + other->sectors &&
                    other->first <= last) return EINVAL;
            }
            gm9_parts[index].first = first;
            gm9_parts[index].sectors = last - first + 1;
            for (unsigned j = 0; j < 36; j++) {
                uint16_t ch = entry[56 + j * 2] | (uint16_t)entry[57 + j * 2] << 8;
                if (!ch) break;
                gm9_parts[index].name[j] = ch >= 32 && ch <= 126 ? (char)ch : '?';
            }
        }
    }
    if (~crc != expected) {
        printf("GM9 eMMC: GPT array CRC mismatch: expected %08x, got %08x\n",
            expected, ~crc);
        return EINVAL;
    }
    gm9_part_count = entries;
    unsigned userdata_count = 0;
    for (unsigned i = 1; i <= entries; i++) {
        if (gm9_parts[i].sectors && !strcmp(gm9_parts[i].name, "userdata")) {
            gm9_userdata = i;
            userdata_count++;
        }
    }
    if (userdata_count != 1) gm9_userdata = 0;
    printf("GM9 eMMC: GPT header and entry-array CRC verified; %llu sectors\n", sectors);
    return 0;
}

static int
gm9_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
    if (!gm9_partition(dev)) return ENXIO;
    return (flags & FWRITE) && ((!gm9_write_enabled && !gm9_cow_enabled) || gm9_write_failed ||
        minor(dev) != gm9_userdata) ? EROFS : 0;
}

static int
gm9_close(__unused dev_t dev, __unused int flags, __unused int type,
    __unused struct proc *p)
{
    return 0;
}

static void
gm9_strategy(buf_t bp)
{
    int error = 0;
    caddr_t address = NULL;
    uint32_t count = buf_count(bp), completed = 0;
    struct gm9_partition *part = gm9_partition(buf_device(bp));
    daddr64_t block = buf_blkno(bp);
    buf_setresid(bp, count);
    bool writing = !(buf_flags(bp) & B_READ);
    if (!part) { error = ENXIO; goto done; }
    if (writing && ((!gm9_write_enabled && !gm9_cow_enabled) || gm9_write_failed ||
        minor(buf_device(bp)) != gm9_userdata)) { error = EROFS; goto done; }
    if (block < 0 || count % 512 || (uint64_t)block > part->sectors ||
        count / 512 > part->sectors - (uint64_t)block) {
        error = EINVAL; goto done;
    }
    if (!count) goto done;
    if (buf_map(bp, &address)) { error = EIO; goto done; }
    lck_mtx_lock(&gm9_emmc_lock);
    while (completed < count) {
        uint64_t lba = part->first + (uint64_t)block + completed / 512;
        if (gm9_cow_enabled && minor(buf_device(bp)) == gm9_userdata) {
            uint64_t rel = (uint64_t)block + completed / 512;
            if (rel >= 0xffffffffULL) { error = EINVAL; break; }
            int slot = gm9_cow_slot((uint32_t)rel, writing);
            if (writing) {
                if (slot < 0) {
                    printf("GM9 eMMC: RAM overlay full (%u sectors); refusing writes\n", gm9_cow_count);
                    gm9_write_failed = true;
                    error = ENOSPC;
                    break;
                }
                memcpy(gm9_cow_data[slot], (uint8_t *)address + completed, 512);
            } else if (slot >= 0) {
                memcpy((uint8_t *)address + completed, gm9_cow_data[slot], 512);
            } else if (read_block(17, (uint32_t)lba, (uint8_t *)address + completed)) {
                error = EIO;
                break;
            }
            completed += 512;
            buf_setresid(bp, count - completed);
            continue;
        }
        int sent = 0;
        if ((writing && (gm9_write_failed || wp_write_one((uint32_t)lba,
                (uint8_t *)address + completed, &sent))) ||
            (!writing && read_block(17, (uint32_t)lba, (uint8_t *)address + completed))) {
            if (writing) gm9_write_failed = true;
            error = EIO; break;
        }
        completed += 512;
        buf_setresid(bp, count - completed);
    }
    lck_mtx_unlock(&gm9_emmc_lock);
    buf_unmap(bp);
done:
    if (error) buf_seterror(bp, error);
    buf_biodone(bp);
}

static int
gm9_ioctl(dev_t dev, u_long command, caddr_t data, __unused int flag,
    __unused struct proc *p)
{
    struct gm9_partition *part = gm9_partition(dev);
    if (!part) return ENXIO;
    switch (command) {
    case DKIOCGETBLOCKSIZE:
    case DKIOCGETPHYSICALBLOCKSIZE: *(uint32_t *)data = 512; return 0;
    case DKIOCSETBLOCKSIZE: return *(uint32_t *)data == 512 ? 0 : EINVAL;
    case DKIOCGETBLOCKCOUNT: *(uint64_t *)data = part->sectors; return 0;
    case DKIOCISWRITABLE: *(uint32_t *)data = (gm9_write_enabled || gm9_cow_enabled) &&
        !gm9_write_failed && minor(dev) == gm9_userdata; return 0;
    case DKIOCGETMAXBLOCKCOUNTREAD: *(uint64_t *)data = 128; return 0;
    case DKIOCGETMAXBLOCKCOUNTWRITE: *(uint64_t *)data = (gm9_write_enabled || gm9_cow_enabled) &&
        !gm9_write_failed && minor(dev) == gm9_userdata ? 1 : 0; return 0;
    default: return ENOTTY;
    }
}

static int
gm9_psize(dev_t dev)
{
    struct gm9_partition *part = gm9_partition(dev);
    return part ? (int)MIN(part->sectors, INT_MAX) : -1;
}

static int
gm9_read(dev_t dev, struct uio *uio, __unused int flags)
{
    return physio(gm9_strategy, NULL, dev, B_READ, minphys, uio, 512);
}

static int
gm9_write(dev_t dev, struct uio *uio, __unused int flags)
{
    if (!gm9_partition(dev)) return ENXIO;
    if ((!gm9_write_enabled && !gm9_cow_enabled) || gm9_write_failed || minor(dev) != gm9_userdata)
        return EROFS;
    return physio(gm9_strategy, NULL, dev, B_WRITE, minphys, uio, 512);
}

static const struct bdevsw gm9_bdevsw = {
    .d_open = gm9_open, .d_close = gm9_close, .d_strategy = gm9_strategy,
    .d_ioctl = gm9_ioctl, .d_dump = eno_dump, .d_psize = gm9_psize, .d_type = D_DISK,
};
static const struct cdevsw gm9_cdevsw = {
    .d_open = gm9_open, .d_close = gm9_close, .d_read = gm9_read,
    .d_write = gm9_write, .d_ioctl = gm9_ioctl, .d_stop = eno_stop,
    .d_reset = eno_reset, .d_ttys = NULL, .d_select = eno_select,
    .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = D_DISK,
};

static int
gm9_disk_init(void)
{
    if (gm9_ready) return 0;
    if (gm9_failed) return ENXIO;
    gm9_failed = true;
    regs = ml_io_map(0x0c0c4000, 0x1000);
    if (!regs) return ENOMEM;
    gm9_quiet = true;
    int error = gm9_read_gpt();
    if (error) return error;
    unsigned int requested = 0;
    gm9_cow_enabled = PE_parse_boot_argn("gm9cow", &requested, sizeof(requested)) &&
        requested == 1 && gm9_userdata;
    requested = 0;
    gm9_write_enabled = PE_parse_boot_argn("gm9rw", &requested, sizeof(requested)) &&
        requested == 1 && gm9_write_safe && gm9_userdata && !gm9_cow_enabled;
    if (gm9_cow_enabled) printf("GM9 eMMC: RAM overlay enabled (%u sectors); physical writes disabled\n", GM9_COW_SLOTS);
    printf("GM9 eMMC: userdata writes %s (cache/CMDQ/WP safe=%d)\n",
        gm9_write_enabled ? "enabled" : "disabled", gm9_write_safe);
    gm9_bmajor = bdevsw_add(-1, &gm9_bdevsw);
    if (gm9_bmajor < 0) return ENXIO;
    gm9_cmajor = cdevsw_add_with_bdev(-1, &gm9_cdevsw, gm9_bmajor);
    if (gm9_cmajor < 0) {
        bdevsw_remove(gm9_bmajor, &gm9_bdevsw);
        return ENXIO;
    }
    /* Registration is serialized by the single IOFindBSDRoot bootstrap caller. */
    for (unsigned i = 0; i <= gm9_part_count; i++) {
        if (!gm9_parts[i].sectors) continue;
        char name[24];
        if (i) snprintf(name, sizeof(name), "emmc0s%u", i);
        else strlcpy(name, "emmc0", sizeof(name));
        int mode = (gm9_write_enabled || gm9_cow_enabled) && i == gm9_userdata ? 0640 : 0440;
        if (!devfs_make_node(makedev(gm9_bmajor, i), DEVFS_BLOCK,
            UID_ROOT, GID_OPERATOR, mode, "%s", name) ||
            !devfs_make_node(makedev(gm9_cmajor, i), DEVFS_CHAR,
            UID_ROOT, GID_OPERATOR, mode, "r%s", name))
            panic("GM9 eMMC: devfs node creation failed for %s", name);
        printf("GM9 eMMC: %s %s first=%llu sectors=%llu %s\n",
            name, gm9_parts[i].name, gm9_parts[i].first, gm9_parts[i].sectors,
            mode == 0640 ? "writable" : "read-only");
    }
    gm9_ready = true;
    return 0;
}

static int
gm9_buffer_test(dev_t dev, daddr64_t block, uint32_t count, bool reading,
    uint8_t *data)
{
    vnode_t vp = NULL;
    int error = bdevvp(dev, &vp);
    if (error) return error;
    buf_t bp = buf_alloc(vp);
    if (!bp) { vnode_put(vp); return ENOMEM; }
    buf_setdataptr(bp, (uintptr_t)data);
    buf_setcount(bp, count);
    buf_setblkno(bp, block);
    if (reading) {
        buf_setflags(bp, B_READ);
    } else {
        /* buf_biodone balances this even when strategy rejects the write. */
        vnode_startwrite(vp);
    }
    bdevsw[major(dev)].d_strategy(bp);
    error = buf_biowait(bp);
    if (!error && buf_resid(bp)) error = EIO;
    buf_free(bp);
    vnode_put(vp);
    return error;
}

void gm9pro_emmc_disk_test(void);
void
gm9pro_emmc_disk_test(void)
{
    int error = gm9_disk_init();
    if (error) panic("GM9 eMMC: disk registration failed: %d", error);
    dev_t dev = makedev(gm9_bmajor, 0);
    uint8_t data[1024];
    error = gm9_buffer_test(dev, 0, sizeof(data), true, data);
    if (error || data[510] != 0x55 || data[511] != 0xaa ||
        memcmp(data + 512, "EFI PART", 8))
        panic("GM9 eMMC: BSD strategy MBR/GPT read failed: %d", error);
    printf("GM9 eMMC: BSD strategy multi-sector read PASS\n");
    if (gm9_open(dev, FWRITE, 0, NULL) != EROFS ||
        gm9_buffer_test(dev, 0, 512, false, data) != EROFS)
        panic("GM9 eMMC: write rejection failed");
    if (gm9_buffer_test(dev, gm9_parts[0].sectors, 512, true, data) != EINVAL)
        panic("GM9 eMMC: disk boundary check failed");
    printf("GM9 eMMC: write rejection and disk boundary checks PASS\n");
    printf("GM9 EMMC BSD DISK PASS\n");
    panic("GM9 eMMC disk diagnostic finished; no root filesystem selected");
}

int gm9pro_emmc_rootdev(dev_t *dev);
int
gm9pro_emmc_rootdev(dev_t *dev)
{
    char wanted[37] = {0};
    /* Never silently use an Android partition as a Darwin root volume. */
    if (!PE_parse_boot_argn("emmcroot", wanted, sizeof(wanted)) || !wanted[0])
        return EINVAL;
    int error = gm9_disk_init();
    if (error) return error;
    unsigned found = 0;
    for (unsigned i = 1; i <= gm9_part_count; i++) {
        if (gm9_parts[i].sectors && !strcmp(wanted, gm9_parts[i].name)) {
            if (found) return EINVAL;
            found = i;
        }
    }
    if (!found) return ENOENT;
    *dev = makedev(gm9_bmajor, found);
    return 0;
}
