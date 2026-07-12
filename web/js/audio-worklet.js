// ============================================================================
// audio-worklet.js — AudioWorkletProcessor consuming PCem's sample rings.
//
// The wasm heap is a SharedArrayBuffer (pthreads build), so the processor
// reads the emulator's int16 stereo rings directly — zero-copy from the
// mixer to the audio device. Ring layout comes from pcem_audio_layout():
//   [ptr, frames, writeIdxPtr, readIdxPtr] for the 48 kHz mix ring,
//   then the same four for the 44.1 kHz CD ring.
// Indices are free-running uint32 frame counters (write ahead of read).
//
// Delivery policy (this is what keeps it click-free):
//   * PRE-ROLL — don't start a stream until targetFill frames are buffered;
//     the emulator produces sound in ~20 ms bursts paced by the emulation
//     thread, so consuming the instant samples appear just drains dry at
//     every burst boundary.
//   * DRIFT SERVO — the guest's sample clock and the audio hardware clock
//     drift; a small (±1.5% max) resample-ratio nudge proportional to the
//     fill error keeps the cushion centred instead of slowly draining to an
//     underrun (or growing into latency).
//   * ADAPTIVE CUSHION — every underrun raises targetFill (up to a cap), so
//     a host that can't sustain the small cushion converges to one it can.
//   * RAMPS — underrun fades out over ~2 ms and resumed streams fade in;
//     hard on/off transitions are audible clicks.
//
// Stats (underruns, fill, target) post to the main thread once a second for
// the status UI and the automated audio test.
// ============================================================================

class RingStream {
  constructor(rate, prerollFrames, maxFrames) {
    this.rate = rate;         // source sample rate
    this.pos = 0;             // fractional resample cursor
    this.started = false;     // pre-rolling until fill >= target
    this.target = prerollFrames;
    this.preroll0 = prerollFrames;
    this.max = maxFrames;
    this.underruns = 0;
    this.fade = 0;            // 0..1 output gain ramp (click suppression)
    this.lastL = 0;
    this.lastR = 0;
    this.fill = 0;
  }

  attach(ring, frames, wIdxArr, rIdxArr) {
    this.ring = ring;
    this.frames = frames;
    this.wIdx = wIdxArr;
    this.rIdx = rIdxArr;
  }

  // Consume one output frame into out[0..1]. Returns true if real samples
  // flowed (fade handled internally; silence written when not started).
  pull(out, deviceRate) {
    if (!this.ring) { out[0] = out[1] = 0; return false; }

    const w = Atomics.load(this.wIdx, 0);
    let r = Atomics.load(this.rIdx, 0);
    this.fill = w - r;

    if (!this.started) {
      if (this.fill >= this.target) {
        this.started = true; // fade-in begins below
      } else {
        // decay the last sample to zero so a stop isn't a step edge
        this.lastL *= 0.98;
        this.lastR *= 0.98;
        out[0] = this.lastL;
        out[1] = this.lastR;
        return false;
      }
    }

    if (this.fill < 2) {
      // ran dry: fade what we have to zero, re-preroll with a bigger cushion
      this.underruns++;
      this.started = false;
      this.target = Math.min(Math.floor(this.target * 1.5), this.max);
      this.fade = 0;
      this.lastL *= 0.98;
      this.lastR *= 0.98;
      out[0] = this.lastL;
      out[1] = this.lastR;
      return false;
    }

    // drift/burst servo: ±1.5% rate nudge toward the target fill
    const err = (this.fill - this.target) / this.target;
    const nudge = Math.max(-0.015, Math.min(0.015, err * 0.06));
    const step = (this.rate / deviceRate) * (1 + nudge);

    const i0 = r & (this.frames - 1);
    const i1 = (r + 1) & (this.frames - 1);
    const frac = this.pos - Math.floor(this.pos);
    let l = (this.ring[i0 * 2] * (1 - frac) + this.ring[i1 * 2] * frac) / 32768;
    let rr = (this.ring[i0 * 2 + 1] * (1 - frac) + this.ring[i1 * 2 + 1] * frac) / 32768;

    if (this.fade < 1) this.fade = Math.min(1, this.fade + 1 / 96); // ~2ms @48k
    l *= this.fade;
    rr *= this.fade;

    const npos = this.pos + step;
    const adv = Math.floor(npos) - Math.floor(this.pos);
    if (adv > 0) Atomics.store(this.rIdx, 0, r + adv);
    this.pos = npos;

    this.lastL = l;
    this.lastR = rr;
    out[0] = l;
    out[1] = rr;
    return true;
  }
}

class PCemAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.gain = 1.0;
    // 48k mix ring: 120 ms pre-roll, 480 ms adaptive cap. Production-grade
    // sizing: short guest-speed dips (app loading, window painting) cost a
    // few hundred ms of production deficit; a cushion this deep rides
    // through them with zero underruns instead of gapping. The added
    // latency is fine for a desktop emulator.
    this.mix = new RingStream(48000, 5760, 23040);
    // CD ring: 44.1k, same policy
    this.cd = new RingStream(44100, 5292, 21168);
    this.statTick = 0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.type === 'init') {
        const sab = d.buffer;
        const L = new Uint32Array(sab, d.layoutPtr, 8);
        this.mix.attach(new Int16Array(sab, L[0], L[1] * 2), L[1],
                        new Uint32Array(sab, L[2], 1), new Uint32Array(sab, L[3], 1));
        this.cd.attach(new Int16Array(sab, L[4], L[5] * 2), L[5],
                       new Uint32Array(sab, L[6], 1), new Uint32Array(sab, L[7], 1));
        this.ready = true;
      } else if (d.type === 'gain') {
        this.gain = d.value;
      } else if (d.type === 'reset') {
        this.mix.started = false;
        this.mix.target = this.mix.preroll0;
        this.mix.underruns = 0;
        this.cd.started = false;
        this.cd.target = this.cd.preroll0;
        this.cd.underruns = 0;
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    if (!this.ready) { L.fill(0); R.fill(0); return true; }

    const a = [0, 0], b = [0, 0];
    for (let i = 0; i < L.length; i++) {
      this.mix.pull(a, sampleRate);
      this.cd.pull(b, sampleRate);
      L[i] = Math.max(-1, Math.min(1, (a[0] + b[0]) * this.gain));
      R[i] = Math.max(-1, Math.min(1, (a[1] + b[1]) * this.gain));
    }

    // ~1 Hz stats for the UI/tests (128-frame quanta)
    if (++this.statTick >= Math.round(sampleRate / 128)) {
      this.statTick = 0;
      this.port.postMessage({
        type: 'stats',
        underruns: this.mix.underruns,
        fill: this.mix.fill,
        target: this.mix.target,
        started: this.mix.started,
        cd_underruns: this.cd.underruns,
      });
    }
    return true;
  }
}

registerProcessor('pcem-audio', PCemAudioProcessor);
