#ifndef MX_FRAME_H
#define MX_FRAME_H

/* The render pipeline (phase 6, decisions D14/D15). A frame is a stream of
   VIF1 packets: GS writes by DIRECT (PATH2) and VU1 batches that send their
   GIF packets with XGKICK (PATH1). The EE builds the stream with cursor
   emitters in two scratchpad halves and pushes each full half into an
   MFIFO ring (fromSPR); VIF1 drains the ring all the time, so the EE never
   starts or waits for a DMA itself. Textures go over PATH3 (GIF channel).

   Every frame ends with FLUSH + FINISH. Its FINISH interrupt marks it done,
   and the vblank handler shows it (mx_gs). Two framebuffers: frame n draws
   into the buffer of frame n - 2, so mx_frame_begin waits until frame n - 1
   is on screen.

   Ownership: data referenced by the stream (vertex arrays, textures) must
   not change until the frame that uses it has passed mx_frame_done(). The
   stream itself (headers, GIF tags) is copied into the ring. */

#include <mx/emit.h>
#include <mx/gs.h>
#include <mx/types.h>
#include <mx/vu.h>

struct mx_frame_stats {
  u32 pushes;      /* halves pushed into the ring */
  u32 wait_ring;   /* EE cycles waiting for ring space */
  u32 wait_buffer; /* EE cycles in mx_frame_begin waiting for the back buffer */
};

extern struct mx_frame_stats mx_frame_stats;

/* Start the pipeline (after mx_gs_init and after loading VU1 microcode
   with mx_vu_load, or load it later with mx_frame_mpg). ring: the MFIFO
   ring (power-of-two size, aligned to it); spr, spr_bytes: the scratchpad
   area for the two halves (at most 8 KiB each). VIF1 belongs to the
   pipeline until mx_frame_stop(). */
void mx_frame_init(void *ring, u32 size, void *spr, u32 spr_bytes);
void mx_frame_stop(void);

/* Begin frame n: waits until its framebuffer is free, then appends the
   drawing environment for it and a clear. Returns the framebuffer (0/1). */
int mx_frame_begin(u32 r, u32 g, u32 b);
/* vsync off: frame n only waits for frame n - 2 to be drawn, not for frame
   n - 1 to be shown, so frames are drawn as fast as the units allow (for
   throughput measurements). Each frame is shown at its FINISH (the display
   tears), before the GS starts on the next one, which draws into the other
   buffer. On by default. */
void mx_frame_vsync(int on);
/* Frame n is ready to be built (mx_frame_begin would not wait). */
int mx_frame_ready(void);
/* Frames whose FINISH has come; frame f is done when mx_frame_done() > f. */
MX_INLINE u32 mx_frame_done(void) { return mx_gs.frames_done; }
/* The frame being built. */
MX_INLINE u32 mx_frame_number(void) { return mx_gs.frames; }

/* Room for qwc quadwords of whole DMA packets (tags with their cnt data)
   in the current half; returns the cursor. qwc <= half / 16. */
u64 *mx_frame_reserve(u32 qwc);
void mx_frame_commit(u64 *end);

/* GS writes over PATH2: returns a cursor for qwc quadwords of GIF packets
   (tags included); commit them with mx_frame_commit. */
u64 *mx_frame_direct(u32 qwc);
/* Wait (in the stream) until PATH3 is idle: textures sent before are in
   GS memory. */
void mx_frame_wait_path3(void);

/* ---- VU1 batches ------------------------------------------------------------ */

/* Set up VIF1 double buffering for a render kernel (BASE = k->data_qw,
   OFFSET = half the rest). Batches then alternate between the buffers. */
void mx_frame_vu1_setup(const struct mx_kernel *k);
/* Upload microcode in the stream (after a FLUSHE), unless it is resident. */
void mx_frame_mpg(const void *code, u32 bytes, u32 addr);
/* The resident header (hdr_qw quadwords at address 0), after a FLUSHE. */
void mx_frame_vu1_header(const void *hdr, u32 hdr_qw);
/* One batch into the next buffer: the GIF tag at quadword 0 (NLOOP = n),
   uni_qw quadwords of constants (copied into the stream) from 1, and n
   items of k->item_qw quadwords from verts (in memory) after them; then
   MSCAL. The kernel writes its output (k->out_qw per item) after the
   items. */
void mx_frame_vu1_batch(const struct mx_kernel *k, u64 giftag, u64 regs, const void *uni,
                        u32 uni_qw, const void *verts, u32 n);
