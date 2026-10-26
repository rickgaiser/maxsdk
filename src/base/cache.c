#include <mx/bios.h>
#include <mx/cpu.h>

/* R5900 data cache operations (EE Core User's Manual, CACHE instruction):
   0x1C DHWOIN = hit write back without invalidate, 0x1A DHIN = hit
   invalidate, each of one 64-byte line; 0x14 DXWBIN = index write back and
   invalidate, of the line of way vAddr[0] in set vAddr[11:6]. */

/* Only cached addresses: the operations are undefined on uncached, UCAB and
   scratchpad addresses. */
static int cached(const void *p) { return (uintptr_t)p < 0x20000000u; }

void mx_dcache_writeback(const void *p, u32 size) {
  if (!cached(p))
    return;
  /* as many lines as the cache has or more: all of the cache, by index
     (64 sets of 2 ways) */
  if (size >= MX_DCACHE_BYTES) {
    __asm__ volatile("sync.l" : : : "memory");
    for (uintptr_t a = 0x80000000u; a < 0x80000000u + MX_DCACHE_BYTES / 2; a += MX_DCACHE_LINE)
      __asm__ volatile("cache 0x14, 0(%0)\n\tsync.l\n\tcache 0x14, 1(%0)\n\tsync.l"
                       : : "r"(a) : "memory");
    return;
  }
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
