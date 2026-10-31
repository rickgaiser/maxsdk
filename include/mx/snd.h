#ifndef MX_SND_H
#define MX_SND_H

/* Sound through ps2sdk's IOP modules libsd.irx and audsrv.irx, which libmx
   carries and loads from EE memory (docs/bios.md). The output (core 1) mixes:
     a PCM stream: the EE queues samples in any format audsrv converts to
       48 kHz (8 or 16 bits, mono or stereo); its ring holds about 100 ms, so
       queue some every frame;
     samples: SPU ADPCM (mx_snd_encode) loaded into SPU RAM once, played on
       voices 1 .. 23 (audsrv keeps voice 0 free).
   Each call is one or more RPCs to the IOP and waits for them. Not for
   interrupt handlers, nor for two threads at once. */

#include <mx/types.h>

#define MX_SND_RATE 48000 /* the SPU2's output rate */
#define MX_SND_VOL_MAX 0x3FFF
#define MX_SND_VOICES 24

/* Load the modules (unless audsrv is there), start audsrv and its ADPCM
   part. Returns < 0 if the IOP does not answer or a module does not load. */
int mx_snd_init(void);

/* ---- the PCM stream ---------------------------------------------------------- */

/* The format of the samples queued from now on (rate in Hz, bits 8 or 16,
   channels 1 or 2; 16-bit stereo is interleaved). The default is 48000 Hz
   16-bit stereo. Returns 0, < 0 if audsrv cannot convert it. */
int mx_snd_stream_format(u32 rate, int bits, int channels);
/* Bytes the ring has room for now. */
u32 mx_snd_stream_room(void);
/* Bytes queued and not played yet. */
u32 mx_snd_stream_queued(void);
/* Queue up to bytes of data (any alignment): returns the bytes taken, which
   is less when the ring is full. The stream sounds from the first queue on. */
u32 mx_snd_stream_queue(const void *data, u32 bytes);
/* Silence the stream and drop what is queued. */
void mx_snd_stream_stop(void);
/* The stream's volume, 0 .. MX_SND_VOL_MAX (the default). */
void mx_snd_stream_volume(int volume);

/* ---- samples ----------------------------------------------------------------- */

/* Bytes of ADPCM for n samples. */
#define MX_SND_ADPCM_SIZE(n) ((((n) + 27) / 28) * 16)
/* Encode n samples (16-bit mono) to SPU ADPCM in out (MX_SND_ADPCM_SIZE(n)
   bytes; the last block is padded with silence). loop: the voice plays it
   again from the start at the end; else it stops there. Returns the bytes
   written. */
u32 mx_snd_encode(const s16 *pcm, u32 n, u8 *out, int loop);

/* A sample in SPU RAM; its address is its id for audsrv, so keep it in
   place while it is loaded. */
struct mx_snd_sample {
  u32 rate;
  u32 size;
};

/* Load size bytes of ADPCM (from mx_snd_encode, any alignment), played at
   rate Hz (up to four times MX_SND_RATE), into SPU RAM. Returns 0, < 0 on
   failure (IOP or SPU RAM full). */
int mx_snd_load(struct mx_snd_sample *s, const void *adpcm, u32 size, u32 rate);
/* Free the SPU RAM of a loaded sample (stop its voices first). */
int mx_snd_free(struct mx_snd_sample *s);
/* Play s on voice (1 .. 23), or on a free voice when voice < 0. Returns the
   voice, < 0 when the voice (or every voice) is busy. */
int mx_snd_play(const struct mx_snd_sample *s, int voice);
/* 1 while voice plays s. A looping sample counts as ended once it has been
   through its loop end (the SPU2's ENDX), so do not test those. */
int mx_snd_playing(int voice, const struct mx_snd_sample *s);
/* A voice's volume, 0 .. MX_SND_VOL_MAX (the default) each side. */
void mx_snd_voice_volume(int voice, int left, int right);

#endif
