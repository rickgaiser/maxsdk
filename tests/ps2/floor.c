/* vuk's floor (tools/vuk) on VU0 against the exact floor, for values with
   every kind of fraction, of both signs, up to 2^21. The PS2's VU (unlike
   PCSX2) cuts the smaller operand of an add to one bit below the sum's last,
   which the magic-constant floor must correct. */

#include "t.h"
#include "floor.h"

extern const u8 floor_vu[], floor_vu_end[];

#define NS 2048
static float in[NS][4] MX_ALIGNED(64);
static float vu[NS][4] MX_ALIGNED(64);
static u32 hdr[8] MX_ALIGNED(16);

static float floor_(float v) {
  s32 i = (s32)v;
  return (float)i > v ? (float)(i - 1) : (float)i;
}

int main(void) {
  struct mx_kernel k = MX_VUK_KERNEL(FL, 0, floor_vu, floor_vu_end);
  mx_vu_reset(1);
  mx_vu_load(0, k.code, k.code_bytes, 0);
  u32 seed = 3;
  for (int i = 0; i < NS; i++) {
    seed = seed * 1103515245u + 12345u;
    /* a magnitude of 2^0..2^20 and a fraction of any size */
    float m = (float)(s32)(1u << (seed >> 27 & 15)) + (float)(s32)(seed >> 8 & 1023) / 1024.0f;
    seed = seed * 1103515245u + 12345u;
    in[i][0] = m, in[i][1] = (float)(s32)(seed >> 8 & 0xFFFF) / 65536.0f;
    /* full fractions, as fbm's coordinates have them: 300 + i / 7 */
    in[i][2] = 300.5f + (float)i / 7.0f, in[i][3] = 0;
  }
  mx_dcache_writeback(in, sizeof in);
  static struct mx_vuq q;
  mx_vuq_init(&q, 0, 1);
  mx_vuq_readback_spr(&q, (void *)0x70000000, 0x2000);
  u32 per = mx_kernel_max_items(&k);
  for (u32 at = 0; at < NS; at += per) {
    u32 n = NS - at < per ? NS - at : per;
    mx_event e = mx_vuq_batch(&q, &k, hdr, in[at], n);
    CHECK(mx_vuq_wait(&q, e) == 0);
    mx_vuq_read(&q, &k, vu[at], n);
  }
  mx_vuq_release(&q);
  int bad = 0;
  for (int i = 0; i < NS; i++) {
    float want[4] = {floor_(in[i][0]), floor_(-in[i][0]), floor_(in[i][1]), floor_(in[i][2])};
    for (int f = 0; f < 4; f++)
      if (vu[i][f] != want[f] && bad++ < 8)
        mx_printf("floor: %d/1024 -> %d, expected %d (field %d)\n",
                  (s32)(in[i][f == 3 ? 2 : f / 2] * 1024),
                  (s32)vu[i][f], (s32)want[f], f);
  }
  CHECK(bad == 0);
  mx_printf("floor: %d of %d wrong\n", bad, 4 * NS);
  return t_done("floor");
}
