/*
 * wasm-diskio.c — browser-File-backed hard disk images.
 *
 * Large disk images cannot live in the Emscripten MEMFS: the whole file
 * would occupy browser/wasm memory (an 8 GB image can never fit under the
 * 2 GB heap ceiling — attempting it is what produced "RangeError: Array
 * buffer allocation failed").  Instead, a hard disk can be attached as a
 * lazily-read view over the user's local File:
 *
 *   - JS registers the File and passes its byte size here
 *     (wasm_diskio_register); the image path in the machine profile becomes
 *     the token  browser:<id>:<name>  which hdd_file.c routes to this
 *     backend.
 *   - Reads fault in 128 KB blocks on demand.  The emulation thread posts a
 *     request into a small mailbox and sleeps; the browser main thread
 *     polls the mailbox from its UI tick (main_tick), slices the File,
 *     copies the bytes into the wasm heap, and completes the request.
 *     A bounded set of clean blocks is kept as a read cache.
 *   - Writes are copy-on-write: the touched block is faulted in, marked
 *     dirty and kept for the whole session (the user's real file is never
 *     modified).  Only written blocks consume memory.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#include "ibm.h" /* pclog */

#define DISKIO_MAX_DISKS 8
#define DISKIO_BLOCK_SHIFT 17 /* 128 KB blocks */
#define DISKIO_BLOCK_SIZE (1u << DISKIO_BLOCK_SHIFT)
#define DISKIO_CLEAN_BUDGET 512 /* clean-block cache: 512 * 128 KB = 64 MB —
                                   sized for an OS boot / app-load working set;
                                   every cache miss freezes guest time (and so
                                   audio) for a main-thread round trip */
#define DISKIO_FETCH_RUN 4      /* blocks fetched per mailbox round trip:
                                   sequential reads (app loading!) pay one
                                   latency per 512 KB instead of per 128 KB */
#define DISKIO_REQ_TIMEOUT_MS 15000.0

enum { BLK_ABSENT = 0, BLK_CLEAN = 1, BLK_DIRTY = 2 };

typedef struct diskio_disk_t {
        int in_use;
        double size;       /* bytes (double: exact to 2^53) */
        uint32_t nblocks;
        uint8_t **block;   /* nblocks pointers */
        uint8_t *state;    /* nblocks BLK_* */
} diskio_disk_t;

static diskio_disk_t disks[DISKIO_MAX_DISKS];

/* FIFO of clean cached blocks for eviction. Entries may be stale (block
   became dirty or was freed); eviction revalidates before freeing. */
static struct {
        int disk;
        uint32_t idx;
} clean_ring[DISKIO_CLEAN_BUDGET];
static int clean_head = 0, clean_count = 0;

/* ---- request mailbox (emulation thread -> browser main thread) ------------ */

typedef struct diskio_req_t {
        volatile int stage; /* 0 idle, 1 posted, 2 in service, 3 done */
        volatile int ok;
        int disk;
        double offset;
        int len;
        uint8_t *buf;
} diskio_req_t;

static diskio_req_t req;

/* Multiple threads read media through this engine (IDE on the emulation
   thread, CD audio on sound.c's CD thread) — serialize mailbox use. */
static pthread_mutex_t fetch_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Polled from main_tick(): cheap C-side check before entering JS. */
int wasm_diskio_pending(void) { return req.stage == 1; }

/* ---- exports for the JS service (browser main thread) --------------------- */

EMSCRIPTEN_KEEPALIVE
int pcem_diskreq_poll(void) {
        if (req.stage != 1)
                return 0;
        req.stage = 2;
        return 1;
}
EMSCRIPTEN_KEEPALIVE
int pcem_diskreq_disk(void) { return req.disk; }
EMSCRIPTEN_KEEPALIVE
double pcem_diskreq_offset(void) { return req.offset; }
EMSCRIPTEN_KEEPALIVE
int pcem_diskreq_len(void) { return req.len; }
EMSCRIPTEN_KEEPALIVE
uint8_t *pcem_diskreq_buf(void) { return req.buf; }
EMSCRIPTEN_KEEPALIVE
void pcem_diskreq_complete(int ok) {
        req.ok = ok;
        req.stage = 3;
}

