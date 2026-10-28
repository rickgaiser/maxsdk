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

#include <mx/chain.h>
#include <mx/emit.h>
#include <mx/gs.h>
#include <mx/types.h>
#include <mx/vu.h>

struct mx_frame_stats {
  u32 pushes;      /* halves pushed into the ring (chain mode: copied into slots) */
  u32 qw;          /* quadwords of stream in them */
  u32 wait_ring;   /* EE cycles waiting for ring space */
  u32 wait_buffer; /* EE cycles in mx_frame_begin waiting for the back buffer */
  u32 idle;        /* EE cycles of the idle work (mx_frame_idle) in those waits */
};

extern struct mx_frame_stats mx_frame_stats;

/* Start the pipeline (after mx_gs_init and after loading VU1 microcode
   with mx_vu_load, or load it later with mx_frame_mpg). ring: the MFIFO
   ring (power-of-two size, aligned to it); spr, spr_bytes: the scratchpad
   area for the two halves (at most 8 KiB each). VIF1 belongs to the
   pipeline until mx_frame_stop(). */
void mx_frame_init(void *ring, u32 size, void *spr, u32 spr_bytes);
/* The same pipeline in chain mode: the stream may call chains in memory
   (mx_frame_call, mx_frame_shader_call), which an MFIFO drain cannot. Each
   half is copied by fromSPR into a slot of mem (slots of half the
   scratchpad area, at most MX_FRAME_SLOTS; mem quadword aligned, not
   written by the EE), and the slots go to VIF1 through a DMA queue. The
   copy of a half runs while the EE builds the next one. Returns 0, or -1 if
   mem holds fewer than two slots. */
#define MX_FRAME_SLOTS 16
int mx_frame_init_chain(void *mem, u32 bytes, void *spr, u32 spr_bytes);
void mx_frame_stop(void);
/* Work for the EE while it would wait for ring space or for the back
   buffer: fn is called until the wait is over or it returns 0 (nothing more
   to do now). Each call should be short, as the push waits for it. fn must
   not use the frame stream. 0: none. */
void mx_frame_idle(int (*fn)(void));

/* Begin frame n: waits until its buffers are free (those of display
   n % d, mx_gs_displays) and returns n. The stream has nothing of it yet:
   set a target (mx_frame_target) and clear it. */
u32 mx_frame_begin_n(void);
/* mx_frame_begin_n, then the drawing environment of mx_gs_init's framebuffer
   n % 2 in context 1 and a clear (r, g, b). Returns the framebuffer (0/1). */
int mx_frame_begin(u32 r, u32 g, u32 b);
/* Draw into target t with context ctx (0, 1) from here on: FLUSH (what came
   before is drawn into the old target first), then its FRAME, ZBUF,
   XYOFFSET and SCISSOR. Switching between the two contexts' targets by
   PRIM.CTXT needs neither. */
void mx_frame_target(const struct mx_rt *t, int ctx);
/* Before a target that was drawn into is read as a texture: FLUSH, then
   TEXFLUSH. */
void mx_frame_texflush(void);
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

/* A CALL of chain ch (a VIF1 chain that ends with ret: mx_chain_close) in
   the stream; chain mode only. The chain and what it references must not
   change until the frame has passed mx_frame_done(). */
void mx_frame_call(const struct mx_chain *ch);

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
/* Present mode, for one shown buffer: the frame was drawn into a target
   that is not shown (<mx/rt.h>), and what is built from here to
   mx_frame_end() copies it into the shown buffer (a sprite, a HUD). Ends
   the frame (FLUSH, FINISH), then stops VIF1 (an I-bit MARK) until the
   first vblank after the frame's FINISH, or the FINISH itself if that comes
   early in the vblank (vsync off: at the FINISH). The picture is then
   drawn before the screen shows its first line; a frame drawn after it
   waits for it, so it may draw into the same target again. */
void mx_frame_present(void);

/* ---- shaders (HLSL vertex programs, PLAN_PROCGEN) ---------------------------- */

/* A VU1 render program compiled from an HLSL vertex shader (include/vu.hlsli):
   per vertex it reads one quadword per input and writes nreg GIF registers.
   Each input is its own stream in memory, in its UNPACK format, so position,
   normal and colour can each take as few bytes as they need. */
