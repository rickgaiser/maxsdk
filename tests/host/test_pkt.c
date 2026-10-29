/* Packet builder: bytes compared with hand-made packets, including the
   packets apps/llvm_bench/vudemo sends on the PS2. */

#include "test.h"
#include <mx/dma.h>
#include <mx/pkt.h>
#include <string.h>

void mx_dcache_writeback(const void *p, u32 size) { (void)p, (void)size; }

static u32 buf[256] __attribute__((aligned(16)));

static void vudemo_ref_tag(void) {
  /* vudemo: tags[0][0] = 1 | 3ull << 28 | addr << 32;
             tags[0][1] = 0x01000101 | (u64)(0x6c010000 | 3) << 32 */
  struct mx_pkt p;
  mx_pkt_begin(&p, buf, sizeof buf, MX_PKT_VIF);
  mx_pkt_ref(&p, (void *)0x00123450, 1, mx_vif_stcycl(1, 1),
             mx_vif_unpack(MX_UNPACK_V4_32, 3, 1, 0));
  mx_pkt_end(&p, 0, 0);
  CHECK_EQ(mx_pkt_finish(&p), 2);
  u64 *q = (u64 *)buf;
  CHECK_EQ(q[0], 1 | 3ull << 28 | 0x00123450ull << 32);
  CHECK_EQ(q[1], 0x01000101ull | (u64)(0x6c010000u | 3) << 32);
  CHECK_EQ(q[2], 7ull << 28);
  CHECK_EQ(q[3], 0);
}

static void vif_cnt(void) {
  static const u32 data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  struct mx_pkt p;
  mx_pkt_begin(&p, buf, sizeof buf, MX_PKT_VIF);
  mx_pkt_cnt(&p, mx_vif_stcycl(1, 1), 0);
  mx_pkt_unpack(&p, MX_UNPACK_V4_32, 10, 2, 0, data); /* 1 code + 8 words */
  mx_pkt_vif(&p, mx_vif_mscal(0));
  mx_pkt_end(&p, mx_vif_flushe(), 0);
  CHECK_EQ(mx_pkt_finish(&p), 1 + 3 + 1);
  /* cnt tag: QWC 3 (9 words + mscal = 10, padded to 12) */
  CHECK_EQ(buf[0], 3 | 1u << 28);
  CHECK_EQ(buf[2], 0x01000101);
  CHECK_EQ(buf[4], 0x6c02000a);
  CHECK_EQ(buf[5], 1);
  CHECK_EQ(buf[12], 8);
  CHECK_EQ(buf[13], 0x14000000);
  CHECK_EQ(buf[14], 0);
  /* end tag with FLUSHE */
  CHECK_EQ(buf[16], 7u << 28);
  CHECK_EQ(buf[18], 0x10000000);
}

static void mpg_alignment(void) {
  static const u32 code[4] = {0x11, 0x22, 0x33, 0x44}; /* 2 instructions */
  struct mx_pkt p;
  mx_pkt_begin(&p, buf, sizeof buf, MX_PKT_RAW);
  mx_pkt_mpg(&p, code, 16, 8);
  CHECK_EQ(buf[0], 0);                     /* pad */
  CHECK_EQ(buf[1], 0x4a020008);            /* MPG 2 at 8 */
  CHECK_EQ(buf[2], 0x11);                  /* 64-bit aligned */
  CHECK_EQ(mx_pkt_finish(&p), 2);
  mx_pkt_begin(&p, buf, sizeof buf, MX_PKT_RAW);
  mx_pkt_direct(&p, 5);
  CHECK_EQ(buf[3], 0x50000005);
  CHECK_EQ(p.cur - p.base, 4);
}

static void gif_ad(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, buf, sizeof buf, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_pkt_ad_begin(&p, 2, 1);
  mx_pkt_ad(&p, MX_GS_REG_FRAME_1, mx_gs_frame(0, 10, MX_GS_PSM_CT32, 0));
  mx_pkt_ad(&p, MX_GS_REG_FINISH, 1);
  mx_pkt_end(&p, 0, 0);
  CHECK_EQ(mx_pkt_finish(&p), 1 + 3 + 1);
  u64 *q = (u64 *)buf;
  CHECK_EQ(q[0], 3 | 1ull << 28);
  CHECK_EQ(q[2], 2 | 1ull << 15 | 1ull << 60); /* NLOOP 2, EOP, PACKED, NREG 1 */
  CHECK_EQ(q[3], 0xe);
  CHECK_EQ(q[4], 10ull << 16);
  CHECK_EQ(q[5], 0x4c);
  CHECK_EQ(q[7], 0x61);
}

static void overflow(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, buf, 32, 0);
  mx_pkt_cnt(&p, 0, 0);
  for (int i = 0; i < 8; i++)
    mx_pkt_w32(&p, (u32)i);
  CHECK_EQ(p.overflow, 1);
  CHECK_EQ(mx_pkt_finish(&p), -1);
}

static void giftag_fields(void) {
  /* GIF tag with PRIM: NLOOP 3, EOP, PRE, PRIM triangle with Gouraud, PACKED, NREG 2 */
  u64 t = mx_giftag(3, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0), 0, 2);
  CHECK_EQ(t, 3 | 1ull << 15 | 1ull << 46 | 0xBull << 47 | 2ull << 60);
}

void test_pkt(void) {
  vudemo_ref_tag();
  vif_cnt();
  mpg_alignment();
  gif_ad();
  overflow();
  giftag_fields();
}
