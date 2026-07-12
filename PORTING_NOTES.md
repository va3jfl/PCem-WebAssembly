# PORTING_NOTES — how PCem was brought to WebAssembly

Engineering record of the port: what was removed, what was added, every core
patch, and why each decision fell the way it did.

Upstream: `https://github.com/sarah-walker-pcem/pcem`
Snapshot: commit `d674c4088e04a5fdc74e452c4d5284fa8920726d` (2026-05-08, "vNext" tree)
Toolchain: emscripten 3.1.52 · LLVM/clang 18 (wasm32 backend + wasm-ld) · binaryen 115

---

## 1. What was nuked

The entire native UI and host-passthrough surface is excluded from the build
(not deleted from `core/`, simply never compiled):

| Excluded                              | Replaced by                                   |
|---------------------------------------|-----------------------------------------------|
| `src/wx-ui/**`, `src/qt-ui/**`        | `web/` single-page app + `wasm/wasm-bridge.cpp` |
| `src/codegen/**` (dynarec)            | `wasm/platform/wasm-codegen-stub.c`            |
| `sound/soundopenal.c`                 | `wasm/platform/wasm-audio.c` (SAB rings + AudioWorklet) |
| `sound/{midi_alsa,sdl2-midi,win-midi}.c` | `wasm-input.c` MIDI stubs                   |
| `cdrom/cdrom-ioctl-{linux,osx}.c`, `cdrom-ioctl.c` | `cdrom-ioctl-dummy.c` (upstream's own no-host-CD backend) |
| networking (`src/networking/**`)      | not built (`USE_NETWORKING` never defined)     |
| plugin engine dlopen path             | not built (`PLUGIN_ENGINE` never defined; built-in device registration only) |

Everything else — every machine model, CPU table, video/sound/storage device,
the DOSBox OPL/CGA-composite code, MiniVHD, SCSI, MFM, LPT sound devices —
compiles unmodified.

## 2. The platform layer (`wasm/platform/`)

PCem's core expects a "platform" to provide a specific surface (the same one
`wx-sdl2*.c` provides natively). The wasm platform implements it:

* **`wasm-main.c`** — `main()` mirrors `wx-sdl2.c`'s startup order exactly:
  logging hooks → `paths_init()` (+ overrides to `/pcem/*`) →
  `init_plugin_engine()` → `*_init_builtin()` registration → ROM-set probe
  (`romspresent[]` via repeated `loadbios()`, `gfx_present[]` via
  `video_card_available()`). Scheduling runs on a DEDICATED EMULATION
  PTHREAD (started on first boot), like the native emulation thread: it
  executes accumulated 10 ms `runpc()` timeslices (the native `drawits`
  pacing algorithm, including the 50 ms clamp and the 200-frame NVR autosave
  cadence) and additionally wall-clock-caps each burst at ~25 ms — a guest
  that emulates slower than real time sheds its backlog and runs below 100%
  instead of freezing anything (v3 ran these slices on the browser main
  thread, which is exactly what froze the page on Pentium-class guests: the
  tab showed the first BIOS frame and stopped responding). Control calls
  (`pcem_wasm_*`, browser main thread) and the emulation thread serialise on
  `emu_mutex`; core-initiated stops (`stop_emulation_now`, which can fire on
  the emulation thread) only set a flag that the main-thread UI tick acts
  on, so JS state callbacks always fire in the page's JS scope. The
  remaining `emscripten_set_main_loop` tick is UI-only (title relays,
  deferred stops, and a stall watchdog: if the emulation thread's heartbeat
  goes stale for >4 s while RUNNING, the user gets an error toast instead
  of a page that looks alive but ignores every control). Control calls
  take the mutex with a 2 s deadline (`pthread_mutex_timedlock`) — if the
  emulation thread ever dies holding it, buttons degrade to an error toast
  rather than hanging the browser main thread forever. A
  `pcem_wasm_debug_burn_us()` export lets tests simulate
  slower-than-realtime guests. Also provides
  `startblit()/endblit()`, the `thread_*_mutex` API (missing from
  `thread-pthread.c`), `timer_read()/timer_freq` (µs via
  `emscripten_get_now`), `set_window_title`, `stop_emulation_now`,
  `atapi_close`, `cdrom_drive`, and the exported `pcem_wasm_*` control API
  (start/stop/pause/reset, floppy/CD hot-swap — the CD swap replays the
  native `IDM_CDROM_IMAGE` sequence: `atapi->exit(); atapi_close();
  image_open(); cdrom_drive = CDROM_IMAGE`).
