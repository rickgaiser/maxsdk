#ifndef MX_CPU_H
#define MX_CPU_H

/* EE core: address spaces, register access, interrupts on/off, cache and
   COP0 counters. */

#include <mx/types.h>

/* ---- Address spaces ---------------------------------------------------- */

/* Main memory is seen three ways: cached (0x0...), uncached (0x2...) and
   uncached accelerated (UCAB, 0x3...). The scratchpad is at 0x7000_0000. DMA
   needs the physical address (docs/dma.md: an uncached alias crashed the
   PS2). */
#define MX_UNCACHED(p) ((__typeof__(&(p)[0]))(((uintptr_t)(p) & 0x0FFFFFFFu) | 0x20000000u))
#define MX_UCAB(p) ((__typeof__(&(p)[0]))(((uintptr_t)(p) & 0x0FFFFFFFu) | 0x30000000u))
#define MX_CACHED(p) ((__typeof__(&(p)[0]))((uintptr_t)(p) & 0x0FFFFFFFu))

MX_INLINE int mx_is_spr(const void *p) { return ((uintptr_t)p & 0xF0000000u) == 0x70000000u; }

/* Address for a DMA tag or MADR/TADR: physical, or an SPR offset with the
   SPR bit set. */
MX_INLINE u32 mx_phys(const void *p) {
  uintptr_t a = (uintptr_t)p;
  if (mx_is_spr(p))
    return (u32)((a & 0x3FFFu) | 0x80000000u);
  return (u32)(a & 0x0FFFFFFFu);
}

/* ---- Register access --------------------------------------------------- */

#define MX_R32(a) (*(volatile u32 *)(uintptr_t)(a))
#define MX_R64(a) (*(volatile u64 *)(uintptr_t)(a))

/* 128-bit store, for FIFOs that take whole quadwords. */
MX_INLINE void mx_w128(u32 addr, u128 v) {
  u64 lo = mx_qw_lo(v), hi = mx_qw_hi(v);
  __asm__ volatile("pcpyld $8, %1, %0\n\tsq $8, 0(%2)"
                   :
                   : "r"(lo), "r"(hi), "r"(addr)
                   : "$8", "memory");
}

/* ---- Barriers and interrupts ------------------------------------------- */

MX_INLINE void mx_sync(void) { __asm__ volatile("sync.l" ::: "memory"); }
MX_INLINE void mx_sync_p(void) { __asm__ volatile("sync.p" ::: "memory"); }
MX_INLINE void mx_barrier(void) { __asm__ volatile("" ::: "memory"); }

MX_INLINE u32 mx_status(void) {
  u32 s;
  __asm__ volatile("mfc0 %0, $12" : "=r"(s));
  return s;
}

MX_INLINE void mx_ei(void) { __asm__ volatile("ei" ::: "memory"); }

/* Disable interrupts. The R5900 can miss a DI that coincides with an
   interrupt, so it is repeated until Status.EIE reads 0 (EE Core User's
   Manual, the DI note). Returns whether they were enabled. */
MX_INLINE int mx_di(void) {
  int was = (mx_status() >> 16) & 1;
  do {
    __asm__ volatile("di\n\tsync.p" ::: "memory");
  } while (mx_status() & (1u << 16));
  return was;
}

MX_INLINE void mx_irq_restore(int was) {
  if (was)
    mx_ei();
}

/* ---- COP0 counters ----------------------------------------------------- */

/* Count runs at the EE clock (294.912 MHz). */
MX_INLINE u32 mx_count(void) {
  u32 c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c;
}

/* Performance counters (COP0 reg 25). The assembler has no MFPC/MTPC/MTPS,
   so they are encoded by hand. */
MX_INLINE void mx_pccr_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c800\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE void mx_pcr0_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c801\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE void mx_pcr1_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c803\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE u32 mx_pcr0(void) {
  u32 v;
  __asm__ volatile(".word 0x4008c801\n\tmove %0, $8" : "=r"(v) : : "$8");
  return v;
}
MX_INLINE u32 mx_pcr1(void) {
  u32 v;
  __asm__ volatile(".word 0x4008c803\n\tmove %0, $8" : "=r"(v) : : "$8");
  return v;
}

/* ---- Cache ------------------------------------------------------------- */

#define MX_DCACHE_LINE 64
#define MX_DCACHE_BYTES 8192

/* Write back (and keep) the data cache lines covering [p, p + size); from
   MX_DCACHE_BYTES on, all of the cache, written back and invalidated (128
   operations instead of one a line). */
void mx_dcache_writeback(const void *p, u32 size);
/* Invalidate the data cache lines covering [p, p + size), without writing
   back. Lines only partly inside the range lose their other bytes too. */
void mx_dcache_invalidate(const void *p, u32 size);
/* Write back and invalidate the whole data cache (BIOS FlushCache(0)). */
void mx_dcache_flush_all(void);

#endif
