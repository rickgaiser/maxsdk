/* All PS2 tests, probes and demos in one ELF, for one run on the PS2. Each
   part prints its own results; the end marker's status is the number of
   parts that failed. */

#include <mx/mx.h>

int dma_main(void);
int gs_main(void);
int vif_main(void);
int vu_main(void);
int p2_dma_main(void);
int p3_gs_main(void);
int p4_vu_main(void);
int spin_main(void);
int dbit_main(void);

static const struct {
  const char *name;
  int (*run)(void);
} parts[] = {
    {"dma", dma_main},       {"gs", gs_main},         {"vif", vif_main},
    {"vu", vu_main},         {"p2_dma", p2_dma_main}, {"p3_gs", p3_gs_main},
    {"p4_vu", p4_vu_main},   {"spin", spin_main},
    /* last: an earlier version of the D-bit check hung the PS2 */
    {"dbit", dbit_main},
};

int main(void) {
  int failed = 0;
  for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
    mx_printf("==== %s\n", parts[i].name);
    int r = parts[i].run();
    mx_printf("==== %s: %s\n", parts[i].name, r ? "FAILED" : "ok");
    failed += r != 0;
  }
  mx_printf("ps2batch: %d of %d parts failed\n", failed,
            (int)(sizeof parts / sizeof parts[0]));
  mx_printf("@@PS2TEST_END@@ status=%d\n", failed);
  return failed;
}