/* ---- registration (called from JS via ccall) ------------------------------- */

EMSCRIPTEN_KEEPALIVE
int wasm_diskio_register(double size) {
        int i;

        if (size <= 0)
                return -1;
        for (i = 0; i < DISKIO_MAX_DISKS; i++) {
                if (!disks[i].in_use)
                        break;
        }
        if (i == DISKIO_MAX_DISKS)
                return -1;

        disks[i].in_use = 1;
        disks[i].size = size;
        disks[i].nblocks = (uint32_t)((size + DISKIO_BLOCK_SIZE - 1) / DISKIO_BLOCK_SIZE);
        disks[i].block = calloc(disks[i].nblocks, sizeof(uint8_t *));
        disks[i].state = calloc(disks[i].nblocks, 1);
        if (!disks[i].block || !disks[i].state) {
                free(disks[i].block);
                free(disks[i].state);
                memset(&disks[i], 0, sizeof(disks[i]));
                return -1;
        }
        pclog("diskio: registered browser disk %i (%.0f bytes, %u blocks)\n", i, size, disks[i].nblocks);
        return i;
}

EMSCRIPTEN_KEEPALIVE
void wasm_diskio_unregister(int id) {
        uint32_t b;

        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return;
        for (b = 0; b < disks[id].nblocks; b++)
                free(disks[id].block[b]);
        free(disks[id].block);
        free(disks[id].state);
        memset(&disks[id], 0, sizeof(disks[id]));
}

/* ---- block engine (emulation thread) --------------------------------------- */

static int diskio_fetch_locked(int id, double offset, int len, uint8_t *dst) {
        double t0;

        /* The mailbox is serviced BY the browser main thread (main_tick /
           __pcemDiskTick). A fetch posted FROM the main thread would wait on
           itself for the full timeout — a frozen tab for 15 s per read, and
           the read still fails. This used to happen when image_open() probed
           an ISO's volume descriptors during start()/cdLoad(), both of which
           run on the main thread: ~4 probe reads x 15 s of busy-wait, then
           the CD silently came up empty. Fail fast and loudly instead; the
           open-time reads are satisfied from blocks pre-faulted at
           registration time (wasm_diskio_preload). */
        if (emscripten_is_main_browser_thread()) {
                pclog("diskio: refusing block fault on the browser main thread "
                      "(disk %i off %.0f len %i) — header not preloaded?\n",
                      id, offset, len);
                return 0;
        }

        req.disk = id;
        req.offset = offset;
        req.len = len;
        req.buf = dst;
        req.ok = 0;
        req.stage = 1; /* post */

        /* Nudge the browser main thread NOW instead of waiting for its next
           rAF tick: the emulation thread (and therefore guest time and audio
           production) is frozen until this request completes, and an rAF-paced
           mailbox costs up to ~16 ms per fault. The rAF poll in main_tick
           remains as the backstop; the JS tick re-checks the mailbox stage,
           so a double poke is a no-op. */
        MAIN_THREAD_ASYNC_EM_ASM({
                if (Module.__pcemDiskTick)
                        Module.__pcemDiskTick();
        });

        t0 = emscripten_get_now();
        while (req.stage != 3) {
                if (emscripten_get_now() - t0 > DISKIO_REQ_TIMEOUT_MS) {
                        pclog("diskio: request timed out (disk %i off %.0f len %i)\n", id, offset, len);
                        req.stage = 0;
                        return 0;
                }
                emscripten_thread_sleep(0.2);
        }
        req.stage = 0;
        return req.ok;
}

static int diskio_fetch(int id, double offset, int len, uint8_t *dst) {
        int r;
        pthread_mutex_lock(&fetch_mutex);
        r = diskio_fetch_locked(id, offset, len, dst);
        pthread_mutex_unlock(&fetch_mutex);
        return r;
}

