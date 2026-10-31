/* IOP modules and the patches of the ROM modules, after ps2sdk's sbv
   (ee/sbv): LOADCORE's tables and the code of FILEIO, LOADFILE and MODLOAD
   are read over SIF (mx_iop_read) and changed by SIF DMA (mx_iop_write). The
   offsets and the code put into the IOP are sbv's. */

#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/string.h>

#define JAL(a) (0x0c000000u | (0x3ffffffu & ((a) >> 2)))
#define J(a) (0x08000000u | (0x3ffffffu & ((a) >> 2)))
#define IS_JAL(w) (((w) >> 26) == 3)
#define JAL_TARGET(w, pc) (((pc) & 0xF0000000u) | (((w) & 0x3ffffffu) << 2))

/* ---- the IOP heap --------------------------------------------------------- */

#define HEAP_SID 0x80000003u

static struct mx_sif_client heap;
static u32 heap_arg[4] MX_ALIGNED(64);

static int heap_call(int fno, u32 *v) {
  if (!heap.server && (mx_sif_init() < 0 || mx_sif_bind(&heap, HEAP_SID) < 0))
    return -1;
  volatile u32 *a = MX_UNCACHED(heap_arg);
  a[0] = *v;
  if (mx_sif_call(&heap, fno, (void *)a, 16, (void *)a, 4) < 0)
    return -1;
  *v = a[0];
  return 0;
}

u32 mx_iop_alloc(u32 size) {
  u32 v = size;
  return heap_call(1, &v) < 0 ? 0 : v;
}

int mx_iop_free(u32 iop) {
  u32 v = iop;
  return heap_call(2, &v) < 0 ? -1 : (int)v;
}

/* Copy n bytes (any alignment) to IOP memory at iop (16-byte aligned), by a
   bounce buffer; the last block is padded with zeros. */
static int iop_copy(u32 iop, const void *src, u32 n) {
  static u8 bounce[2048] MX_ALIGNED(64);
  const u8 *s = src;
  while (n) {
    u32 len = n < sizeof bounce ? n : sizeof bounce;
    u32 padded = (len + 15) & ~15u;
    u8 *b = MX_UNCACHED(bounce);
    for (u32 i = 0; i < len; i++)
      b[i] = s[i];
    for (u32 i = len; i < padded; i++)
      b[i] = 0;
    if (mx_iop_write(iop, b, padded) < 0)
      return -1;
    iop += len;
    s += len;
    n -= len;
  }
  return 0;
}

/* Write n words at iop (4-byte aligned, within 32 bytes from its 16-byte
   block) in one DMA, so the IOP never runs half of them. */
static int iop_write_words(u32 iop, const u32 *w, u32 n) {
  static u32 blk[8] MX_ALIGNED(64);
  u32 base = iop & ~15u, first = (iop - base) / 4;
  if ((iop & 3) || first + n > 8)
    return -1;
  u32 size = first + n > 4 ? 32 : 16;
  u32 tmp[8];
  if (mx_iop_read(base, tmp, size) < 0)
    return -1;
  for (u32 i = 0; i < n; i++)
    tmp[first + i] = w[i];
  volatile u32 *b = MX_UNCACHED(blk);
  for (u32 i = 0; i < size / 4; i++)
    b[i] = tmp[i];
  return mx_iop_write(base, (const void *)b, size);
}

/* ---- modules and export libraries ---------------------------------------- */

/* LOADCORE's list of modules starts with SYSMEM's record at 0x800. */
#define FIRST_MODULE 0x800u
#define MAX_LIST 256

int mx_iop_find_module(const char *name, struct mx_iop_module *m) {
  u32 len = strlen(name);
  char buf[64];
  if (len >= sizeof buf)
    return 0;
  u32 a = FIRST_MODULE;
  for (int k = 0; a && k < MAX_LIST; k++, a = m->next) {
    if (mx_iop_read(a, m, sizeof *m) < 0)
      return 0;
    if (!m->name || mx_iop_read(m->name, buf, len + 1) < 0)
      continue;
    if (!memcmp(buf, name, len + 1))
      return m->id;
  }
  return 0;
}