/* Items per batch that fit a buffer of kernel k with uni_qw constants and
   its output. */
u32 mx_frame_vu1_max_items(const struct mx_kernel *k, u32 uni_qw);

/* VU1 compute between render passes (D16 b): once the batches before it
   are drawn (FLUSH), upload k's code if it is not resident, run it on n
   items at data (in memory) with the header hdr (k's single-buffer layout,
   items transformed in place), and stop the stream at a VIF i-bit until the
   EE has read the results into dst (toSPR through spr, spr_bytes; the EE
   copies them out). Blocks the EE until then; the GS idles meanwhile. k's
   code must not overlap the render kernel's. Returns 0, or -1 on timeout. */
int mx_frame_vu1_compute(const struct mx_kernel *k, const void *hdr, const void *data, u32 n,
                         void *dst, void *spr, u32 spr_bytes);

/* End the frame: FLUSH (VU1 and PATH1 done), FINISH, push the stream. */
void mx_frame_end(void);

/* ---- shaders (HLSL vertex programs, PLAN_PROCGEN) ---------------------------- */

/* A VU1 render program compiled from an HLSL vertex shader (include/vu.hlsli):
   per vertex it reads one quadword per input and writes nreg GIF registers.
   Each input is its own stream in memory, in its UNPACK format, so position,
   normal and colour can each take as few bytes as they need. */
struct mx_shader_in {
  u8 fmt;   /* UNPACK vn << 2 | vl (MX_UNPACK_*) */
  u8 usn;   /* zero-extend */
  u8 bytes; /* per vertex in memory */
};
#define MX_SHADER_MAX_IN 8
struct mx_shader {
  const void *code;
  u32 code_bytes;
  u32 entry;   /* 64-bit units */
  u32 buf_qw;  /* VU1 double buffer (VIF1 BASE 0, OFFSET buf_qw) */
  u32 uni_qw;  /* uniforms after the GIF tag */
  u32 item_qw; /* = number of inputs */
  u32 out_qw;  /* = nreg */
  u32 max_n;   /* vertices per batch */
  u32 align;   /* batches start at multiples of it, and up to align - 1 more
                  vertices are read after the last one */
  u32 nreg;
  u64 regs;    /* GIF register list */
  u32 nin;
  struct mx_shader_in in[MX_SHADER_MAX_IN];
  u32 in_at;    /* the first input in a buffer */
  u32 out_at;   /* the GIF packet: in a buffer; indexed, in data memory */
  /* indexed programs (max_i != 0): MX_VU_ISHADER */
  u32 max_i;    /* indices per batch */
  u32 idx_at;   /* the indices in a buffer */
  u32 const_at; /* the two uniform blocks, uni_qw apart */
};

/* From the layout header of vertex program NAME (llc's section .vu.header)
   and its code symbols. */
