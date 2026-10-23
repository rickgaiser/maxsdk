/* CPU exceptions: report where the program faulted, then end it.
   Without this, the kernel or the loader shows the exception on the screen
   only, and a test run looks like a hang. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/mx.h>

/* Filled in by mx__exception_vector (exception.S): the GPRs, 128 bits each,
   then Status, Cause, EPC, BadVAddr and BadPAddr. */
struct mx_exception_state {
  u64 gpr[32][2];
  u32 status, cause, epc, badvaddr, badpaddr;
};

struct mx_exception_state mx__exception_state MX_ALIGNED(16);
u8 mx__exception_stack[8192] MX_ALIGNED(16);

void mx__exception_vector(void);
void mx__exception_report(void) __attribute__((noreturn, used));

static const char *const names[14] = {
    "interrupt",         "TLB modified",        "TLB miss (load)",   "TLB miss (store)",
    "address error (load)", "address error (store)", "bus error (fetch)", "bus error (data)",
    "syscall",           "breakpoint",          "reserved instruction",
    "coprocessor unusable", "overflow",          "trap"};

static const char *const reg_names[32] = {
    "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

void mx__exception_report(void) {
  const struct mx_exception_state *s = &mx__exception_state;
  u32 code = s->cause >> 2 & 31;
  mx_ei();
  mx_debug_hold(0);
  mx_printf("\nEXCEPTION %u (%s)%s: EPC %08x BadVAddr %08x BadPAddr %08x Status %08x Cause "
            "%08x\n",
            code, code < 14 ? names[code] : "?", s->cause >> 31 ? " in a branch delay slot" : "",
            s->epc, s->badvaddr, s->badpaddr, s->status, s->cause);
  for (int i = 0; i < 32; i += 4)
    mx_printf("  %s %08x  %s %08x  %s %08x  %s %08x\n", reg_names[i], (u32)s->gpr[i][0],
              reg_names[i + 1], (u32)s->gpr[i + 1][0], reg_names[i + 2], (u32)s->gpr[i + 2][0],
              reg_names[i + 3], (u32)s->gpr[i + 3][0]);
  MX_TEST_END(255);
  mx_exit(255);
}

void mx_exception_init(void) {
  for (int c = 1; c <= 3; c++)
    mx_bios_set_tlb_handler(c, mx__exception_vector);
  static const u8 common[] = {4, 5, 6, 7, 9, 10, 12, 13};
  for (u32 i = 0; i < sizeof common; i++)
    mx_bios_set_common_handler(common[i], mx__exception_vector);
}
