#ifndef MX_IOP_H
#define MX_IOP_H

/* IOP memory, modules and the patches of the ROM modules (ps2sdk's sbv), all
   from the EE: IOP RAM is read over SIF (RPC RDATA) and written by SIF DMA.
   There is still no IOP code in maxsdk. Not for interrupt handlers, nor for
   two threads at once. */

#include <mx/types.h>

/* Copy size bytes of IOP RAM at iop (any alignment) to dst. */
int mx_iop_read(u32 iop, void *dst, u32 size);
/* Copy size bytes from src to IOP RAM at iop: both 16-byte aligned, size a
   multiple of 16 (SIF DMA). */
int mx_iop_write(u32 iop, const void *src, u32 size);
/* Write one word (4-byte aligned) of IOP RAM: the 16 bytes around it are read
   and written back. */
int mx_iop_write_word(u32 iop, u32 value);

/* The IOP heap (RPC 0x80000003, served by FILEIO). 0 / < 0 on failure. */
u32 mx_iop_alloc(u32 size);
int mx_iop_free(u32 iop);

/* LOADCORE's record of a loaded module (ModuleInfo). */
struct mx_iop_module {
  u32 next; /* the next record, 0 at the end */
  u32 name; /* IOP address of the name */
  u16 version, flags, id, unused;
  u32 entry, gp, text_start, text_size, data_size, bss_size, unused1, unused2;
};

/* The module named name (exactly): returns its id and its record in *m, 0 if
   it is not loaded. */
int mx_iop_find_module(const char *name, struct mx_iop_module *m);
/* The export library named name (e.g. "modload"): its first max function
   addresses go in exports (if exports); returns how many it has, 0 if none. */
int mx_iop_find_exports(const char *name, u32 *exports, int max);

/* The patches. Each returns 0 when it patched, 1 when the patch is there
   already (ps2link applies the LMB and prefix check ones itself, after its
   IOP reset) or the module does not need it, < 0 on an error. */

/* rom0:FILEIO 1.1: getstat and dread call sceSifSetDma with interrupts on
   (a race with the other SIF DMA of the IOP, e.g. PADMAN's), and remove goes
   on into mkdir. mx_fio_dopen applies it the first time. */
int mx_iop_patch_fileio(void);
/* rom0:LOADFILE has no LoadModuleBuffer (function 6): add it. */
int mx_iop_patch_lmb(void);
/* MODLOAD loads unprotected modules and ELFs only from some devices: from
   all of them. */
int mx_iop_patch_prefix_check(void);

/* Load and start the IRX module of size bytes at buf (EE memory; it is copied
   into the IOP heap, the copy freed after). Applies mx_iop_patch_lmb. Returns
   the module id (< 0 on failure); its start result goes in *modres (if
   modres). */
int mx_iop_load_module_buffer(const void *buf, u32 size, const char *args, u32 args_len,
                              int *modres);

#endif
