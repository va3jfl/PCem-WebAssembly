/*
 * wasm/shim/slirp/libslirp.h — the libslirp v4 API surface ne2000.c uses,
 * re-pointed at the browser WebSocket-relay backend (wasm-net.c).
 *
 * PCem's networking (src/networking/ne2000.c) was written against system
 * libslirp: guest TX frames go into slirp_input(), and slirp hands
 * host-side frames back through the SlirpCb.send_packet callback, which
 * ne2000.c queues and its vlan poller delivers into the emulated NIC.
 *
 * In the browser there is no host NAT stack to run — but that exact API is
 * also the shape of an ethernet pipe. wasm-net.c implements this header:
 *
 *   slirp_input()        -> guest frame into a shared-memory TX ring; the
 *                           browser main thread sends it over a WebSocket
 *                           relay (v86/websockproxy protocol: one binary
 *                           message == one raw ethernet frame).
 *   (WS message arrives) -> RX ring; a vlan_handler pump on the emulation
 *                           thread replays it into SlirpCb.send_packet —
 *                           the same path ne2000.c already drains.
 *
 * ne2000.c compiles against this header UNMODIFIED — every type/function
 * it references exists here with libslirp's real signature, so if the core
 * file ever tracks upstream libslirp changes, this shim either still fits
 * or fails loudly at compile time.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifndef WASM_SHIM_LIBSLIRP_H
#define WASM_SHIM_LIBSLIRP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* ssize_t */
#include <netinet/in.h>  /* struct in_addr, struct in6_addr */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Slirp Slirp; /* opaque */

/* poll event bits (values only need to be self-consistent with wasm-net.c) */
#define SLIRP_POLL_IN  (1 << 0)
#define SLIRP_POLL_OUT (1 << 1)
#define SLIRP_POLL_PRI (1 << 2)
#define SLIRP_POLL_ERR (1 << 3)
#define SLIRP_POLL_HUP (1 << 4)

typedef void (*SlirpTimerCb)(void *opaque);
typedef int (*SlirpAddPollCb)(int fd, int events, void *opaque);
typedef int (*SlirpGetREventsCb)(int idx, void *opaque);

typedef struct SlirpCb {
        /* host -> guest frame delivery (ne2000.c queues these) */
        ssize_t (*send_packet)(const void *buf, size_t len, void *opaque);
        void (*guest_error)(const char *msg, void *opaque);
        int64_t (*clock_get_ns)(void *opaque);
        void *(*timer_new)(SlirpTimerCb cb, void *cb_opaque, void *opaque);
        void (*timer_free)(void *timer, void *opaque);
        void (*timer_mod)(void *timer, int64_t expire_time, void *opaque);
        void (*register_poll_fd)(int fd, void *opaque);
        void (*unregister_poll_fd)(int fd, void *opaque);
        void (*notify)(void *opaque);
} SlirpCb;

typedef struct SlirpConfig {
        int version;
        int restricted;
        int in_enabled;
        struct in_addr vnetwork;
        struct in_addr vnetmask;
        struct in_addr vhost;
        int in6_enabled;
        struct in6_addr vprefix_addr6;
        uint8_t vprefix_len;
        struct in6_addr vhost6;
        const char *vhostname;
        const char *tftp_server_name;
        const char *tftp_path;
        const char *bootfile;
        struct in_addr vdhcp_start;
        struct in_addr vnameserver;
        struct in6_addr vnameserver6;
        const char **vdnssearch;
        const char *vdomainname;
        size_t if_mtu;
        size_t if_mru;
        int disable_host_loopback;
} SlirpConfig;

Slirp *slirp_new(const SlirpConfig *cfg, const SlirpCb *callbacks, void *opaque);
void slirp_cleanup(Slirp *slirp);

/* guest -> host: ne2000.c calls this with the raw ethernet frame on TX */
void slirp_input(Slirp *slirp, const uint8_t *pkt, int pkt_len);

int slirp_add_hostfwd(Slirp *slirp, int is_udp, struct in_addr host_addr, int host_port,
                      struct in_addr guest_addr, int guest_port);

void slirp_pollfds_fill(Slirp *slirp, uint32_t *timeout, SlirpAddPollCb add_poll, void *opaque);
void slirp_pollfds_poll(Slirp *slirp, int select_error, SlirpGetREventsCb get_revents, void *opaque);

#ifdef __cplusplus
}
#endif

#endif /* WASM_SHIM_LIBSLIRP_H */
