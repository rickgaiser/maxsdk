#include <mx/bios.h>
#include <mx/cpu.h>

/* R5900 data cache operations (EE Core User's Manual, CACHE instruction):
   0x1C DHWOIN = hit write back without invalidate, 0x1A DHIN = hit
   invalidate. Each takes one 64-byte line. */

/* Only cached addresses: the operations are undefined on uncached, UCAB and
   scratchpad addresses. */
static int cached(const void *p) { return (uintptr_t)p < 0x20000000u; }

void mx_dcache_writeback(const void *p, u32 size) {
  if (!cached(p))
    return;
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)(MX_DCACHE_LINE - 1);
  uintptr_t end = (uintptr_t)p + size;
  for (; a < end; a += MX_DCACHE_LINE)
    __asm__ volatile("sync.l\n\tcache 0x1c, 0(%0)\n\tsync.l" : : "r"(a) : "memory");
}

void mx_dcache_invalidate(const void *p, u32 size) {
  if (!cached(p))
    return;
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)(MX_DCACHE_LINE - 1);
  uintptr_t end = (uintptr_t)p + size;
  for (; a < end; a += MX_DCACHE_LINE)
    __asm__ volatile("sync.l\n\tcache 0x1a, 0(%0)\n\tsync.l" : : "r"(a) : "memory");
}

void mx_dcache_flush_all(void) { mx_bios_flush_cache(0); }
