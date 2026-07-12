/*
 * wasm-audio.c — PCem-web audio backend.
 *
 * Replaces soundopenal.c.  sound.c mixes the emulated cards at 48 kHz and CD
 * audio at 44.1 kHz and hands us int32 / int16 stereo blocks (givealbuffer /
 * givealbuffer_cd).  We push them into lock-free single-producer /
 * single-consumer ring buffers that live in the (SharedArrayBuffer-backed)
 * wasm heap; an AudioWorkletProcessor on the JS side consumes them directly —
 * no copies through postMessage, no OpenAL emulation layer.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <string.h>

#include "ibm.h"
#include "sound.h"

/* Must be powers of two. Sized for ~1.4s of buffering headroom. */
#define RING_FRAMES 65536
#define CD_RING_FRAMES 65536

/* Mix chunk size. Native PCem hands OpenAL ~50 ms blocks; here the worklet
   consumes continuously, and smaller production chunks mean a smaller cushion
   covers the same burstiness: 20 ms chunks let ~3-chunk (60 ms) pre-roll
   absorb the emulation thread's burst pacing. sound.c reads this variable per
   mixed block, so it is safe to differ from the native default. */
int sound_buf_len_al = 48000 / 50; /* 960 frames = 20 ms per block */

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_write = 0; /* frame indices, free-running */
static volatile uint32_t ring_read = 0;  /* consumed by the audio worklet  */

static int16_t cd_ring[CD_RING_FRAMES * 2];
static volatile uint32_t cd_ring_write = 0;
static volatile uint32_t cd_ring_read = 0;

static int audio_muted = 0;

void initalmain(int argc, char *argv[]) {
        (void)argc;
        (void)argv;
}

void closeal() {}

void inital() {
        ring_write = ring_read = 0;
        cd_ring_write = cd_ring_read = 0;
}

/* sound.c calls this once per mixed block: sound_buf_len_al stereo frames of
   int32 accumulator samples. Clamp to int16 and enqueue. */
void givealbuffer(int32_t *buf) {
        int c;
        uint32_t w = ring_write;
        uint32_t fill = w - ring_read;

        if (audio_muted)
                return;
        if (fill >= RING_FRAMES - (uint32_t)sound_buf_len_al)
                return; /* consumer stalled (tab hidden etc.) — drop block */

        for (c = 0; c < sound_buf_len_al; c++) {
                int32_t l = buf[c * 2];
                int32_t r = buf[c * 2 + 1];

                if (l < -32768)
                        l = -32768;
                else if (l > 32767)
                        l = 32767;
                if (r < -32768)
                        r = -32768;
                else if (r > 32767)
                        r = 32767;

                ring[((w + c) & (RING_FRAMES - 1)) * 2] = (int16_t)l;
                ring[((w + c) & (RING_FRAMES - 1)) * 2 + 1] = (int16_t)r;
        }
        __sync_synchronize();
        ring_write = w + sound_buf_len_al;
}

/* CD audio: CD_BUFLEN stereo frames of int16 at 44.1 kHz, delivered from the
   sound_cd_thread. Same ring scheme; the worklet resamples 44.1 -> device. */
void givealbuffer_cd(int16_t *buf) {
        int c;
        uint32_t w = cd_ring_write;
        uint32_t fill = w - cd_ring_read;

        if (audio_muted)
                return;
        if (fill >= CD_RING_FRAMES - (uint32_t)CD_BUFLEN)
                return;

        for (c = 0; c < CD_BUFLEN; c++) {
                cd_ring[((w + c) & (CD_RING_FRAMES - 1)) * 2] = buf[c * 2];
                cd_ring[((w + c) & (CD_RING_FRAMES - 1)) * 2 + 1] = buf[c * 2 + 1];
        }
        __sync_synchronize();
        cd_ring_write = w + CD_BUFLEN;
}

/* ---- exports consumed by the JS front-end / audio worklet ---------------- */

/* Layout blob so JS can build typed-array views over the rings:
   [0] ring ptr        [1] ring frames      [2] &ring_write  [3] &ring_read
   [4] cd ring ptr     [5] cd ring frames   [6] &cd_write    [7] &cd_read   */
static uint32_t audio_layout[8];

uint32_t *pcem_audio_layout() {
        audio_layout[0] = (uint32_t)(uintptr_t)ring;
        audio_layout[1] = RING_FRAMES;
        audio_layout[2] = (uint32_t)(uintptr_t)&ring_write;
        audio_layout[3] = (uint32_t)(uintptr_t)&ring_read;
        audio_layout[4] = (uint32_t)(uintptr_t)cd_ring;
        audio_layout[5] = CD_RING_FRAMES;
        audio_layout[6] = (uint32_t)(uintptr_t)&cd_ring_write;
        audio_layout[7] = (uint32_t)(uintptr_t)&cd_ring_read;
        return audio_layout;
}

void pcem_audio_set_muted(int m) { audio_muted = m; }
