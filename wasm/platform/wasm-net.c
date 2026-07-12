/*
 * wasm-net.c — WebSocket-relay ethernet backend behind the libslirp shim.
 *
 * The emulated NIC (core/src/networking/ne2000.c, compiled UNMODIFIED) talks
 * libslirp; this file implements that API over two lock-free SPSC rings in
 * shared memory, and the browser main thread (web/js/net.js) moves frames
 * between the rings and a WebSocket relay speaking the v86/websockproxy
 * protocol — one binary message per raw ethernet frame, every client on the
 * same relay effectively plugged into the same hub. Multiplayer between
 * browsers is "point both machines at the same relay"; the stock relay
 * image also answers DHCP and NATs to the internet.
 *
 *   guest OUT: ne2000.c -> slirp_input()            [emulation thread]
 *                -> TX ring -> poke main thread -> ws.send()
 *   guest IN : ws.onmessage -> wasm_net_rx_push()   [browser main thread]
 *                -> RX ring -> wasm_net_pump() (vlan_handler, emulation
 *                thread) -> SlirpCb.send_packet -> ne2000.c's queue/poller
 *
 * Threading contract, same discipline as wasm-diskio.c / wasm-audio.c:
 * each ring has exactly one producer and one consumer thread; head/tail
 * are C11 atomics; frame payloads never straddle a torn read because an
 * index is published only after its bytes are written.
 *
 * If networking is disabled for the boot (no card, backend "off"), JS never
 * arms the backend and slirp_new() returns NULL — ne2000.c then runs with
 * net_slirp_inited = 0, which is PCem's own "no backend" mode (the guest
 * still sees the card; the cable is unplugged).
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include <slirp/libslirp.h>
#include "nethandler.h" /* vlan_handler() — pump registration */
#include "queue.h"      /* queueADT / QueuePeek — ne2000's frame queue */
#include "ibm.h"        /* pclog */

/* ---- rings ------------------------------------------------------------------
 * Byte rings carrying [u16 little-endian length][payload] records. 256 KB
 * holds ~170 full-size frames a side; a guest or relay burst beyond that is
 * dropped whole (ethernet is lossy; the counters make it visible). Payload
 * bytes wrap around the ring edge; indices are free-running uint32.
 */
#define NET_RING_SIZE (256 * 1024) /* power of two */
#define NET_RING_MASK (NET_RING_SIZE - 1)
#define NET_FRAME_MAX 1600         /* > 1522 (802.1Q max) */
#define NET_FRAME_MIN 14           /* ethernet header */

typedef struct {
        uint8_t buf[NET_RING_SIZE];
        _Atomic uint32_t head; /* producer writes */
        _Atomic uint32_t tail; /* consumer reads  */
} net_ring_t;

static net_ring_t tx_ring; /* guest -> relay: producer emu thread, consumer main thread */
static net_ring_t rx_ring; /* relay -> guest: producer main thread, consumer emu thread */

/* stats (relaxed; read from JS for the status pill and the tests) */
static _Atomic uint32_t st_tx_frames, st_tx_drops;
static _Atomic uint32_t st_rx_frames, st_rx_drops, st_rx_delivered;
static _Atomic uint32_t st_link; /* JS-set: 0 down, 1 up */

/* backend arming: JS sets this BEFORE pcem starts the machine */
static _Atomic int net_enabled = 0;

/* the one live "slirp" instance (ne2000.c creates/destroys per machine init) */
static const SlirpCb *net_cb = NULL;
static void *net_cb_opaque = NULL;
static struct Slirp {
        int alive;
} net_instance;

extern int net_slirp_inited; /* ne2000.c: set once its queue exists */

static void ring_reset(net_ring_t *r) {
        atomic_store(&r->head, 0);
        atomic_store(&r->tail, 0);
}

static uint32_t ring_used(net_ring_t *r) {
        return atomic_load(&r->head) - atomic_load(&r->tail);
}