struct mx_shader_in {
  u8 fmt;   /* UNPACK vn << 2 | vl (MX_UNPACK_*) */
  u8 usn;   /* zero-extend */
  u8 bytes; /* per vertex in memory */
  u32 mask; /* STMASK for the UNPACK, 0: none (fields 1.0 from COL: a float3
               input from V2-32 is (x, y, 1); values from ROW: an int3 input
               TEXCOORD_V2_16_F4096 is (s, t, 4096)) */
};
#define MX_SHADER_MAX_IN 8
struct mx_shader {
  const void *code;
  u32 code_bytes;
  u32 entry;   /* 64-bit units */
  u32 buf_qw;  /* VU1 double buffer (VIF1 BASE 0, OFFSET buf_qw) */
  u32 uni_qw;  /* uniforms after the context's GIF tag */
  u32 ctx_at;  /* the contexts: GIF tag (NLOOP 0) and uniforms, set once per draw */
  u32 ctx_qw;  /* = 1 + uni_qw */
  u32 nctx;    /* 2: used by turns; 1: a FLUSHE before writing it */
  u32 item_qw; /* = number of inputs */
  u32 out_qw;  /* = nreg */
  u32 max_n;   /* vertices per batch */
  u32 align;   /* batches start at multiples of it, and up to align - 1 more
                  vertices are read after the last one */
  u32 nreg;
  u64 regs;    /* GIF register list */
  u32 nin;
  struct mx_shader_in in[MX_SHADER_MAX_IN];
  u32 row[4];   /* ROW for the fields that the masked UNPACKs take from it */
  u32 in_at;    /* the first input in a buffer */
  u32 out_at;   /* the GIF packet: in a buffer; indexed, in data memory */
  /* indexed programs (max_i != 0): MX_VU_ISHADER */
  u32 max_i;    /* indices per batch */
  u32 idx_at;   /* the indices in a buffer */
};

/* From the layout header of vertex program NAME (llc's section .vu.header)
   and its code symbols. */
