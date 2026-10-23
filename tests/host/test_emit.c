/* Cursor emitters: the same bytes as the checked packet builder. */

#include "test.h"
#include <mx/emit.h>
#include <mx/pkt.h>
#include <string.h>

static u64 a[64] __attribute__((aligned(16)));
static u32 b[128] __attribute__((aligned(16)));

static void same(const u64 *end, int qwc) {
  CHECK_EQ((end - a) / 2, qwc);
  CHECK_EQ(memcmp(a, b, (size_t)qwc * 16), 0);
}

static void ad_packet(void) {
  memset(a, 0xAA, sizeof a);
  memset(b, 0x55, sizeof b);
  struct mx_pkt p;
  mx_pkt_begin(&p, b, sizeof b, 0);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_ad_begin(&p, 2, 1);
  mx_pkt_ad(&p, MX_GS_REG_PRIM, 0x1234);
  mx_pkt_ad(&p, MX_GS_REG_FINISH, 1);
  int n = mx_pkt_finish(&p);

  u64 *t = a, *c = mx_e_end(a, 0, 0, 0);
  c = mx_e_ad_tag(c, 2, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, 0x1234);
  c = mx_e_ad(c, MX_GS_REG_FINISH, 1);
  mx_e_close(t, c);
  same(c, n);
}

static void vif_chain(void) {
  static const u32 data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  memset(a, 0xAA, sizeof a);
  memset(b, 0x55, sizeof b);
  struct mx_pkt p;
  mx_pkt_begin(&p, b, sizeof b, MX_PKT_VIF);
  mx_pkt_ref(&p, (void *)0x00123450, 2, mx_vif_stcycl(1, 1),
             mx_vif_unpack(MX_UNPACK_V4_32, 4, 2, 0));
  mx_pkt_cnt(&p, 0, mx_vif_unpack(MX_UNPACK_V4_32, 10, 2, 0));
  mx_pkt_data(&p, data, 2);
  mx_pkt_end(&p, mx_vif_mscal(0), mx_vif_flushe());
  int n = mx_pkt_finish(&p);

  u64 *c = mx_e_ref(a, (void *)0x00123450, 2, mx_vif_stcycl(1, 1),
                    mx_vif_unpack(MX_UNPACK_V4_32, 4, 2, 0));
  c = mx_e_cnt(c, 2, 0, mx_vif_unpack(MX_UNPACK_V4_32, 10, 2, 0));
  c = mx_e_w4(c, 1, 2, 3, 4);
  c = mx_e_w4(c, 5, 6, 7, 8);
  c = mx_e_end(c, 0, mx_vif_mscal(0), mx_vif_flushe());
  same(c, n);
}

static void packed_vertex(void) {
  /* PACKED XYZ2: X 0-15, Y 32-47, Z 64-95, ADC bit 111 */
  u64 *c = mx_e_xyz2(a, 0x8010, 0x7ff0, 0x123, 1);
  CHECK_EQ(c - a, 2);
  CHECK_EQ(a[0], 0x8010ull | 0x7ff0ull << 32);
  CHECK_EQ(a[1], 0x123ull | 1ull << 47);
  mx_e_rgbaq(a, 1, 2, 3, 4);
  CHECK_EQ(a[0], 1 | 2ull << 32);
  CHECK_EQ(a[1], 3 | 4ull << 32);
}

void test_emit(void) {
  ad_packet();
  vif_chain();
  packed_vertex();
}