static void clean_ring_insert(int id, uint32_t idx) {
        if (clean_count == DISKIO_CLEAN_BUDGET) {
                /* evict oldest still-clean entry */
                int n;
                for (n = 0; n < clean_count; n++) {
                        int pos = (clean_head + n) % DISKIO_CLEAN_BUDGET;
                        int d = clean_ring[pos].disk;
                        uint32_t bi = clean_ring[pos].idx;
                        /* skip stale entries (dirty or freed since) */
                        if (pos == clean_head) {
                                clean_head = (clean_head + 1) % DISKIO_CLEAN_BUDGET;
                                clean_count--;
                                if (disks[d].in_use && disks[d].state[bi] == BLK_CLEAN) {
                                        free(disks[d].block[bi]);
                                        disks[d].block[bi] = NULL;
                                        disks[d].state[bi] = BLK_ABSENT;
                                        break;
                                }
                                n = -1; /* restart scan from new head */
                                continue;
                        }
                }
        }
        clean_ring[(clean_head + clean_count) % DISKIO_CLEAN_BUDGET].disk = id;
        clean_ring[(clean_head + clean_count) % DISKIO_CLEAN_BUDGET].idx = idx;
        clean_count++;
}

/* Staging area for multi-block fetches (one mailbox round trip fills the
   whole run, then blocks are distributed to their own allocations). */
static uint8_t fetch_stage[DISKIO_FETCH_RUN * DISKIO_BLOCK_SIZE];

/* Ensure block idx is resident; returns pointer or NULL. Fetches a run of
   up to DISKIO_FETCH_RUN consecutive absent blocks in ONE round trip —
   sequential guest reads (OS boot, app loading) otherwise pay a full
   emulation-thread stall per 128 KB. */
static uint8_t *diskio_block(int id, uint32_t idx) {
        diskio_disk_t *d = &disks[id];
        uint32_t run, k;
        double off, remain;
        int len;

        if (d->state[idx] != BLK_ABSENT)
                return d->block[idx];

        /* how many consecutive blocks (starting at idx) are absent? */
        for (run = 1; run < DISKIO_FETCH_RUN; run++) {
                if (idx + run >= d->nblocks || d->state[idx + run] != BLK_ABSENT)
                        break;
        }

        off = (double)idx * DISKIO_BLOCK_SIZE;
        remain = d->size - off;
        len = (remain >= (double)(run * DISKIO_BLOCK_SIZE)) ? (int)(run * DISKIO_BLOCK_SIZE) : (int)remain;

        if (!diskio_fetch(id, off, len, fetch_stage))
                return NULL;
        if (len < (int)(run * DISKIO_BLOCK_SIZE))
                memset(fetch_stage + len, 0, run * DISKIO_BLOCK_SIZE - len);

        for (k = 0; k < run; k++) {
                uint8_t *p = malloc(DISKIO_BLOCK_SIZE);
                if (!p) {
                        /* out of memory mid-run: keep what we placed so far */
                        break;
                }
                memcpy(p, fetch_stage + k * DISKIO_BLOCK_SIZE, DISKIO_BLOCK_SIZE);
                d->block[idx + k] = p;
                d->state[idx + k] = BLK_CLEAN;
                clean_ring_insert(id, idx + k);
        }

        return d->block[idx]; /* NULL only if the very first malloc failed */
}

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

