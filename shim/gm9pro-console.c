/* UART-only implementation of the payload loader's console interface. */
#include "console.h"
volatile uint32_t *con_fb;
static void put(char c)
{
    volatile uint32_t *sr = (volatile uint32_t *)0x0c170008UL;
    unsigned int limit = 1000000;
    while (!(*sr & 8)) if (!--limit) return;
    *(volatile uint32_t *)0x0c170040UL = 1;
    *(volatile uint32_t *)0x0c170070UL = (uint8_t)c;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void text(const char *s) { while (*s) put(*s++); }
static void number(uint64_t n)
{
    text("0x");
    for (int i = 60; i >= 0; i -= 4) put("0123456789abcdef"[(n >> i) & 15]);
}
void con_clear(uint32_t bg) { (void)bg; }
void con_flush(void) { __asm__ volatile("dsb sy" ::: "memory"); }
void con_skip(void) { text("\r\n"); }
void con_str(const char *label, const char *s) { text(label); text(": "); text(s); con_skip(); }
void con_hex(const char *label, uint64_t n) { text(label); text(": "); number(n); con_skip(); }
void con_hex2(const char *label, const char *a, uint32_t x, const char *b, uint32_t y)
{ text(label); text(": "); text(a); number(x); text(" "); text(b); number(y); con_skip(); }