* **`wasm-video.c`** — implements `video_blit_memtoscreen_func`. PCem's blit
  thread (spawned by `video.c` itself, running as a pthread/Web Worker) calls
  it with rendered rows of `buffer32`. NOTE the pixel format: `makecol(r,g,b)
  = b | g<<8 | r<<16`, i.e. 0x00RRGGBB values = little-endian memory bytes
  B,G,R,0 — the blit swizzles red/blue while forcing alpha opaque as it
  copies rows into a 2048×2048 RGBA shadow buffer and bumps a generation
  counter. (v1–v8 copied bytes straight through, which red/blue-swapped
  every video mode; the smoke tests now assert the CGA test card's cyan bar
  is actually cyan — the canary for this class of bug.) JS polls the
  counter each rAF and `putImageData`s only changed frames. Key subtlety:
  **blit size is the pixel size, `updatewindowsize()` is only the display
  aspect** — CGA blits 656×208 but asks for a 656×416 window (line doubling
  is the host scaler's job, natively done by the GPU; here by canvas CSS).
  Also provides `create_bitmap`/`destroy_bitmap`/`hline` (native versions
  live in `wx-sdl2-video.c`).
* **`wasm-audio.c`** — replaces the OpenAL backend. Mix blocks are 20 ms
  (`sound_buf_len_al = 48000/50`; native hands OpenAL ~50 ms lumps, but here
  smaller production chunks let a smaller cushion absorb the emulation
  thread's burst pacing). The worklet (below) owns delivery policy: PRE-ROLL
  (~60 ms before starting a stream — consuming the instant samples appear
  drains dry at every burst boundary), a ±1.5% resample-ratio DRIFT SERVO
  centred on the target fill (guest timer clock vs audio hardware clock
  drift otherwise walks the cushion into periodic underruns), an ADAPTIVE
  CUSHION (each underrun raises the target, capped at 250 ms, so hosts that
  can't sustain a small cushion converge on one they can), and ~2 ms
  fade-out/fade-in RAMPS instead of click-edged silence. Delivery counters
  (underruns/fill/target) post to the page once a second —
  `emu.audioStats()` — and `tools/audio_test.mjs` asserts zero underruns in
  a 10 s full-speed run, adaptation under a forced slow-motion burn, and
  clean recovery. When the guest itself runs below 100%, samples simply
  don't exist to play — that stutter is the speed deficit, not delivery,
  and it now degrades with fades instead of grinding. `sound.c` hands over
  mixed int32 stereo blocks at 48 kHz (`givealbuffer`) and int16 CD audio at
  44.1 kHz from the CD pthread (`givealbuffer_cd`); both go into lock-free
  SPSC rings in static memory. Because the pthreads build makes the wasm
  heap a `SharedArrayBuffer`, the `AudioWorkletProcessor`
  (`web/js/audio-worklet.js`) builds typed-array views directly over the
  rings and consumes them on the audio thread with linear resampling —
  zero-copy, no postMessage in the steady state.
* **`wasm-input.c`** — owns `pcem_key[272]` (indexed by XT set-1 scancode,
  extended keys `|0x80`, exactly what `keyboard.c`'s scancode tables expect;
  the JS keymap translates `KeyboardEvent.code`), mouse delta accumulation
  with PCem's button bit order, joystick/MIDI stubs.
* **`wasm-utils.c`** — the ~10 `wx_*` utility functions core code actually
  links against (dir/file existence, home dir, `wx_messagebox` → JS toast
  via `Module.onPCemMessage`, …) plus `wasm_rom_fetch()`, the /roms
  on-demand fetcher (see §7).
* **`wasm-diskio.c`** — browser-File-backed hard disks. Big images cannot
  live in MEMFS (the whole file would sit inside the ≤2 GB wasm heap; an
  8 GB image died with "RangeError: Array buffer allocation failed"), so a
  hard disk attaches as a lazily-read view over the user's local File:
  `hdd_file.c` routes `browser:<id>:<name>` paths here; reads fault 128 KB
  blocks through a mailbox the browser main thread services with
  `File.slice()` from `main_tick` (the emulation thread sleep-waits ~ms);
  a 12 MB FIFO of clean blocks is the read cache; writes are copy-on-write
  into pinned dirty blocks (session-only — the user's file is never
  touched). Core patch: `HDD_IMG_BROWSER` branches in `hdd_file.{h,c}`.
  Fixed VHDs are treated as raw (footer subtracted); dynamic VHDs still
  stage to MEMFS (≤256 MB) since their sparse layout needs minivhd's FILE*
  path. Fault cost engineering (this is where "app loading stutters the
  whole machine" lived): every uncached block freezes the emulation thread
  — guest time and audio production stop — until the browser main thread
  services the mailbox. Three measures shrink that stall by ~30x for
  sequential I/O: the faulting thread now POSTS a service request to the
  main thread immediately (MAIN_THREAD_ASYNC_EM_ASM poke; the old rAF-paced
  poll, up to ~16 ms per fault, remains only as backstop), each round trip
  fetches a RUN of up to 4 consecutive absent blocks (512 KB per stall
  instead of 128 KB), and the clean-block cache is 64 MB (512 blocks — an
  OS-boot working set) instead of 12 MB.
* The same engine backs THREE media sources: local Files (`File.slice`),
  hosted URLs (HTTP Range requests — production servers support this
  natively for static files; `serve.py` implements single-range GETs), and
  the CD path (a `BrowserFile : TrackFile` in `dosbox/cdrom_image.cpp`
  routes `browser:` image paths through the block engine, so ISOs of any
  size attach lazily from either source; the mailbox is mutex-serialized
  because the CD-audio pthread reads too).
* **Main-thread faults are forbidden — CD headers are preloaded.** The
  mailbox is serviced BY the browser main thread, but `image_open()`'s
  ISO volume-descriptor sniffing runs ON that thread (inside `start()`,
  a hard reset, or a CD swap). Before v20.3 each such read busy-waited
  the full 15 s `DISKIO_REQ_TIMEOUT_MS` waiting for a service only the
  blocked thread itself could provide — attaching an ISO froze the tab
  for ~a minute (4 PVD probes) and the machine then *silently* booted
  with an empty CD drive. Now: (1) `emu.preloadDiskHeader()` runs right
  after CD registration (machine editor + deep-link), staging the first
  512 KB into `wasm_diskio_preload()` as pinned resident blocks (never
  evicted, so a later hard reset finds them too), which satisfies every
  open-time read without touching the mailbox; (2) `diskio_fetch`
  refuses main-thread faults instantly (pclog) instead of freezing; and
  (3) pc.c's empty-drive fallback posts an `onPCemMessage` toast, so a
  CD that fails to open is never silent. `tools/cdrom_iso_test.mjs`
  regression-tests all of it (and `--stock` reproduces the old hang).
* **Media hot-swap is live from the UI (v21.1.2).** The core's swap entry
  points (`pcem_wasm_cdrom_load/eject`, `pcem_wasm_fdd_load/eject` — the CD
  pair replays native `IDM_CDROM_IMAGE`: `atapi->exit(); atapi_close();
  image_open()`, so the guest gets UNIT ATTENTION / "medium may have
  changed" and multi-disc installs see disc 2) existed since the first
  port but nothing in the front-end ever called them — media picks only
  edited the profile JSON, which `start()` reads once, so every disc
  change silently demanded a reboot. Fixed in the front-end:
  `ui.setCd()/setFloppy()` drive the entry points and keep the profile +
  a `ui.mounted` snapshot in sync; a status-bar **💿 Media** menu swaps
  floppies/CD on the running machine (single .iso lazily via
  register+preload, .cue/.bin sets staged together); saving the Configure
  dialog for the *running* system applies floppy/CD image diffs live and
  says hardware changes wait for reboot. Browser-disk slot release is now
  deferred to an orphan sweep (`ui.sweepOrphanDisks()` on editor close /
  boot / stop / swap): the editor's old unregister-at-pick freed a slot a
  RUNNING machine could still be mounted on — reads failed and, worse,
  the freed id was instantly re-issued to the new file, so the guest read
  disc 2's bytes through disc 1's TOC. A slot is only freed once no
  profile and no live drive references its token (hard disks still need
  a reboot — IDE geometry binds at reset, same as the native GUI).
  `tools/cd_hotswap_test.mjs` regression-tests the lot: hot-insert into
  an empty drive, swap, swap again through the real Media menu +
  file-chooser, floppy swap, eject, hard reset — asserting `image_open
  ok` per swap, byte-verified reads per disc, slot recycling, no mailbox
  timeouts, machine RUNNING throughout. `web/js/export.js` is the
  inverse: it streams base-plus-overlay merged disks (dirty blocks read
  straight out of the wasm heap via `wasm_diskio_block_*` exports), staged
  media, and the machine's .nvr files into a user-picked folder with a
  `profile.json` media manifest — the deep-link loader consumes exactly
  that manifest (`url` fields resolved relative to the profile, disks/CDs
  lazy, floppies staged, CMOS restored).
* **`wasm-stubs.c`** — debug viewers (font/palette/VRAM/voodoo) and
  screenshot-flash no-ops.
* **`wasm-bridge.cpp` also exports the device_config_t tables** —
  `getDeviceConfig(kind, name, machine)` walks the same per-device option
  tables native PCem generates its "Configure" dialogs from
  (wx-deviceconfig.cc): sound cards by internal name, video cards through
  `video_card_getdevice()` (so "builtin" resolves per romset — Tandy/PCjr
  display options), the model device, HDD controllers, and `voodoo_device`.
  CONFIG_MIDI rows are filtered out (no host-MIDI backend; the native dialog
  hides them too when `midi_get_num_devs()==0`). The returned `section` is
  `device_t.name` — the exact `[section]` `device_get_config_int()` reads —
  and the web editor's ⚙/Configure dialogs write their values into
  `profile.device_settings[section]`, which `build_cfg_text()` was already
  passing through into pcem.cfg. The editor also mirrors the native PCI rule:
  the Voodoo checkbox + Configure grey out on non-PCI machines
  (`MODEL_PCI`, wx-config.c).
* **`wasm-codegen-stub.c`** — see §4.
* **`wasm/shim/`** — `SDL.h` (only `paths.c` touches SDL: `SDL_GetBasePath`),
  `wx/defs.h` (satisfies `wx-utils.h`'s typedefs), and
  `codegen_backend_wasm.h` (block-table constants for shared headers).

## 3. Core patches (complete list)

Kept deliberately tiny; everything else is additive. All are marked with a
`PCem-web:` comment.

1. **`includes/private/codegen/codegen_backend.h`** — add an
   `#elif defined __EMSCRIPTEN__` branch including `codegen_backend_wasm.h`
   instead of `#error`ing on unknown architectures.
2. **`src/thread-pthread.c`** — `#include <unistd.h>` for the `usleep`
   prototype (implicit declarations are fatal in this port, see §5).
3. **Nine timer-callback signature fixes** (`disc.c`, `fdc.c`, `ide.c`×2,
   `keyboard_{amstrad,at,olim24,pcjr,xt}.c`): upstream registers no-arg
   functions into `void (*)(void *)` timer slots through `(void *)` casts.
   Native x86 shrugs; **WebAssembly traps on indirect calls whose signature
   mismatches** ("null function or function signature mismatch"). Each site
   gained a three-line `static void <fn>_timer_cb(void *priv)` adapter and
   the `timer_add()` call was pointed at it. (The alternative,
   `-sEMULATE_FUNCTION_POINTER_CASTS`, taxes *every* indirect call —
   unacceptable in an emulator built out of indirect calls.)
4. **`src/devices/nvr.c` — first-boot RTC initialisation.** With
   `enable_sync` on and NO existing .nvr file, upstream leaves the whole
   CMOS at 0xFF: REGB.SET is stuck on (so `nvr_onesec` never calls
   `rtc_tick`) and the time registers are unreadable garbage. Real firmware
   boot blocks poll the RTC seconds register until it looks sane — the
   PB570's Intel AMIBIOS spins at FE00:076E doing exactly that — so every
   AT-class machine hung on a black screen at 100% CPU. Natively this is
   masked because savenvr() creates the file at the end of the first
   session; in the browser every page load starts from a fresh MEMFS, so
   EVERY boot is a first boot. The missing-file path now initialises to the
   same end state as the file-exists path: REGA=6, REGB=RTC_2412, time
   registers synced from the host clock.
5. **`src/thread-pthread.c` — stateful events.** Events were stateless
   condvars: a `thread_set_event()` with no waiter was lost forever. The
   video blit handshake makes that fatal: if the emulation thread's wake
   fires while the blit thread is between waits, the blit thread sleeps
   forever, `blit_data.busy` never clears, and the emulation thread wedges
   inside `video_wait_for_blit()` — video freezes while the (stale) speed
   readout still says 100%. Fixed with the Win32-manual-reset semantics the
   core was written against (`state` flag; identical to the fix upstream
   already carries in `qt-thread.c` — the wx-era `thread-pthread.c` never
   got it).
6. **`src/pc.c` — CD image-open failures must bind a backend (×2 sites).**
   `initpc()`/`resetpchard()` called `image_open()` and ignored its return.
   On an unreadable/invalid image, image_open bails out BEFORE assigning the
   global `atapi` ops pointer; the next IDE reset then executes
   `atapi->stop()` through NULL — a wasm indirect-call trap that killed the
   emulation thread mid-POST (with drives attached) while it held
   `emu_mutex`, freezing the page. Failures now fall back to the empty
   (null) drive. Related, in the platform (no core change): the browser
   build must use `cdrom_drive = -1` (the unix "null CD") for an empty
   drive, NEVER 0 — 0 routes to `ioctl_set_drive()`, a no-op in the dummy
   ioctl backend, which also leaves `atapi` NULL. The bridge, the eject
   path, and the platform default were switched to -1 accordingly.
7. **`src/flash/rom.c` — `romfopen()` HTTP fallback.** Under
   `__EMSCRIPTEN__`, when a ROM file isn't in the virtual FS, the platform's
   `wasm_rom_fetch()` pulls `/roms/<fn>` from the web server with a
   synchronous XHR, writes it to `/pcem/roms/<fn>`, and romfopen retries.
   This is what makes the server's `/roms` directory behave exactly like the
   desktop app's roms folder: same layout, per-file on-demand access, no
   manifest or pre-download step. 404s are remembered per page-load (cleared
   by the UI's Rescan) so repeated ROM-set probes stay cheap.

## 4. The dynarec problem

PCem's recompiler emits native x86/ARM machine code into buffers and jumps
into them. WebAssembly cannot do that (no runtime-generated callable code
inside a module), so:

* `src/codegen/**` is excluded; `wasm-codegen-stub.c` supplies the full API
  surface the rest of the core links against — self-modifying-code flush
  hooks as no-ops (no generated blocks can exist), zeroed timing tables,
  `fatal()` tripwires on any true recompile entry point.
* `386_dynarec.c` still compiles (it owns interpreter-adjacent globals like
  `cpu_recomp_*` counters and `cpu_end_block_after_ins`), but
  `exec386_dynarec()` is unreachable: the bridge hard-codes
  `cpu_use_dynarec = 0` when generating configs, so `pc.c::runpc()` always
  takes the `exec386()` interpreter path.
* CPUs whose `cpu_flags` include `CPU_REQUIRES_DYNAREC` (Pentium, K5/K6,
  6x86/M II, WinChip-adjacent overdrives …) ARE offered, labelled
  "(interpreter — slower)". The flag is native-UI policy, not a core
  invariant: nothing in the core checks it, and PCem's recompiler is
  interpreter-first by design (blocks execute through the interpreter while
  being compiled), so the interpreter op tables necessarily cover every
  CPU. Verified end-to-end by `tools/interp_cpu_test.mjs` (Pentium 75
  executes CPUID/RDTSC and renders). v3 disabled these CPUs outright — but
  note the PB570's IDT WinChip family is merely `CPU_SUPPORTS_DYNAREC`, so
  it slipped through v3's gate and exposed the main-thread freeze below.
* A wasm JIT backend is now **built, integrated, and validated** — see §4a for
  the enabling mechanism and §4b for the CPU backend that now services the
  dynarec path. The interpreter cores remain the DEFAULT (the JIT is opt-in per
  profile via `cpu_dynarec`); when enabled, `pc.c::runpc()` takes the
  `exec386_dynarec()` path and compiled blocks execute as wasm functions.

## 4a. Runtime wasm codegen — the foundation is proven

The "no runtime-generated callable code" limitation is real only for code that
tries to mutate the *running* module. A **freshly emitted `WebAssembly.Module`
that imports the main instance's shared memory and function table** sidesteps
it entirely, and that is exactly what a browser JIT does (v86 JITs x86 this
way). `wasm/jit/` implements the foundation both the CPU and Voodoo JITs need:

* **`wasm-emit.{h,c}`** — a runtime wasm bytecode emitter: LEB128/opcode
  helpers, a fixed type/import/export module template, and
  `wasm_emit_wrap_module()` which wraps a function body into a complete module
  importing `env.memory` (the shared heap, `shared` limits matching
  `MAXIMUM_MEMORY`) and `env.table` (`__indirect_function_table`). Because the
  generated instance shares our heap, absolute addresses of C globals are baked
  in as `i32.const`s; because it shares the table, **any C function pointer
  value is directly `call_indirect`-able** (in wasm a function pointer *is* a
  table index).
* **`wasm_jit_install()`** — `EM_JS` glue that `new WebAssembly.Module`/
  `Instance`s the bytes on the CALLING pthread (workers may sync-compile
  modules of any size — the 4 KB sync limit is main-thread-only, and neither
  JIT runs there), then installs the exported `f` into the shared table via
  `wasmTable.grow()` (link flag `ALLOW_TABLE_GROWTH=1`), returning the slot
  index — a callable C function pointer. A per-thread free-list recycles slots.
* **Thread model:** each pthread instantiates its own table, so a compiled
  function is callable only on the thread that installed it. Both JITs respect
  this — the CPU JIT compiles/runs on the emulation thread; the Voodoo JIT does
  so per render thread.

`tools/jit_selftest.mjs` proves all of this **inside the real pcem.wasm**, and
proves both backends' specific mechanisms bit-exactly (`jitSelfTest()` returns
`0x3f`):

* **module round-trip** — emit a function that reads a C global from the heap,
  writes it back, and calls a C helper through the shared table; assert the
  return value and the observed global write;
* **CPU path** — emit an integer-ALU kernel (`add`/`shl`/`and`/`or`/`xor`) that
  operates on "register" state in the shared heap, and check it matches the
  identical C computation bit-for-bit across a value sweep. add/shl/and/or/xor
  are exactly the uops a real block is built from;
* **Voodoo path** — emit a `voodoo_draw`-shaped span-fill (`for x in [x0,x1)
  fb[x]=colour`, a real wasm `loop`/`br_if`) over a shared framebuffer and
  verify the covered pixels — and only those — were written.

Both backends are now built on this foundation (§4b, §4c).

## 4b. CPU JIT — DONE (wasm/jit/codegen_backend_wasm*.c)

The full IR/uop backend now lives in `wasm/jit/`, compiled alongside the shared
`src/codegen/` IR layer (the native x86/arm backends stay excluded; only the
portable `codegen_ir.c`/`codegen_reg.c`/`codegen_block.c` build):

* **`codegen_backend_wasm.c`** — block lifecycle. Each block becomes ONE runtime
  `WebAssembly.Module` (function `f`, type `()->()`) importing the shared heap +
  this thread's table (§4a). Host registers map onto wasm locals
  (`codegen_backend_wasm.h` documents the layout: integer regs, an (i64,f64)
  pair per FP reg for MMX/x87, temp-slot and scratch locals). PCem's
  forward-only IR jumps can't patch emitted branch targets, so the epilogue
  rewrites the body once — every jump label becomes a `block…end`, every
  placeholder `br` gets its real relative depth. Block exits
  (`codegen_exit_rout`/`codegen_gpf_rout` sentinels) are inlined `return`s
  (register state is already flushed at those barrier uops). A C-side,
  mutex-guarded table-slot free-list lets `codegen_reset()` from the UI thread
  recycle slots the emulation thread reuses.
* **`codegen_backend_wasm_uops.c`** — the ~120 uop handlers plus the
  `codegen_direct_*` register-spill surface, mirroring the arm64 backend's exact
  size/flag semantics. Guest memory ops and flag rebuilds `call_indirect` the
  existing C helpers (`readmemll`, `writememql`, `CF_SET`, …) through the shared
  table; MMX/3DNow lanes go through small C kernels for trivially-correct
  saturation. `386_dynarec.c::exec_recompiler()` calls the block via its table
  index (a real C function-pointer value).
* **Differential oracle — `wasm-jit-oracle.c`** (build with
  `cpu_dynarec_oracle`): every compiled block is executed, then rolled back
  (a per-block RAM write-journal restores memory, `cpu_state` is snapshot/
  restored) and re-executed one instruction at a time under the interpreter;
  the architectural-state contract AND the write journals must match
  byte-for-byte or the core `fatal()`s. Handles loop UNROLLING (steps the
  interpreter until it reproduces the JIT's exit state) and device I/O (tainted
  blocks aren't replayable → kept as-is; blocks that call instruction handlers
  stay on the interpreter). `tools/jit_oracle_test.mjs` soaks real BIOS boots
  across 386SX/386DX/Pentium: **11.4 M blocks verified bit-exact, 0 mismatches**,
  and a pure-JIT boot reaches the same POST framebuffer the interpreter does.
* **Performance** — `tools/jit_bench_kernel.mjs` runs an identical ALU kernel
  under both modes (controlled, same guest work): **~2.3–3× on 386, ~5× on
  Pentium** (22→113 MIPS). Boot-to-POST is display-identical to the interpreter.
* **Not yet covered:** the 486-class default CPU boots with CR0.CD set (cache
  disabled), so `CACHE_ON()` keeps it on the interpreter here — upstream
  behaviour, not a JIT gap. Uops the backend doesn't emit `fatal()` loudly
  rather than silently mis-executing; none were hit across the boot soaks.

### CPU JIT in the app

The CPU recompiler is OPT-IN (`cpu_dynarec`, default false; "CPU recompiler"
checkbox in the machine editor, default unticked). This was default-on for
one release and reverted on measurement: the JIT wins ~2.7x on tight compute
(`jit_bench_kernel`), but on dispatch-bound workloads — a Win9x desktop is
timer ticks, tiny blocks and I/O — it measures ~0.6x the interpreter
(`jit_bench`, wall-time for identical cycle-budgeted slices), and a field
report matched exactly (67% guest speed at an idle desktop, 100% on the
interpreter). Per-block costs (hash+validate lookup, call_indirect, prologue)
dominate blocks of a few instructions; the interpreter's straight-line loop
has none of that. A compile-time cost
model now rejects blocks that can't win: any block containing interpreter
call-outs (I/O, HLT, system ops) or shorter than 6 instructions is flagged
CODEBLOCK_WASM_NOJIT and interprets permanently — measured, this lifts the
desktop-class bench from 0.6x to ~0.9-1.0x of the interpreter (parity within
container noise) while keeping ~2.4x on the compute kernel. A dispatch-cost
probe (`--threshold 2147483647`, never compile) measures the dispatcher
itself at parity with the plain interpreter loop, proving the remaining JIT
gap lives in short-block entry/exit ceremony, not lookup. Block chaining is
the known lever if the JIT is ever to clearly win this class of workload. The old "(interpreter — slower)" CPU labels and the
"can't exist in WebAssembly" help text predated the JIT and are gone;
`tools/interp_cpu_test.mjs` asserts the toggle exists, defaults OFF, and that
the interpreter path executes Pentium-class code correctly.

**Hot-block compile threshold.** A runtime `WebAssembly.Module` compile costs
orders of magnitude more than a native block emit, and PCem's stock policy
(compile on the second visit) turned OS boots into compile storms — loading a
Windows desktop bogged the guest to ~13% with choppy audio while the emulation
thread did little but synchronous module compiles. Blocks now interpret until
they've been visited `wjit_compile_threshold` (16) times (`codeblock_t
.wasm_hits`, reset when a block object is recycled but deliberately NOT on
dirty-page invalidation — the BYTE_MASK/NO_IMMEDIATES escalation already
bounds SMC recompiles, and resetting there demoted hot loops sharing a page
with data writes to permanent interpretation). Once-run code never compiles;
the differential oracle bypasses the gate (it wants maximal coverage).
Measured on the settled AMI386 BIOS loop: thousands of cold stretches
interpreted, zero steady-state compiles; the ALU kernel keeps its ~2.7x JIT
win. Known honest cost: tiny-block, I/O-bound stretches still dispatch
~1.5x slower under the JIT than the plain interpreter loop (block lookup +
call_indirect + prologue per handful of instructions) — that only shows when
the interpreter had less than that much headroom; block chaining is the
obvious next lever if it ever matters. `jitStats()` exposes
`cold_runs`/`compile_threshold`, and `tools/jit_bench.mjs` now reports the
honest wall-time-per-slice comparison (the `ins` counter is interpreter-only,
as on native).

## 4c. Voodoo JIT — span recompiler, full pixel pipeline (validated)

`core/includes/private/video/vid_voodoo_codegen_wasm.h` is the wasm analogue of
`vid_voodoo_codegen_x86-64.h`, included by `vid_voodoo_render.c` under
`__EMSCRIPTEN__`. `voodoo_get_block()` emits a runtime wasm module implementing
the per-pixel span loop specialised to the render state, installed into the
render thread's table, and returned as the `voodoo_draw` function pointer.
Blocks are cached per-thread with the same full-field key compare as the
native recompiler (xdir, fbzMode, fbzColorPath, alphaMode, fogMode,
textureMode[0/1], tLOD mirror bits, trexInit1 override bit).

* **Covered pipeline:** the pixel pipe end to end, both span directions —
  iterated-RGB *and* single-TMU textured colour (point + bilinear filtering,
  perspective correction with the exact `(1<<48)/w` reciprocal + `fastlog`
  per-pixel LOD, mip clamp, S/T mirror/clamp/wrap), the complete colour and
  alpha combine unit (all local/other selects incl. colour0/1, every
  mselect/reverse/add/invert variant the C rasteriser implements), chroma
  key, fog in all five modes (constant / fog-table with dfog interpolation /
  Z / iterated-alpha / W), alpha test (all eight funcs), alpha blending (the
  full src/dest factor matrix incl. the ASATURATE and default-case
  behaviours), dithering (4×4 and 2×2 via the shared tables), and the whole
  depth pipe — all eight ops, read/write masks, depth bias, depth-source and
  the 16-bit float W-buffer encoding. Emit-time specialised on the exact
  mode bits: a compiled span contains no runtime mode branches. The span
  bumps `state->pixel_count`/`texel_count` per pixel exactly like the native
  x86-64 recompiler, so `fbiPixelsIn` keeps advancing on JIT-rendered spans.
* **Depth-source fidelity note:** upstream's `DEPTH_TEST()` macro substitutes
  its argument unparenthesised into `!(comp OP old)`, so with
  `FBZ_DEPTH_SOURCE` the ternary swallows the comparison and every comparison
  op passes iff `(zaColor & 0xffff) != 0`. The emitted code reproduces this
  faithfully — byte-parity with the C rasteriser is the contract, quirks
  included. (The A/B oracle caught the initial "fixed" version diverging.)
* **The rest of the stack (all covered now):** dual-TMU sampling — TMU0
  pass-through and full fetch-and-blend with the TMU combine unit (both
  self-combine stages, detail/LOD-frac factors, the trilinear reverse-flip on
  odd LODs, and the C code's factor-carryover for reserved mselects) —
  Banshee tiled colour/aux addressing (`x_tiled`), SLI spans (the interleave
  is caller-side; the span is direction/parity-agnostic), dither-subtraction
  of the blend destination (4×4 and 2×2), the trexInit1 TMU-config output
  override (including the uint8 cother truncation its >8-bit value exposes),
  and the LFB colour select.
* **Safe fallback (returns NULL → portable C rasteriser):** only register
  configurations the C rasteriser itself `fatal()`s on (a_sel 3,
  cca_localselect 3, cc_mselect 6/7, cca_mselect 5-7, cc_add 3,
  ACOLORBEFOREFOG src blend) — NULL preserves upstream's loud failure — and
  untextured states whose combine references texture outputs (the C would
  read stale/uninitialised state). These can't be A/B-rendered (the reference
  would crash), so the oracle proves them with gate probes instead.
* **A/B framebuffer oracle — `wasm/jit/voodoo_ab.c`:** renders identical
  geometry through the C rasteriser and the wasm span into a private
  framebuffer (non-zero canvas, deterministic RGBA texture with a full mip
  chain) and `memcmp`s colour+depth. `tools/voodoo_ab_test.mjs` asserts
  **145 covered spans byte-identical, 0 mismatches** — the original 63-case
  untextured sweep kept verbatim, plus depth-pipe extras, dither, all fog
  modes, alpha test/blend matrices, combine-unit variations, textured spans
  (affine/perspective × point/bilinear × wrap/clamp/mirror × LOD, chroma
  key, texture-fed combine/alpha), right-to-left spans, SLI, tiled
  colour/aux, dual-TMU pass-through and blend (six combine variants incl.
  trilinear + TMU1 self-combine + detail), dithersub, the trex override and
  LFB — **163 covered spans, 0 mismatches, 109 compiled span variants proven
  engaged** (compile-count floor guards against the gate silently rejecting
  everything). A probe phase then asserts every fatal()-class config is
  rejected by the gate, representative covered states are accepted, and no
  probe compiles anything.

## 5. Wasm-specific gotchas encountered (and their fixes)

* **Implicit function declarations are runtime traps.** An implicitly
  declared `lpt_init_builtin()` (default-int signature) trapped on its very
  first call. The build now keeps `-Wimplicit-function-declaration` an error
  — in wasm that warning *is* a crash report from the future.
* **Function-pointer signature casts trap** (§3).
* **`fopen(...,"wt")` failures surface as traps in `fclose(NULL)`** — the
  global config write needed `/pcem/.pcem/` to exist before
  `saveconfig(NULL)` ran.
* **`ImageData` cannot wrap a `SharedArrayBuffer`** — the framebuffer is
  copied row-by-row out of the heap each presented frame (~1–2 MB per frame;
  negligible next to emulation cost).
* **Ubuntu's proxy blocked emsdk binaries**, so the toolchain was assembled
  from parts: system clang 18 + wasm-ld, binaryen 115 built from source,
  emscripten 3.1.52 from git (with acorn staged from its GitHub source —
  see `README.md` "Toolchain" for the sane emsdk route).

## 6. Threading model

```
browser main thread      UI only: control API entry points, canvas
                         putImageData (rAF), input events, title/stop relays
pthread: emulation       runpc() timeslices (drawits pacing + ~25 ms wall
                         burst cap), onesec() stats, NVR autosave cadence
pthread: blit thread     video.c's own worker; runs wasm_blit_memtoscreen
pthread: sound_cd        sound.c's CD mixer; feeds the CD ring
pthread(s): voodoo       vid_voodoo render threads when a Voodoo is configured
audio thread             AudioWorkletProcessor reading both rings from the SAB
```

`-pthread -sPTHREAD_POOL_SIZE=12` pre-spawns workers so `thread_create()`
inside `initvideo()`/`sound_init()` starts threads without yielding to the
event loop first. Cross-origin isolation is therefore mandatory at serve
time (`serve.py`, nginx snippet, or `coi-sw.js` fallback).

Control safety: JS-invoked mutations (boot/stop/reset/media swaps) run on
the browser main thread and take `emu_mutex`, which the emulation thread
holds while inside a runpc() burst; the wall-clock burst cap bounds
control-call latency to ~25 ms. `wx_messagebox` proxies to the main thread
(`MAIN_THREAD_ASYNC_EM_ASM` + strdup'd strings) because core code can raise
it from the emulation thread, where the page's `Module` callbacks don't
exist. `<unistd.h>` must NOT be included in files that see PCem's global
`int pause` — POSIX declares a `pause()` function (the emulation thread
sleeps via `emscripten_thread_sleep()` instead of `usleep()`).

## 7. Server-side ROMs

The deployment server exposes a `/roms` directory with the standard desktop
layout (`roms/<set>/<files>`, font ROMs at the top level). The wasm build
reads it through the romfopen fallback above — copy a desktop `roms/`
folder to the server (or `serve.py --roms /path/to/roms` to serve one in
place) and machine availability mirrors a native install. ROM images are
simply not part of the build artifacts; the server operator supplies the
directory.

## 7b. Host I/O bridges — networking, gamepad, MIDI (v21)

Three roadmap items shipped together, all built on the same discipline: the
core compiles UNMODIFIED, the platform layer speaks the interface the core
already expects, and lock-free SPSC rings in shared memory cross the
main-thread/emulation-thread boundary (the audio/diskio pattern).

**Networking — a WebSocket ethernet relay behind a libslirp shim.**
`core/src/networking/{ne2000,nethandler,queue}.c` (NE2000 + RTL8029AS, the
vlan poller, the frame queue) now compile with `USE_NETWORKING`, byte-for-byte
unmodified. ne2000.c was written against system libslirp — guest TX frames go
into `slirp_input()`, and host frames come back through the
`SlirpCb.send_packet` callback it registers via `slirp_new()`. That API is
also the shape of an ethernet pipe, so `wasm/shim/slirp/libslirp.h` +
`wasm/platform/wasm-net.c` implement exactly the surface ne2000.c calls
(real libslirp v4 signatures — designated initializers and all — so a future
core update either still fits or fails loudly at compile time) and route it:

* `slirp_input()` (emulation thread, NIC TX) → 256 KB TX ring → poke the
  browser main thread on the idle→busy edge (`MAIN_THREAD_ASYNC_EM_ASM`,
  main_tick backstop) → `web/js/net.js` → `ws.send()` — one binary WebSocket
  message per raw ethernet frame, the v86/websockproxy relay protocol.
  Everyone on the same relay is on the same virtual LAN, which makes
  browser-to-browser multiplayer "point both machines at the same URL"
  (IPX needs zero guest config; the stock relay images also answer DHCP and
  NAT to the internet).
* `ws.onmessage` (main thread) → RX ring → `wasm_net_pump()`, registered as
  an extra `vlan_handler` at `slirp_new()` time so it runs on the emulation
  thread at the NIC's own ~12 kHz poll cadence → `send_packet` → ne2000's
  queue → `ne2000_rx_frame()`. The pump respects the queue's headroom (its
  array caps at 100 and silently leaks on overflow) and delivers ≤8
  frames/tick.
* No backend armed for the boot (`net_mode` off / no card) → `slirp_new()`
  returns NULL → `net_slirp_inited = 0`, PCem's own "cable unplugged" mode.
  pcap never existed here: `USE_PCAP_NETWORKING` stays undefined and every
  pcap reference compiles out.

UI: the machine editor's Input / Network tab picks the card (RTL8029AS is
DEVICE_PCI and gated to PCI machines like the Voodoo), the connection
(baked-in public relay `wss://relay.widgetry.org/` — v86's long-standing
default, verified live — **Tabs in this browser**, a custom `ws(s)://` URL,
or Off), and the MAC. The tabs mode (v21.1) swaps the WebSocket for a
`BroadcastChannel`: machines in other tabs of the same browser share a LAN
with no server and no relay — the C side is oblivious (same rings, same
`__pcemNetTx` drain; the transport is a JS seam). BroadcastChannel never
echoes to the sender, so hub semantics hold for free; lightweight heartbeat
beacons give the status pill a live peer count.

Field note (matches desktop PCem and real hardware): under Windows 9x the
RTL8029AS installs itself — PCI PnP enumerates it and the inbox driver
binds, straight to Network Neighborhood. The NE2000 is a legacy ISA card
with no PnP: Windows can't detect it, and the manually-added
"Novell/Anthem NE2000 Compatible" driver only comes alive once its Device
Manager resources are set to the emulated card's values (default I/O
0x300, IRQ 10 — device_get_config_int falls back to the table defaults
when no ⚙ section exists, verified live). The Network tab now says so.
Internal-loopback mode 1 IS implemented in ne2000.c (driver self-tests
pass); modes 2–3 log-and-skip, same as upstream. The "Network relay
unreachable" toast is transport-side only — the card exists and the
machine keeps running with the cable "unplugged" while the client retries
with backoff; it never affects device detection or driver installation.
Every profile gets a random locally-administered MAC (02:xx…) at creation,
and deep-linked boots re-randomize it — N people booting one shared link
must not bring identical MACs to one relay LAN. `netcard` + `macaddr` are
machine-level cfg keys (pc.c loadconfig / ne2000_common_init read them);
the ⚙ dialog serves NE2000's own addr/IRQ table through the same
device_config_t path as every other card. A status pill shows link state
and frame counters; reconnects back off exponentially; `tools/net_relay.mjs`
is a zero-dependency self-hostable hub (also the test fixture, so CI never
depends on the public relay).

**Gamepad — the Gamepad API into `plat_joystick_state[]`.** wasm-input.c now
owns the real joystick platform layer: `web/js/gamepad.js` polls
`navigator.getGamepads()` per rAF on the main thread and writes axes
(±32767), a button bitmask and the d-pad-as-SDL-hat into shared state via
`pcem_joy_update()`; `joystick_poll()` (called from `runpc()` on the
emulation thread, same as native) maps plat state → `joystick_state[]` with
wx-sdl2-joystick.c's exact semantics — mappings, `POV_X`/`POV_Y` encodings,
atan2 hat angles. The generated cfg emits `joystick_type`, per-stick
`[Joysticks] joystick_N_nr` (pads map 1:1 when the profile's "Use host
gamepads" toggle is on, `nr = 0` when off — the guest then sees centred
sticks), explicit d→d axis/button mappings (Standard Gamepad order already
matches), and hat-backed POV mappings. All seven emulated stick types
(2/4/6/8-button, CH Flightstick Pro, SW Pad, TM FCS) come straight from the
core's `joystick_get_*` tables through the bridge inventory.

**MIDI — `midi_write()` to WebMIDI.** The MPU-401 UART (SB16/AWE32/Azt2316a)
calls `midi_write(byte)` on the emulation thread at ≤3125 B/s; bytes land in
a 4 KB ring, poke the main thread on the idle edge, and `web/js/midi.js`
reassembles the stream into complete messages — status/running status, the
2-vs-3-byte channel groups, realtime bytes passed through mid-message, SysEx
accumulated F0…F7 (dropped politely if the permission grant excluded sysex)
— because WebMIDI's `send()` only accepts whole messages. Enabling is a
user-gesture button in the editor (`requestMIDIAccess` rules); granted
outputs register with the core (`wasm_midi_set_devs`), which is what makes
`midi_get_num_devs() > 0` and the ⚙ dialogs grow their native
CONFIG_MIDI "MIDI out device" row — the row's value is the machine-level
`midi` cfg key (`profile.midi_dev`), exactly where native platforms read it.

## 8. Verification

* `tools/make_test_rom.py` — original 8088 program (CRTC init + CGA colour
  bars) standing in for a BIOS; lets CI exercise the full stack with zero
  proprietary bytes. Now also emits a 128 KB variant for the
  "[Socket 7] ASUS P/I-P55TVP4" (`p55tvp4/tv5i0204.awd`) whose prologue
  executes CPUID and loops on RDTSC until the TSC advances (a single
  back-to-back delta can be 0 under PCem's Pentium pairing model) before
  drawing — bars on screen prove Pentium-class interpreter execution.
* `tools/smoke_test.mjs` — headless Chromium: cross-origin isolation on,
  inventory sane (97 machines / 49 video / 20 sound), genxt ROM detected via
  the on-demand /roms fetch, machine boots, framebuffer generation advances,
  canvas contains ≥3 colours and ≥20% non-black pixels.
* `tools/interp_cpu_test.mjs` — boots a Pentium 75 (a `CPU_REQUIRES_DYNAREC`
  part) on the interpreter; asserts the editor offers it enabled, CPUID/RDTSC
  execute, the test card renders, and the main thread stays at 60 fps.
* `tools/responsiveness_test.mjs` — regression for the v3 freeze: with a
  40 ms wall burn injected per 10 ms slice (`pcem_wasm_debug_burn_us`), the
  page must keep animating (<200 ms rAF gaps), a real Pause click must land
  within 1 s, speed must honestly report <100%, video must keep advancing,
  and removing the burn must recover to ~100%.
* `tools/bigdisk_test.mjs` — attaches a 256 MB patterned File as an IDE disk
  with zero staging, boots, and verifies byte-exact sector checksums through
  the full lazy path (File.slice → mailbox → block cache → hdd_file),
  copy-on-write isolation, and 8 GB-scale offset math via a synthetic File.
* `tools/cd_hotswap_test.mjs` — media hot-swap on a RUNNING machine: boots
  with an empty CD drive, hot-inserts disc 1, swaps to disc 2, swaps to
  disc 3 through the real status-bar Media menu + native file chooser,
  swaps a floppy, ejects, hard-resets — asserting `image_open ok` per
  swap, byte-verified backend reads per disc, browser-disk slot
  recycling, no mailbox timeouts, and state RUNNING throughout.
* `tools/publish_test.mjs` — the publishing round trip: a hosted system
  folder boots from one `?system=` URL with the disk attached lazily
  (asserts only a few hundred KB crossed the network for a 24 MB disk),
  then Export reproduces the folder with a session write merged in,
  neighbours byte-exact, ISO copied, manifest + LINK.txt present.
* `tools/deeplink_test.mjs` — `?system=&fd=` deep link: profile fetch,
  remote image staged into `/pcem/media`, autoboot, warm stop→boot cycle,
  floppy hot-swap API.
* `tools/jit_selftest.mjs` — proves runtime wasm codegen inside pcem.wasm:
  emit → compile → install → call sharing the heap + table, plus the CPU path
  (emitted ALU bit-exact vs a C oracle) and the Voodoo path (a compiled
  span-fill writes the right framebuffer region). `jitSelfTest()` must be
  `0x3f`, and must stay stable across repeat runs (slot reuse).
* `tools/jit_oracle_test.mjs` — the CPU recompiler correctness regression:
  boots real BIOS firmware across 386SX/386DX/Pentium with the JIT AND its
  interpreter-differential oracle active (§4b), asserting a large verified
  block count with zero mismatches, and that a pure-JIT boot reaches the same
  POST framebuffer as the interpreter.
* `tools/jit_bench_kernel.mjs` — controlled throughput: an identical ALU kernel
  under interpreter vs JIT (same guest work), reporting MIPS and speedup.
* `tools/voodoo_ab_test.mjs` — the Voodoo span-recompiler regression: the A/B
  framebuffer oracle (§4c) rendering the full-pipeline sweep (145 covered
  spans: depth pipe, dither, fog, alpha test/blend, combine unit, textured
  spans, both directions) through both the C rasteriser and the wasm span,
  asserting byte-identical framebuffers and a minimum engaged-compile count;
  plus the uncovered-mode fallback sweep (byte-identical, zero compiles).
* `tools/devcfg_test.mjs` — per-device settings end to end: bridge
  descriptors match the C tables (SB16 addr list, OPL cores, hidden MIDI
  row, Voodoo type/memories/threads, no `recompiler` under NO_CODEGEN),
  the ⚙ dialog round-trips values / Defaults / Cancel, the Voodoo checkbox
  is PCI-gated, the saved profile carries `device_settings` keyed by device
  name, the generated pcem.cfg contains the exact `[section]` lines, and
  the tuned machine (SB16 @0x240/NukedOPL + Voodoo 2 4MB+4MB, 4 render
  threads) boots to rendered frames.
* Observed on the test XT: 100% emulation speed, 100 fps video, in headless
  Chromium on two container cores.
* `tools/net_test.mjs` — the networking regression (§7b): local relay + two
  browser instances of the NE2000 machine; a synthetic broadcast frame
  travels A→B and B→A through the full pipe (TX ring → WebSocket → hub →
  RX ring → emulation-thread pump → `send_packet`) and is asserted
  byte-exact by checksum at the receiver; hub semantics (no echo to
  sender), the status pill, stop-drops-link, and the net_mode=off
  cold-backend switch are all asserted.
* `tools/gamepad_test.mjs` — scripted `navigator.getGamepads()`: axis
  scaling through the d→d mapping into `joystick_state[]` on the emulation
  thread, button order, POV hat angles (0°/90°) on the CH Flightstick Pro
  type, and that `gamepad_enabled = false` really disconnects (guest reads
  a centred stick while the host pad is held hard over).
* `tools/midi_test.mjs` — the genxt test ROM's MPU-401 epilogue (UART mode,
  note-on, running-status note-off) against a recording fake WebMIDI
  output: asserts the ⚙ descriptor grows the CONFIG_MIDI row listing the
  granted device, and that the synth receives exactly the reassembled
  [90 3c 7f] and [90 3c 00] — running status expanded, zero ring drops.
* `tools/download_test.mjs` — the per-disk Download button (v21.1): stamps a
  sector through the emulation-thread write path, downloads the image, and
  asserts the merged result byte-for-byte — overlay bytes in the stamped
  sector, source bytes everywhere else; the fixed-VHD footer re-appended
  ('conectix' tail, *.vhd name); the streaming (showSaveFilePicker) and
  Blob-anchor fallback paths byte-identical; staged VFS images (blank
  floppies) served by the same call.
* `tools/net_local_test.mjs` — the tabs-LAN backend (v21.1): two pages in
  one browser context, net_mode 'local', no relay process anywhere; instant
  online, presence peer counts, frames byte-exact both directions, no
  self-echo, pill text, and a clean leave when one machine stops.

## 9. Sensible next steps

1. Persist media + NVR via IDBFS (or OPFS) with an explicit "sync" affordance.
   (Deliberately shelved — and the Download button now covers the main loss
   case by hand. If ever wanted, it can be done JS-only via the disk-service
   read path; no core changes.)
2. ~~`SaveFilePicker`-based "download disk image" button in the media UI~~ —
   DONE (v21.1): per-slot Download in the Media tab; hard disks stream out
   base+overlay merged (constant memory via showSaveFilePicker, Blob
   fallback ≤1 GiB elsewhere), fixed VHDs get their footer back, staged
   images and blanks download as-is. The guest pauses during the stream.
3. ~~Gamepad API → `plat_joystick_state`~~ — DONE (§7b): rAF-polled Standard
   Gamepad mapping through the native joystick_poll() semantics, per-profile
   on/off toggle, all seven emulated stick types. Remaining: a mapping editor
   (the native wx-joystickconfig equivalent) for non-standard pads.
4. ~~WebMIDI → `midi_write`~~ — DONE (§7b): MPU-401 UART byte stream
   reassembled into whole messages (running status, SysEx) into a chosen
   WebMIDI output; the ⚙ dialogs grow the native CONFIG_MIDI row once
   access is granted.
5. ~~A wasm codegen backend for the IR~~ — DONE (§4b): the CPU JIT services
   the dynarec path, oracle-validated bit-exact, ~2.3–5× on hot code. Remaining
   here: make it the default once soaked on a broad machine matrix, and cover
   the 486 cache-disabled boot window (a `CACHE_ON()` policy choice).
6. Extend the Voodoo span JIT (§4c) to the textured / alpha / fog / dither
   pixel pipeline, each mode gated byte-green through the A/B oracle.
7. ~~Networking~~ — DONE (§7b), as an L2 WebSocket relay (v86/websockproxy
   protocol) behind a libslirp shim rather than SLiRP-in-wasm: NE2000 +
   RTL8029AS compile unmodified, browser-to-browser LAN via a shared relay
   (baked-in public default, custom URL, or off — per profile), bundled
   self-host relay in tools/. A wasm SLiRP NAT (guest internet with no
   relay at all) remains a possible follow-up.