/* An export library: its header, then the function addresses up to a 0. */
struct exp_lib {
  u32 prev, caller;
  u16 version, flags;
  char name[8];
};

/* The address of LOADCORE's pair (tail, head) of the export library list:
   LOADCORE's own export library sits at the end of its .text (SYSMEM's, at
   0x830, before it), and its function 3 (GetLoadcoreInternalData) is
   lui v0, hi; addiu v0, v0, lo of the pair. */
static u32 lib_list;

static u32 find_lib_list(void) {
  if (lib_list)
    return lib_list;
  u32 next;
  struct mx_iop_module core;
  if (mx_iop_read(FIRST_MODULE, &next, 4) < 0 || !next || mx_iop_read(next, &core, sizeof core) < 0)
    return 0;
  u32 end = core.text_start + core.text_size;
  static u32 t[128];
  if (mx_iop_read(end - sizeof t, t, sizeof t) < 0)
    return 0;
  u32 i = 0;
  for (; i < 128 - 5; i++)
    if (t[i] == 0x830 && !memcmp(&t[i + 3], "loadcore", 8))
      break;
  if (i >= 128 - 5)
    return 0;
  u32 fn, code[2];
  if (mx_iop_read(end - sizeof t + i * 4 + sizeof(struct exp_lib) + 3 * 4, &fn, 4) < 0 ||
      mx_iop_read(fn, code, 8) < 0)
    return 0;
  if ((code[0] & 0xffff0000u) != 0x3c020000u || (code[1] & 0xffff0000u) != 0x24420000u)
    return 0;
  lib_list = ((code[0] & 0xffff) << 16) + (u32)(s32)(s16)(code[1] & 0xffff);
  return lib_list;
}

int mx_iop_find_exports(const char *name, u32 *exports, int max) {
  u32 len = strlen(name), list[2];
  u32 l = find_lib_list();
  if (len > 8 || !l || mx_iop_read(l, list, 8) < 0)
    return 0;
  u32 a = list[0]; /* the tail; then back by prev */
  for (int k = 0; a && k < MAX_LIST; k++) {
    struct exp_lib h;
    if (mx_iop_read(a, &h, sizeof h) < 0)
      return 0;
    if (!memcmp(h.name, name, len) && (len == 8 || !h.name[len])) {
      u32 f[64];
      int count = 0;
      for (u32 at = a + sizeof h;; at += sizeof f) {
        if (mx_iop_read(at, f, sizeof f) < 0)
          return 0;
        for (u32 i = 0; i < 64; i++, count++) {
          if (!f[i])
            return count;
          if (exports && count < max)
            exports[count] = f[i];
        }
        if (count >= 1024)
          return 0;
      }
    }
    a = h.prev;
  }
  return 0;
}

/* ---- FILEIO --------------------------------------------------------------- */