#define MX_VU_SHADER(NAME, code_, code_end_)                                                       \
  ((struct mx_shader){.code = (code_),                                                             \
                      .code_bytes = (u32)((code_end_) - (code_)),                                  \
                      .entry = VU_##NAME##_ENTRY,                                                  \
                      .buf_qw = VU_##NAME##_BUF_QW,                                                \
                      .uni_qw = VU_##NAME##_UNI_QW,                                                \
                      .item_qw = VU_##NAME##_ITEM_QW,                                              \
                      .out_qw = VU_##NAME##_OUT_QW,                                                \
                      .max_n = VU_##NAME##_MAX_N,                                                  \
                      .align = VU_##NAME##_ALIGN,                                                  \
                      .nreg = VU_##NAME##_NREG,                                                    \
                      .regs = VU_##NAME##_REGS,                                                    \
                      .nin = VU_##NAME##_NIN,                                                      \
                      .in = VU_##NAME##_INPUTS,                                                    \
                      .in_at = VU_##NAME##_IN_AT,                                                  \
                      .out_at = VU_##NAME##_OUT_AT})

/* An indexed vertex program (built with opt -vu-indexed=N: the HLSL_IX list
   of the Makefile, docs/hlsl.md). */
#define MX_VU_ISHADER(NAME, code_, code_end_)                                                      \
  ((struct mx_shader){.code = (code_),                                                             \
                      .code_bytes = (u32)((code_end_) - (code_)),                                  \
                      .entry = VU_##NAME##_ENTRY,                                                  \
                      .buf_qw = VU_##NAME##_BUF_QW,                                                \
                      .uni_qw = VU_##NAME##_UNI_QW,                                                \
                      .item_qw = VU_##NAME##_ITEM_QW,                                              \
                      .out_qw = VU_##NAME##_OUT_QW,                                                \
                      .max_n = VU_##NAME##_MAX_N,                                                  \
                      .align = VU_##NAME##_ALIGN,                                                  \
                      .nreg = VU_##NAME##_NREG,                                                    \
                      .regs = VU_##NAME##_REGS,                                                    \
                      .nin = VU_##NAME##_NIN,                                                      \
                      .in = VU_##NAME##_INPUTS,                                                    \
                      .in_at = VU_##NAME##_IN_AT,                                                  \
                      .out_at = VU_##NAME##_OUT_AT,                                                \
                      .max_i = VU_##NAME##_MAX_I,                                                  \
                      .idx_at = VU_##NAME##_IDX_AT,                                                \
                      .const_at = VU_##NAME##_CONST_AT})

/* Quadwords of VIF packets that mx_shader_e_batch writes (the input data is
   referenced, not copied). */
MX_INLINE u32 mx_shader_batch_qw(const struct mx_shader *s) { return 3 + s->uni_qw + s->nin; }
/* One batch of n <= s->max_n vertices: the GIF tag (PRIM prim, NLOOP n, EOP)
   and the uniforms into the next VU1 buffer (UNPACK with FLG), then each
   input from streams[i] (vertex 0 of the batch, quadword aligned; n rounded
   up to s->align vertices are read), then MSCAL. Returns the cursor. */
u64 *mx_shader_e_batch(u64 *c, const struct mx_shader *s, u32 prim, const void *uni,
                       const void *const *streams, u32 n);

/* Set up VIF1 double buffering for s (BASE 0, OFFSET s->buf_qw) and upload
   its code unless it is resident. Waits (in the stream) until the batches
   before it are drawn (FLUSH): s may use the memory where the GIF still
   reads the packets of another program. */
void mx_frame_shader_setup(const struct mx_shader *s);
/* Draw n vertices of streams with s in the current frame, as one primitive
   (MX_GS_PRIM_TRISTRIP: batches overlap by 2 vertices; TRIANGLE: batches of
   whole triangles; anything else: batches of any size). prim is the PRIM
   register value (shading, texture, ...). The streams need room for
   s->align - 1 vertices after the last one. */
void mx_frame_shader_draw(const struct mx_shader *s, u32 prim, const void *uni,
                          const void *const *streams, u32 n);

/* ---- indexed shaders ----------------------------------------------------------- */

/* A batch of an indexed program: nv <= s->max_n vertices from vertex first of
   the streams (a multiple of s->align; nv rounded up to it are read), and
   ni <= s->max_i indices at idx (quadword aligned, read in whole quadwords
   of 16). An index is the vertex's number in the batch times s->out_qw.
   Strips restart by repeating vertices; the primitive is any. */
struct mx_ibatch {
  u32 first;
  u16 nv, ni;
  const u8 *idx;
};

/* Quadwords of VIF packets that mx_shader_e_uniforms and mx_shader_e_ibatch
   write. */
MX_INLINE u32 mx_shader_uniforms_qw(const struct mx_shader *s) { return 1 + s->uni_qw; }
MX_INLINE u32 mx_shader_ibatch_qw(const struct mx_shader *s) { return 5 + s->nin; }
/* The uniforms of a draw into the block at VU address block (s->const_at, or
   s->const_at + s->uni_qw: by turns, one per draw that has batches). */
u64 *mx_shader_e_uniforms(u64 *c, const struct mx_shader *s, u32 block, const void *uni);
/* One batch with the uniforms of block: the GIF tag (PRIM prim, NLOOP
   b->ni) and the counts, the inputs, the indices, MSCAL. */
u64 *mx_shader_e_ibatch(u64 *c, const struct mx_shader *s, u32 prim, u32 block,
                        const void *const *streams, const struct mx_ibatch *b);
/* Draw n batches of an indexed program with the uniforms uni (copied into
   the stream once). */
void mx_frame_shader_idraw(const struct mx_shader *s, u32 prim, const void *uni,
                           const void *const *streams, const struct mx_ibatch *b, u32 n);

/* ---- textures (PATH3) -------------------------------------------------------- */

/* Upload w x h pixels at data (in memory) to dbp by the GIF channel, now,
   without waiting. The chain is built in the slot's own buffer. Returns 0,
   or -1 if the slot's previous upload is still running. */
int mx_frame_texture(int slot, u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, const void *data);

#endif
