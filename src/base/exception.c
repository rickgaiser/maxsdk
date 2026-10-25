/* CPU exceptions: report where the program faulted, then end it.
   Without this, the kernel or the loader shows the exception on the screen
   only, and a test run looks like a hang. The "trace:" line lists the EPC,
   ra and likely return addresses from the stack, for tools/symbolize.py. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/mx.h>

/* Filled in by mx__exception_vector (exception.S): the GPRs, 128 bits each,
   then Status, Cause, EPC, BadVAddr and BadPAddr, HI and LO, and the FPU
   when it was usable. The offsets are used by exception.S. */
struct mx_exception_state {
  u64 gpr[32][2];
  u32 status, cause, epc, badvaddr, badpaddr, pad;
  u64 hi, lo;
  u32 fcr31, fpu_saved;
  u32 fpr[32];
};

_Static_assert(__builtin_offsetof(struct mx_exception_state, hi) == 536, "exception.S offsets");
_Static_assert(__builtin_offsetof(struct mx_exception_state, fpr) == 560, "exception.S offsets");

struct mx_exception_state mx__exception_state MX_ALIGNED(16);
u8 mx__exception_stack[8192] MX_ALIGNED(16);

extern const u32 __text_start[], __text_end[];

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

/* Can a word at addr be read without faulting again? Main RAM (and its
   kernel segment mirrors) or the scratchpad. */
static int readable(u32 addr) {
  if (addr & 3)
    return 0;
  if (addr >= 0x70000000 && addr < 0x70004000)
    return 1;
  u32 phys = addr >= 0x80000000 && addr < 0xc0000000 ? addr & 0x1fffffff : addr;
  return phys >= 0x00080000 && phys < 0x02000000;
}

/* FPU register i as a float. */
static double fpr(const struct mx_exception_state *s, int i) {
  float f;
  __builtin_memcpy(&f, &s->fpr[i], sizeof f);
  return f;
}

/* A return address: in .text, right after a jal or jalr and its delay slot. */
static int return_address(u32 addr) {
  if ((addr & 3) || addr < (u32)__text_start + 8 || addr > (u32)__text_end)
    return 0;
  u32 call = *(const u32 *)(addr - 8);
  return (call >> 26) == 3 || ((call >> 26) == 0 && (call & 0x3f) == 9);
}

void mx__exception_report(void) {
  const struct mx_exception_state *s = &mx__exception_state;
  u32 code = s->cause >> 2 & 31;
  u32 sp = (u32)s->gpr[29][0];
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
  mx_printf("  hi %016llx  lo %016llx\n", (unsigned long long)s->hi, (unsigned long long)s->lo);
  if (s->fpu_saved) {
    for (int i = 0; i < 32; i += 4)
      mx_printf("  f%-2d %g  f%-2d %g  f%-2d %g  f%-2d %g\n", i, fpr(s, i), i + 1, fpr(s, i + 1),
                i + 2, fpr(s, i + 2), i + 3, fpr(s, i + 3));
    mx_printf("  fcr31 %08x\n", s->fcr31);
  }

  /* The top of the stack, and the words in it that look like return
     addresses: without frame pointers, that is the best backtrace there is. */
  if (readable(sp)) {
    mx_printf("  stack at %08x:\n", sp);
    for (u32 a = sp; a < sp + 128 && readable(a); a += 16)
      mx_printf("    %08x: %08x %08x %08x %08x\n", a, ((const u32 *)a)[0], ((const u32 *)a)[1],
                ((const u32 *)a)[2], ((const u32 *)a)[3]);
  }
  mx_printf("  trace: %08x %08x", s->epc, (u32)s->gpr[31][0]);
  int found = 0;
  for (u32 a = sp; found < 16 && a < sp + 2048 && readable(a); a += 4) {
    u32 w = *(const u32 *)a;
    if (return_address(w)) {
      mx_printf(" %08x", w);
      found++;
    }
  }
  mx_printf("\n");
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