/* copy in/out with wraparound */
static void ring_write_bytes(net_ring_t *r, uint32_t pos, const uint8_t *src, uint32_t len) {
        uint32_t off = pos & NET_RING_MASK;
        uint32_t first = NET_RING_SIZE - off;
        if (first > len)
                first = len;
        memcpy(r->buf + off, src, first);
        if (len > first)
                memcpy(r->buf, src + first, len - first);
}

static void ring_read_bytes(net_ring_t *r, uint32_t pos, uint8_t *dst, uint32_t len) {
        uint32_t off = pos & NET_RING_MASK;
        uint32_t first = NET_RING_SIZE - off;
        if (first > len)
                first = len;
        memcpy(dst, r->buf + off, first);
        if (len > first)
                memcpy(dst + first, r->buf, len - first);
}

/* single-producer push; returns 0 when the frame doesn't fit (dropped) */
static int ring_push(net_ring_t *r, const uint8_t *frame, uint32_t len) {
        uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        uint32_t need = 2 + len;

        if (NET_RING_SIZE - (head - tail) < need)
                return 0;

        uint8_t hdr[2] = {(uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
        ring_write_bytes(r, head, hdr, 2);
        ring_write_bytes(r, head + 2, frame, len);
        atomic_store_explicit(&r->head, head + need, memory_order_release);
        return 1;
}

/* single-consumer pop into dst (>= NET_FRAME_MAX); returns length or 0 */
static uint32_t ring_pop(net_ring_t *r, uint8_t *dst) {
        uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
        uint8_t hdr[2];
        uint32_t len;

        if (head == tail)
                return 0;

        ring_read_bytes(r, tail, hdr, 2);
        len = hdr[0] | (hdr[1] << 8);
        if (len > NET_FRAME_MAX) {
                /* corrupt record — should be impossible; resync by draining */
                atomic_store_explicit(&r->tail, head, memory_order_release);
                return 0;
        }
        ring_read_bytes(r, tail + 2, dst, len);
        atomic_store_explicit(&r->tail, tail + 2 + len, memory_order_release);
        return len;
}

/* ---- JS-facing exports (browser main thread) -------------------------------- */

/* Arm/disarm the backend for the NEXT machine init. JS calls this before
   Module.start() so ne2000's device init sees the right answer. */
EMSCRIPTEN_KEEPALIVE
void wasm_net_set_enabled(int on) { atomic_store(&net_enabled, on ? 1 : 0); }

EMSCRIPTEN_KEEPALIVE
void wasm_net_set_link(int up) { atomic_store(&st_link, up ? 1u : 0u); }

/* Cheap C-side check for main_tick(): anything for JS to send? */
int wasm_net_tx_pending(void) { return net_instance.alive && ring_used(&tx_ring) != 0; }

static uint8_t tx_stage[NET_FRAME_MAX];
static uint8_t rx_stage[NET_FRAME_MAX];

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_net_tx_buf(void) { return tx_stage; }
EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_net_rx_buf(void) { return rx_stage; }
EMSCRIPTEN_KEEPALIVE
int wasm_net_frame_max(void) { return NET_FRAME_MAX; }

/* Pop one guest frame into the TX stage buffer; returns its length, 0 = none.
   Main thread only. */
EMSCRIPTEN_KEEPALIVE
int wasm_net_tx_pop(void) { return (int)ring_pop(&tx_ring, tx_stage); }

/* A relay frame arrived: JS staged `len` bytes into rx_stage. Main thread. */
EMSCRIPTEN_KEEPALIVE
int wasm_net_rx_push(int len) {
        if (!net_instance.alive || len < NET_FRAME_MIN || len > NET_FRAME_MAX) {
                atomic_fetch_add(&st_rx_drops, 1);
                return 0;
        }
        if (!ring_push(&rx_ring, rx_stage, (uint32_t)len)) {
                atomic_fetch_add(&st_rx_drops, 1);
                return 0;
        }
        atomic_fetch_add(&st_rx_frames, 1);
        return 1;
}

EMSCRIPTEN_KEEPALIVE uint32_t wasm_net_stat_tx(void) { return atomic_load(&st_tx_frames); }
EMSCRIPTEN_KEEPALIVE uint32_t wasm_net_stat_tx_drops(void) { return atomic_load(&st_tx_drops); }
EMSCRIPTEN_KEEPALIVE uint32_t wasm_net_stat_rx(void) { return atomic_load(&st_rx_frames); }
EMSCRIPTEN_KEEPALIVE uint32_t wasm_net_stat_rx_drops(void) { return atomic_load(&st_rx_drops); }
EMSCRIPTEN_KEEPALIVE uint32_t wasm_net_stat_rx_delivered(void) { return atomic_load(&st_rx_delivered); }
EMSCRIPTEN_KEEPALIVE int wasm_net_active(void) { return net_instance.alive; }

/* ---- test/debug exports ------------------------------------------------------
 * wasm_net_debug_tx: emit a synthetic broadcast frame through the exact
 * guest-TX path (ring -> WS), without needing a guest driver. Test-only:
 * called from the main thread while the guest is idle, so the SPSC
 * single-producer rule isn't violated in practice.
 * The rx checksum lets the peer's test assert byte-exact delivery.
 */
static _Atomic uint32_t dbg_last_rx_sum;
static _Atomic uint32_t dbg_last_rx_len;

EMSCRIPTEN_KEEPALIVE
int wasm_net_debug_tx(int seed, int len) {
        uint8_t frame[NET_FRAME_MAX];
        int i;

        if (len < 60)
                len = 60;
        if (len > NET_FRAME_MAX)
                len = NET_FRAME_MAX;
        memset(frame, 0xff, 6);            /* broadcast dst */
        for (i = 0; i < 6; i++)
                frame[6 + i] = (uint8_t)(seed + i); /* src */
        frame[12] = 0x88; frame[13] = 0xb5;         /* experimental ethertype */
        for (i = 14; i < len; i++)
                frame[i] = (uint8_t)(seed + i * 3);

        slirp_input(net_instance.alive ? &net_instance : NULL, frame, len);
        return len;
}

EMSCRIPTEN_KEEPALIVE
uint32_t wasm_net_debug_rx_sum(void) { return atomic_load(&dbg_last_rx_sum); }
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_net_debug_rx_len(void) { return atomic_load(&dbg_last_rx_len); }

/* Reference checksum for what wasm_net_debug_tx(seed,len) emits. */
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_net_debug_tx_sum(int seed, int len) {
        uint32_t sum = 0;
        int i;
        uint8_t b;

        if (len < 60)
                len = 60;
        if (len > NET_FRAME_MAX)
                len = NET_FRAME_MAX;
        for (i = 0; i < len; i++) {
                if (i < 6)
                        b = 0xff;
                else if (i < 12)
                        b = (uint8_t)(seed + (i - 6));
                else if (i == 12)
                        b = 0x88;
                else if (i == 13)
                        b = 0xb5;
                else
                        b = (uint8_t)(seed + i * 3);
                sum = sum * 31 + b;
        }
        return sum;
}

/* ---- the RX pump (emulation thread, via nethandler's vlan timer) ------------- */

#define NET_PUMP_MAX_PER_TICK 8   /* vlan timer runs ~12 kHz; 8/tick ≈ 96k f/s cap */
#define NET_QUEUE_HEADROOM 90     /* ne2000's queue caps at 100 and leaks on overflow */

extern queueADT slirpq; /* ne2000.c's frame queue — NULL until its init finished */

static void wasm_net_pump(void *priv) {
        static uint8_t frame[NET_FRAME_MAX];
        int n = 0;

        (void)priv;
        if (!net_instance.alive || !net_cb || !net_slirp_inited || !slirpq)
                return;

        while (n < NET_PUMP_MAX_PER_TICK && QueuePeek(slirpq) < NET_QUEUE_HEADROOM) {
                uint32_t len = ring_pop(&rx_ring, frame);
                uint32_t sum = 0;
                uint32_t i;

                if (!len)
                        break;
                for (i = 0; i < len; i++)
                        sum = sum * 31 + frame[i];
                atomic_store(&dbg_last_rx_sum, sum);
                atomic_store(&dbg_last_rx_len, len);

                net_cb->send_packet(frame, len, net_cb_opaque);
                atomic_fetch_add(&st_rx_delivered, 1);
                n++;
        }
}

/* ---- libslirp API (what ne2000.c actually calls) ------------------------------ */

Slirp *slirp_new(const SlirpConfig *cfg, const SlirpCb *callbacks, void *opaque) {
        (void)cfg;

        if (!atomic_load(&net_enabled)) {
                pclog("wasm-net: backend disabled for this boot (no relay) — NIC runs unplugged\n");
                return NULL;
        }

        net_cb = callbacks;
        net_cb_opaque = opaque;
        ring_reset(&tx_ring);
        ring_reset(&rx_ring);
        net_instance.alive = 1;

        /* Deliver relay frames on the same cadence ne2000 polls its queue.
           vlan_reset() zeroed the handler table just before this device
           init, so registration here never accumulates stale entries. */
        vlan_handler(wasm_net_pump, NULL);

        pclog("wasm-net: WebSocket relay backend armed (frames <-> browser rings)\n");
        return &net_instance;
}

void slirp_cleanup(Slirp *slirp) {
        if (slirp != &net_instance)
                return;
        net_instance.alive = 0;
        net_cb = NULL;
        net_cb_opaque = NULL;
        ring_reset(&tx_ring);
        ring_reset(&rx_ring);
        pclog("wasm-net: backend closed\n");
}

void slirp_input(Slirp *slirp, const uint8_t *pkt, int pkt_len) {
        uint32_t was_empty;

        if (slirp != &net_instance || !net_instance.alive)
                return;
        if (pkt_len < NET_FRAME_MIN || pkt_len > NET_FRAME_MAX) {
                atomic_fetch_add(&st_tx_drops, 1);
                return;
        }

        was_empty = ring_used(&tx_ring) == 0;
        if (!ring_push(&tx_ring, pkt, (uint32_t)pkt_len)) {
                atomic_fetch_add(&st_tx_drops, 1);
                return;
        }
        atomic_fetch_add(&st_tx_frames, 1);

        /* Wake the main thread the moment a burst starts; main_tick()'s
           wasm_net_tx_pending() poll is the backstop (same pattern as the
           diskio mailbox). */
        if (was_empty) {
                MAIN_THREAD_ASYNC_EM_ASM({
                        if (Module.__pcemNetTx)
                                Module.__pcemNetTx();
                });
        }
}

int slirp_add_hostfwd(Slirp *slirp, int is_udp, struct in_addr host_addr, int host_port,
                      struct in_addr guest_addr, int guest_port) {
        /* No host TCP/IP stack to forward through — the relay is L2. */
        (void)slirp; (void)is_udp; (void)host_addr; (void)host_port;
        (void)guest_addr; (void)guest_port;
        return 0;
}

void slirp_pollfds_fill(Slirp *slirp, uint32_t *timeout, SlirpAddPollCb add_poll, void *opaque) {
        /* Nothing to poll: frames arrive by push. */
        (void)slirp; (void)add_poll; (void)opaque;
        if (timeout)
                *timeout = 0;
}

void slirp_pollfds_poll(Slirp *slirp, int select_error, SlirpGetREventsCb get_revents, void *opaque) {
        (void)select_error; (void)get_revents; (void)opaque;
        /* ne2000's slirp_tic() calls this ~10x/s; use it as a second pump
           site so delivery still works even if a future core drops the
           vlan_handler registration path. */
        if (slirp == &net_instance)
                wasm_net_pump(NULL);
}