int mx_iop_patch_fileio(void) {
  /* At FILEIO 1.1's .text + 0x738 fio_remove, + 0xce8 the end of the RPC
     handler, + 0x108c CpuSuspendIntr, + 0x1094 CpuResumeIntr, + 0x10c0
     sceSifSetDma. remove (at + 0xbb8) jumps to the first part, getstat and
     dread (+ 0x9cc, + 0xa58) call the second instead of sceSifSetDma. */
  static const u32 code[20] = {
      0x0c0001ce, /* jal fio_remove */
      0x00000000, /* nop */
      0x0800033a, /* j the end of the handler */
      0x00000000, /* nop */
      0x27bdfff0, /* addiu sp, sp, -16 */
      0xafbf0000, /* sw ra, 0(sp) */
      0xafa40004, /* sw a0, 4(sp) */
      0xafa50008, /* sw a1, 8(sp) */
      0x0c000423, /* jal CpuSuspendIntr */
      0x27a4000c, /* addiu a0, sp, 12 */
      0x8fa40004, /* lw a0, 4(sp) */
      0x0c000430, /* jal sceSifSetDma */
      0x8fa50008, /* lw a1, 8(sp) */
      0x8fa4000c, /* lw a0, 12(sp) */
      0x0c000425, /* jal CpuResumeIntr */
      0xafa20004, /* sw v0, 4(sp) */
      0x8fbf0000, /* lw ra, 0(sp) */
      0x8fa20004, /* lw v0, 4(sp) */
      0x03e00008, /* jr ra */
      0x27bd0010, /* addiu sp, sp, 16 */
  };
  static u32 stub[20] MX_ALIGNED(64);
  struct mx_iop_module m;
  if (!mx_iop_find_module("FILEIO_service", &m) || m.version != 0x101)
    return 1;
  u32 t = m.text_start, rm, gs, dr;
  if (mx_iop_read(t + 0xbb8, &rm, 4) < 0 || mx_iop_read(t + 0x9cc, &gs, 4) < 0 ||
      mx_iop_read(t + 0xa58, &dr, 4) < 0)
    return -1;
  if (IS_JAL(gs) && JAL_TARGET(gs, t) - t >= m.text_size)
    return 1; /* getstat calls outside FILEIO: patched already */
  if (gs != JAL(t + 0x10c0) || dr != JAL(t + 0x10c0) || !IS_JAL(rm))
    return -2; /* not the code the patch is for */

  u32 p = mx_iop_alloc(sizeof stub);
  if (!p)
    return -1;
  volatile u32 *s = MX_UNCACHED(stub);
  for (int i = 0; i < 20; i++)
    s[i] = code[i];
  s[0] += t >> 2;
  s[2] += t >> 2;
  s[8] += t >> 2;
  s[11] += t >> 2;
  s[14] += t >> 2;
  if (mx_iop_write(p, (const void *)s, sizeof stub) < 0)
    return -1;
  if (mx_iop_write_word(t + 0xbb8, J(p)) < 0 || mx_iop_write_word(t + 0x9cc, JAL(p + 16)) < 0 ||
      mx_iop_write_word(t + 0xa58, JAL(p + 16)) < 0)
    return -1;
  return 0;
}

/* ---- LOADFILE: LoadModuleBuffer ------------------------------------------ */

int mx_iop_patch_lmb(void) {
  /* LOADFILE's function 6: a0 the RPC argument (the module's IOP address,
     the length of its arguments, the path field, the arguments at + 0x104);
     LoadModuleBuffer, then StartModule; returns (id, its start result). The
     bracketed fields are filled in. */
  static const u32 code[24] = {
      0x27bdffd8, /* addiu sp, -40 */
      0xafb00018, /* sw s0, 0x18(sp) */
      0xafbf0020, /* sw ra, 0x20(sp) */
      0x00808021, /* move s0, a0 */
      0x8c840000, /* lw a0, 0(a0) */
      0x0c000000, /* jal [LoadModuleBuffer] */
      0xafb1001c, /* sw s1, 0x1c(sp) */
      0x3c110000, /* lui s1, [hi(result)] */
      0x04400008, /* bltz v0, 1f */
      0x36310000, /* ori s1, [lo(result)] */
      0x00402021, /* move a0, v0 */
      0x26250008, /* addiu a1, s1, 8 */
      0x8e060004, /* lw a2, 4(s0) */
      0x26070104, /* addiu a3, s0, 0x104 */
      0x26280004, /* addiu t0, s1, 4 */
      0x0c000000, /* jal [StartModule] */
      0xafa80010, /* sw t0, 0x10(sp) */
      0xae220000, /* 1: sw v0, 0(s1) */
      0x02201021, /* move v0, s1 */
      0x8fbf0020, /* lw ra, 0x20(sp) */
      0x8fb1001c, /* lw s1, 0x1c(sp) */
      0x8fb00018, /* lw s0, 0x18(sp) */
      0x03e00008, /* jr ra */
      0x27bd0028, /* addiu sp, 40 */
  };
  static u32 stub[32] MX_ALIGNED(64); /* the code, then the result at + 96 */
  u32 ex[16];
  if (mx_iop_find_exports("modload", ex, 16) < 16)
    return -1;
  struct mx_iop_module m;
  if (!mx_iop_find_module("LoadModuleByEE", &m))
    return -1;
  /* The RPC dispatcher at + 0x4c4 (the same LOADFILE in all boot ROMs):
     addiu sp, -24; sltiu v0, a0, 6; bnez v0; sw ra, 0x10(sp); ...; the jump
     table's address in words 7 and 9. */
  u32 d[10], disp = m.text_start + 0x4c4;
  if (m.text_size < 0x4c4 + sizeof d)
    return 1;
  if (mx_iop_read(disp, d, sizeof d) < 0)
    return -1;
  if (d[0] != 0x27bdffe8 || d[2] != 0x14400003 || d[3] != 0xafbf0010 || d[5] != 0x00001021 ||
      d[6] != 0x00041080)
    return 1; /* another LOADFILE */
  if (d[1] == 0x2c820007)
    return 1; /* patched already (ps2link does) */
  if (d[1] != 0x2c820006)
    return 1;
  u32 table_end = ((d[7] & 0xffff) << 16) + (u32)(s32)(s16)(d[9] & 0xffff) + 0x18;

  u32 p = mx_iop_alloc(sizeof stub);
  if (!p)
    return -1;
  u32 result = p + 96;
  volatile u32 *s = MX_UNCACHED(stub);
  for (int i = 0; i < 32; i++)
    s[i] = i < 24 ? code[i] : 0;
  s[5] = JAL(ex[10]);
  s[7] = 0x3c110000u | (result >> 16);
  s[9] = 0x36310000u | (result & 0xffff);
  s[15] = JAL(ex[8]);
  if (mx_iop_write(p, (const void *)s, sizeof stub) < 0)
    return -1;
  /* entry 6 of the jump table, then let function 6 through */
  if (mx_iop_write_word(table_end, p) < 0 || mx_iop_write_word(disp + 4, 0x2c820007) < 0)
    return -1;
  return 0;
}