static int diskio_rw(int id, double offset, int len, uint8_t *buf, int is_write) {
        int r = 0;

        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return 1;
        if (offset < 0 || offset + len > disks[id].size)
                return 1;

        pthread_mutex_lock(&table_mutex);

        while (len > 0) {
                uint32_t idx = (uint32_t)(offset / DISKIO_BLOCK_SIZE);
                uint32_t in_block = (uint32_t)(offset - (double)idx * DISKIO_BLOCK_SIZE);
                int chunk = DISKIO_BLOCK_SIZE - in_block;
                uint8_t *p;

                if (chunk > len)
                        chunk = len;

                p = diskio_block(id, idx);
                if (!p) {
                        r = 1;
                        break;
                }

                if (is_write) {
                        memcpy(p + in_block, buf, chunk);
                        if (disks[id].state[idx] != BLK_DIRTY)
                                disks[id].state[idx] = BLK_DIRTY; /* pin: never evicted */
                } else {
                        memcpy(buf, p + in_block, chunk);
                }

                buf += chunk;
                offset += chunk;
                len -= chunk;
        }
        pthread_mutex_unlock(&table_mutex);
        return r;
}

/* ---- header preload (browser main thread) -----------------------------------
 *
 * image_open() runs on the browser main thread (inside start(), a hard
 * reset, or a CD hot-swap) and reads the image's first sectors to sniff the
 * ISO's volume descriptor.  The main thread cannot fault blocks through the
 * mailbox — it would be waiting for itself to service the request — so JS
 * pre-faults the image header right after registering the disk:  it stages
 * up to DISKIO_FETCH_RUN blocks' worth of leading bytes into preload_stage
 * and calls wasm_diskio_preload().
 *
 * Preloaded blocks are marked CLEAN but are deliberately NOT entered into
 * the clean-block eviction ring: they stay resident for the disk's lifetime
 * (512 KB per disk), because a later hard reset re-opens the image on the
 * main thread and must still find them.
 */

static uint8_t preload_stage[DISKIO_FETCH_RUN * DISKIO_BLOCK_SIZE];

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_diskio_preload_buf(void) { return preload_stage; }

EMSCRIPTEN_KEEPALIVE
int wasm_diskio_preload_max(void) { return (int)sizeof(preload_stage); }

/* Returns the number of leading blocks now resident, 0 on bad args. */
EMSCRIPTEN_KEEPALIVE
int wasm_diskio_preload(int id, int len) {
        uint32_t idx, nblk;
        int resident = 0;

        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return 0;
        if (len <= 0 || len > (int)sizeof(preload_stage))
                return 0;

        pthread_mutex_lock(&table_mutex);
        nblk = (uint32_t)(((uint32_t)len + DISKIO_BLOCK_SIZE - 1) / DISKIO_BLOCK_SIZE);
        for (idx = 0; idx < nblk && idx < disks[id].nblocks; idx++) {
                uint32_t off = idx * DISKIO_BLOCK_SIZE;
                uint32_t n = (uint32_t)len - off;
                uint8_t *p;

                if (n > DISKIO_BLOCK_SIZE)
                        n = DISKIO_BLOCK_SIZE;
                /* Whole-block granularity: a partially-covered block would
                   serve zeros for its uncovered tail. Only the disk's own
                   final (short) block may be partial — its tail is past EOF
                   and reads as zeros anyway, same as diskio_block(). */
                if (n < DISKIO_BLOCK_SIZE && idx != disks[id].nblocks - 1)
                        break;
                if (disks[id].state[idx] != BLK_ABSENT) {
                        resident++; /* already faulted (or dirty) — keep it */
                        continue;
                }
                p = malloc(DISKIO_BLOCK_SIZE);
                if (!p)
                        break;
                if (n < DISKIO_BLOCK_SIZE)
                        memset(p + n, 0, DISKIO_BLOCK_SIZE - n);
                memcpy(p, preload_stage + off, n);
                disks[id].block[idx] = p;
                disks[id].state[idx] = BLK_CLEAN;
                /* no clean_ring_insert(): pinned for the session */
                resident++;
        }
        pthread_mutex_unlock(&table_mutex);
        pclog("diskio: preloaded %i header block(s) for disk %i\n", resident, id);
        return resident;
}

/* ---- API used by hdd_file.c ------------------------------------------------ */