#define MX_VU_SHADER(NAME, code_, code_end_)                                                       \
  ((struct mx_shader){.code = (code_),                                                             \
                      .code_bytes = (u32)((code_end_) - (code_)),                                  \
                      .entry = VU_##NAME##_ENTRY,                                                  \
                      .buf_qw = VU_##NAME##_BUF_QW,                                                \
                      .uni_qw = VU_##NAME##_UNI_QW,                                                \
                      .ctx_at = VU_##NAME##_CTX_AT,                                                \
                      .ctx_qw = VU_##NAME##_CTX_QW,                                                \
                      .nctx = VU_##NAME##_NCTX,                                                    \
                      .item_qw = VU_##NAME##_ITEM_QW,                                              \
                      .out_qw = VU_##NAME##_OUT_QW,                                                \
                      .max_n = VU_##NAME##_MAX_N,                                                  \
                      .align = VU_##NAME##_ALIGN,                                                  \
                      .nreg = VU_##NAME##_NREG,                                                    \
                      .regs = VU_##NAME##_REGS,                                                    \
                      .nin = VU_##NAME##_NIN,                                                      \
                      .in = VU_##NAME##_INPUTS,                                                    \
                      .row = VU_##NAME##_ROW,                                                      \
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
                      .ctx_at = VU_##NAME##_CTX_AT,                                                \
                      .ctx_qw = VU_##NAME##_CTX_QW,                                                \
                      .nctx = VU_##NAME##_NCTX,                                                    \
                      .item_qw = VU_##NAME##_ITEM_QW,                                              \
                      .out_qw = VU_##NAME##_OUT_QW,                                                \
                      .max_n = VU_##NAME##_MAX_N,                                                  \
                      .align = VU_##NAME##_ALIGN,                                                  \
                      .nreg = VU_##NAME##_NREG,                                                    \
                      .regs = VU_##NAME##_REGS,                                                    \
                      .nin = VU_##NAME##_NIN,                                                      \
                      .in = VU_##NAME##_INPUTS,                                                    \
                      .row = VU_##NAME##_ROW,                                                      \
                      .in_at = VU_##NAME##_IN_AT,                                                  \
                      .out_at = VU_##NAME##_OUT_AT,                                                \
                      .max_i = VU_##NAME##_MAX_I,                                                  \
                      .idx_at = VU_##NAME##_IDX_AT})

/* The VU address of context k (0 .. s->nctx - 1). */
MX_INLINE u32 mx_shader_ctx(const struct mx_shader *s, u32 k) { return s->ctx_at + k * s->ctx_qw; }
/* Quadwords of VIF packets that mx_shader_e_context and mx_shader_e_batch
   write (the input data is referenced, not copied). */
MX_INLINE u32 mx_shader_masks(const struct mx_shader *s) {
  u32 k = 0;
  for (u32 i = 0; i < s->nin; i++)
    k += s->in[i].mask != 0;
  return k;
}
/* Inputs with fields from ROW (STMASK 01). */
MX_INLINE u32 mx_shader_rows(const struct mx_shader *s) {
  u32 k = 0;
  for (u32 i = 0; i < s->nin; i++)
    k += (s->in[i].mask & 0x55555555u) != 0;
  return k;
}
MX_INLINE u32 mx_shader_context_qw(const struct mx_shader *s) {
  return (s->nctx == 1 ? 3 : 2) + s->ctx_qw + (mx_shader_masks(s) ? 2 : 0) +
         (mx_shader_rows(s) ? 2 : 0);
}
MX_INLINE u32 mx_shader_batch_qw(const struct mx_shader *s) {
  return 3 + s->nin + mx_shader_masks(s);
}
/* What a draw sets once, into the context at VU address ctx: the GIF tag
   (PRIM prim, EOP; the program fills in NLOOP) and the uniforms uni (copied
   into the stream); then ITOP ctx. The batches after it (until the next
   context) use it. With s->nctx == 1 it waits (FLUSHE) until the batches
   before it have ended. Returns the cursor. */
u64 *mx_shader_e_context(u64 *c, const struct mx_shader *s, u32 ctx, u32 prim, const void *uni);
/* One batch of n <= s->max_n vertices: the counts into the next VU1 buffer
   (UNPACK with FLG), then each input from streams[i] (vertex 0 of the
   batch, quadword aligned; n rounded up to s->align vertices are read), then
   MSCAL for the first batch after a context (the program's init takes the
   context into registers) and MSCNT for the others. Returns the cursor. */
u64 *mx_shader_e_batch(u64 *c, const struct mx_shader *s, const void *const *streams, u32 n,
                       int first);

/* Set up VIF1 double buffering for s (BASE 0, OFFSET s->buf_qw) and upload
   its code unless it is resident. Waits (in the stream) until the batches
   before it are drawn (FLUSH): s may use the memory where the GIF still
   reads the packets of another program. */
void mx_frame_shader_setup(const struct mx_shader *s);
/* The batches a draw of n vertices as primitive prim (its type, prim & 7) is
   cut into: batch b starts at vertex b * step and has the rest, at most
   size. Returns their number. */
u32 mx_shader_batches(const struct mx_shader *s, u32 prim, u32 n, u32 *size, u32 *step);
/* All batches of such a draw (what mx_frame_shader_draw sends after the
   context): mx_shader_batches(..) * mx_shader_batch_qw(s) quadwords. */
u64 *mx_shader_e_draw(u64 *c, const struct mx_shader *s, u32 prim, const void *const *streams,
                      u32 n);
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

/* Quadwords of VIF packets that mx_shader_e_ibatch writes (the context:
   mx_shader_e_context). */
MX_INLINE u32 mx_shader_ibatch_qw(const struct mx_shader *s) {
  return 4 + s->nin + mx_shader_masks(s);
}
/* One batch: the counts, the inputs, the indices, MSCAL (first) or MSCNT
   (as mx_shader_e_batch). */
u64 *mx_shader_e_ibatch(u64 *c, const struct mx_shader *s, const void *const *streams,
                        const struct mx_ibatch *b, int first);
/* n batches of an indexed program (n * mx_shader_ibatch_qw(s) quadwords);
   first: b[0] is the draw's first (MSCAL). */
u64 *mx_shader_e_idraw(u64 *c, const struct mx_shader *s, const void *const *streams,
                       const struct mx_ibatch *b, u32 n, int first);
/* Draw n batches of an indexed program with the uniforms uni (copied into
   the stream once, with the context). */
void mx_frame_shader_idraw(const struct mx_shader *s, u32 prim, const void *uni,
                           const void *const *streams, const struct mx_ibatch *b, u32 n);

/* ---- mesh chains --------------------------------------------------------------- */

/* A draw's batches built once, as a chain that ends with ret (<mx/chain.h>):
   the bytes mx_frame_shader_draw / mx_frame_shader_idraw send after the
   context. It is tied to the input layout of s (programs with the same
   inputs, in_at and align can share it), to the primitive type, and to the
   streams' addresses (not their contents: vertices that change in place,
   such as skinned ones, keep their chain). Returns 0, or -1 if pb is full. */
u32 mx_shader_mesh_qw(const struct mx_shader *s, u32 prim, u32 n);
int mx_shader_mesh(struct mx_chain *ch, struct mx_pbuf *pb, const struct mx_shader *s, u32 prim,
                   const void *const *streams, u32 n);
MX_INLINE u32 mx_shader_imesh_qw(const struct mx_shader *s, u32 n) {
  return n * mx_shader_ibatch_qw(s) + 1;
}
int mx_shader_imesh(struct mx_chain *ch, struct mx_pbuf *pb, const struct mx_shader *s,
                    const void *const *streams, const struct mx_ibatch *b, u32 n);
/* Draw a mesh chain of s: the context (prim of the chain's primitive type,
   uniforms uni), then a CALL of the chain (chain mode only). */
void mx_frame_shader_call(const struct mx_shader *s, u32 prim, const void *uni,
                          const struct mx_chain *mesh);

/* ---- textures (PATH3) -------------------------------------------------------- */

/* Upload w x h pixels at data (in memory) to dbp by the GIF channel, now,
   without waiting. The chain is built in the slot's own buffer. Returns 0,
   or -1 if the slot's previous upload is still running. */
int mx_frame_texture(int slot, u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, const void *data);
/* The same for the levels of t, level n from data[n], in one chain. */
struct mx_tex;
int mx_frame_tex_upload(int slot, const struct mx_tex *t, const void *const *data);

#endif