#define LOADFILE_SID 0x80000006u
#define LOADFILE_BUFFER 6

static struct mx_sif_client lf;
static struct {
  u32 ptr;      /* in: the module; out: the result */
  u32 args_len; /* out: the start result */
  char path[252];
  char args[252];
} lmb_arg MX_ALIGNED(64);
static s32 lmb_result[4] MX_ALIGNED(64);

int mx_iop_load_module_buffer(const void *buf, u32 size, const char *args, u32 args_len,
                              int *modres) {
  if (mx_iop_patch_lmb() < 0)
    return -1;
  if (!lf.server && (mx_sif_init() < 0 || mx_sif_bind(&lf, LOADFILE_SID) < 0))
    return -1;
  u32 p = mx_iop_alloc((size + 15) & ~15u);
  if (!p)
    return -1;
  int r = -1;
  if (iop_copy(p, buf, size) == 0) {
    __typeof__(lmb_arg) *a = MX_UNCACHED(&lmb_arg);
    if (args_len > sizeof a->args)
      args_len = sizeof a->args;
    a->ptr = p;
    a->args_len = args_len;
    for (u32 i = 0; i < args_len; i++)
      a->args[i] = args[i];
    volatile s32 *res = MX_UNCACHED(lmb_result);
    if (mx_sif_call(&lf, LOADFILE_BUFFER, a, sizeof *a, (void *)res, 8) == 0) {
      r = res[0];
      if (modres)
        *modres = res[1];
    }
  }
  mx_iop_free(p);
  return r;
}

/* ---- MODLOAD: the device check -------------------------------------------- */

int mx_iop_patch_prefix_check(void) {
  static const u32 ret0[2] = {0x03e00008, 0x00001021}; /* jr ra; move v0, zero */
  u32 ex[16], cur[2];
  if (mx_iop_find_exports("modload", ex, 16) < 16)
    return -1;
  if (mx_iop_read(ex[15], cur, 8) < 0)
    return -1;
  if (cur[0] == ret0[0] && cur[1] == ret0[1])
    return 1;
  return iop_write_words(ex[15], ret0, 2) < 0 ? -1 : 0;
}