/* Parses "browser:<id>:<name>"; returns id or -1. */
int wasm_diskio_parse(const char *fn) {
        int id;

        if (strncmp(fn, "browser:", 8) != 0)
                return -1;
        id = atoi(fn + 8);
        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use) {
                /* Browsers cannot reopen local files across page loads — the
                   profile remembers the disk but the File handle is gone. */
                MAIN_THREAD_ASYNC_EM_ASM({
                        if (Module.onPCemMessage)
                                Module.onPCemMessage("Disk image needs re-attaching",
                                                     "This system uses a local disk or CD image, and the browser cannot reopen local files automatically after a page reload. Open Configure \u2192 Media and choose the image again.",
                                                     true);
                });
                return -1;
        }
        return id;
}

double wasm_diskio_size(int id) {
        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return 0;
        return disks[id].size;
}

int wasm_diskio_read(int id, uint64_t byte_offset, int len, void *buf) {
        return diskio_rw(id, (double)byte_offset, len, buf, 0);
}

int wasm_diskio_write(int id, uint64_t byte_offset, int len, void *buf) {
        return diskio_rw(id, (double)byte_offset, len, buf, 1);
}

/* ---- export support (browser main thread; pause the machine first) --------- */

EMSCRIPTEN_KEEPALIVE
double wasm_diskio_get_size(int id) { return wasm_diskio_size(id); }

EMSCRIPTEN_KEEPALIVE
int wasm_diskio_nblocks(int id) {
        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return 0;
        return (int)disks[id].nblocks;
}

/* 0 absent, 1 clean (identical to source), 2 dirty (session writes) */
EMSCRIPTEN_KEEPALIVE
int wasm_diskio_block_state(int id, int idx) {
        if (id < 0 || id >= DISKIO_MAX_DISKS || !disks[id].in_use)
                return 0;
        if (idx < 0 || (uint32_t)idx >= disks[id].nblocks)
                return 0;
        return disks[id].state[idx];
}

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_diskio_block_ptr(int id, int idx) {
        if (wasm_diskio_block_state(id, idx) == BLK_ABSENT)
                return NULL;
        return disks[id].block[idx];
}

/* ---- test/debug exports -----------------------------------------------------
   These must run on the EMULATION thread: block faults sleep-wait for the
   browser main thread to service them, so calling the engine directly from
   a main-thread ccall would deadlock. JS posts an op; the emulation thread
   pumps it between bursts; JS polls for the result. */

static volatile int dbg_stage = 0; /* 0 idle, 1 posted, 2 done */
static int dbg_op, dbg_disk, dbg_seed;
static double dbg_lba;
static volatile double dbg_result;

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_debug_disk_op(int op, int id, double lba, int seed) {
        dbg_op = op; /* 0 = checksum sector, 1 = stamp sector */
        dbg_disk = id;
        dbg_lba = lba;
        dbg_seed = seed;
        dbg_result = -1;
        dbg_stage = 1;
}

EMSCRIPTEN_KEEPALIVE
int pcem_wasm_debug_disk_done(void) { return dbg_stage == 2; }

EMSCRIPTEN_KEEPALIVE
double pcem_wasm_debug_disk_result(void) {
        dbg_stage = 0;
        return dbg_result;
}

/* Called from the emulation thread's loop. */
void wasm_diskio_debug_pump(void) {
        uint8_t sector[512];
        int i;

        if (dbg_stage != 1)
                return;
        if (dbg_op == 0) {
                if (wasm_diskio_read(dbg_disk, (uint64_t)dbg_lba * 512, 512, sector)) {
                        dbg_result = -1;
                } else {
                        double sum = 0;
                        for (i = 0; i < 512; i++)
                                sum += sector[i] * (double)(i + 1);
                        dbg_result = sum;
                }
        } else {
                for (i = 0; i < 512; i++)
                        sector[i] = (uint8_t)(dbg_seed + i * 3);
                dbg_result = wasm_diskio_write(dbg_disk, (uint64_t)dbg_lba * 512, 512, sector) ? -1 : 1;
        }
        dbg_stage = 2;
}
