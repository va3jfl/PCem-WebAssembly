// ============================================================================
// net.js — WebSocket ethernet relay client (the browser half of wasm-net.c).
//
// Protocol: the v86/websockproxy convention — one binary WebSocket message is
// one raw ethernet frame, and every client connected to the same relay is on
// the same virtual LAN (the relay is a hub). That makes multiplayer "point
// both machines at the same relay URL": IPX games see each other directly,
// and the stock relay images also run DHCP+NAT so guests can reach the
// internet (slowly — it's a public hub, not a datacenter).
//
// Frame flow (see wasm-net.c for the C half):
//   guest TX: NE2000 -> slirp_input() -> TX ring -> Module.__pcemNetTx (poke
//             or main_tick backstop) -> here -> ws.send()
//   guest RX: ws.onmessage -> stage buffer -> wasm_net_rx_push() -> RX ring
//             -> emulation-thread pump -> NE2000
//
// The default public relay is the one the v86 project has used for years.
// Per-profile settings (machine editor → Network tab):
//   net_mode: 'relay'  — use DEFAULT_RELAY (the baked-in public hub)
//             'custom' — use net_url (self-hosted: tools/net_relay.mjs, or
//                        any websockproxy/wsnic-compatible relay)
//             'local'  — tabs in THIS browser share a LAN over a
//                        BroadcastChannel: no server, no relay, no network.
//                        (Same origin + same browser profile; the channel
//                        never echoes to the sender — true hub semantics.)
//             'off'    — NIC present but unplugged (no transport at all)
// ============================================================================
import { toast } from './ui-toast.js';

export const DEFAULT_RELAY = 'wss://relay.widgetry.org/';
const LOCAL_CHANNEL = 'pcem-web.lan.v1';
const PEER_TTL_MS = 7000; // heartbeats every 2.5 s; peers expire after this

class NetService {
  constructor() {
    this.M = null;
    this.ws = null;
    this.bc = null;           // BroadcastChannel (net_mode 'local')
    this.mode = 'off';
    this.armed = false;       // this boot wants a transport
    this.url = '';
    this.status = 'off';      // off | connecting | online | error
    this.onStatus = () => {};
    this._retryMs = 1000;
    this._retryTimer = 0;
    this._stopped = true;
    this._warnedOnce = false;
    this._peerId = Math.random().toString(36).slice(2, 10);
    this._peers = new Map();  // id -> lastSeen (local mode presence)
    this._hbTimer = 0;
  }

  attach(Module) {
    this.M = Module;
    // TX drain: invoked by the async poke on the idle->busy edge and by the
    // main-thread tick backstop while frames are queued.
    Module.__pcemNetTx = () => this._drainTx();
  }

  /** Decide + arm the C backend for the machine that is about to start.
      MUST run before Module.start() — ne2000's init asks wasm-net.c whether
      a backend exists at device-init time. */
  configureForBoot(profile) {
    const card = profile.netcard || '';
    this.mode = profile.net_mode || 'relay';
    this.armed = !!card && this.mode !== 'off';
    this.url = this.mode === 'custom' && profile.net_url ? profile.net_url : DEFAULT_RELAY;
    this.M.ccall('wasm_net_set_enabled', null, ['number'], [this.armed ? 1 : 0]);
  }

  /** Called after a successful start. Opens the transport (async). */
  machineStarted() {
    this._stopped = false;
    this._warnedOnce = false;
    this._retryMs = 1000;
    if (!this.armed) { this._setStatus('off'); return; }
    if (this.mode === 'local') this._connectLocal();
    else this._connect();
  }

  /** Called when the machine stops (user stop or core-initiated). */
  machineStopped() {
    this._stopped = true;
    this.armed = false;
    clearTimeout(this._retryTimer);
    clearInterval(this._hbTimer);
    this._retryTimer = 0;
    this._hbTimer = 0;
    if (this.ws) {
      try { this.ws.close(); } catch (e) {}
      this.ws = null;
    }
    if (this.bc) {
      try { this.bc.postMessage({ t: 'bye', id: this._peerId }); this.bc.close(); } catch (e) {}
      this.bc = null;
    }
    this._peers.clear();
    this.M?.ccall('wasm_net_set_enabled', null, ['number'], [0]);
    this._setStatus('off');
  }

  stats() {
    if (!this.M || !this.M.ccall) return null;
    try {
      return {
        active: !!this.M.ccall('wasm_net_active', 'number', [], []),
        tx: this.M.ccall('wasm_net_stat_tx', 'number', [], []) >>> 0,
        rx: this.M.ccall('wasm_net_stat_rx', 'number', [], []) >>> 0,
        rx_delivered: this.M.ccall('wasm_net_stat_rx_delivered', 'number', [], []) >>> 0,
        tx_drops: this.M.ccall('wasm_net_stat_tx_drops', 'number', [], []) >>> 0,
        rx_drops: this.M.ccall('wasm_net_stat_rx_drops', 'number', [], []) >>> 0,
        status: this.status,
        mode: this.mode,
        url: this.armed ? (this.mode === 'local' ? 'tabs in this browser' : this.url) : '',
        peers: this.mode === 'local' ? this._livePeerCount() : null,
      };
    } catch (e) { return null; }
  }

  _livePeerCount() {
    const now = performance.now();
    for (const [id, seen] of this._peers) if (now - seen > PEER_TTL_MS) this._peers.delete(id);
    return this._peers.size;
  }

  // ---- internals -------------------------------------------------------------

  _setStatus(s) {
    if (this.status === s) return;
    this.status = s;
    this.M?.ccall('wasm_net_set_link', null, ['number'], [s === 'online' ? 1 : 0]);
    this.onStatus(s);
  }

  /** net_mode 'local': a BroadcastChannel is the hub. Always "online" —
      there is nothing to connect to; frames reach whoever else is armed. */
  _connectLocal() {
    if (this._stopped || !this.armed) return;
    this.bc = new BroadcastChannel(LOCAL_CHANNEL);
    this.bc.onmessage = (ev) => {
      const d = ev.data;
      if (d && d.t) { // presence beacon
        if (d.t === 'bye') this._peers.delete(d.id);
        else if (d.id) this._peers.set(d.id, performance.now());
        return;
      }
      // ethernet frame (Uint8Array through structured clone)
      const data = d instanceof Uint8Array ? d : new Uint8Array(d);
      const max = this.M.ccall('wasm_net_frame_max', 'number', [], []);
      if (data.length < 14 || data.length > max) return;
      const buf = this.M.ccall('wasm_net_rx_buf', 'number', [], []);
      this.M.HEAPU8.set(data, buf);
      this.M.ccall('wasm_net_rx_push', 'number', ['number'], [data.length]);
    };
    this.bc.postMessage({ t: 'hb', id: this._peerId });
    this._hbTimer = setInterval(() => {
      try { this.bc?.postMessage({ t: 'hb', id: this._peerId }); } catch (e) {}
    }, 2500);
    this._setStatus('online');
    this._drainTx();
  }

  _connect() {
    if (this._stopped || !this.armed) return;
    this._setStatus('connecting');
    let ws;
    try {
      ws = new WebSocket(this.url);
    } catch (e) {
      this._connectFailed(String(e.message || e));
      return;
    }
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => {
      this._retryMs = 1000;
      this._setStatus('online');
      this._drainTx(); // anything queued while we were connecting
    };
    ws.onmessage = (ev) => {
      const data = new Uint8Array(ev.data);
      const max = this.M.ccall('wasm_net_frame_max', 'number', [], []);
      if (data.length < 14 || data.length > max) return;
      const buf = this.M.ccall('wasm_net_rx_buf', 'number', [], []);
      this.M.HEAPU8.set(data, buf);
      this.M.ccall('wasm_net_rx_push', 'number', ['number'], [data.length]);
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      if (!this._stopped && this.armed) this._connectFailed('connection closed');
    };
    ws.onerror = () => { /* onclose follows with the real disposition */ };
  }

  _connectFailed(why) {
    this._setStatus('error');
    if (!this._warnedOnce) {
      this._warnedOnce = true;
      toast('Network relay unreachable',
            `Could not reach ${this.url} (${why}). The machine keeps running with its ` +
            'network cable "unplugged"; reconnecting in the background. For a private LAN, ' +
            'run `node tools/net_relay.mjs` and point the system at ws://localhost:8087/.',
            true);
    }
    if (this._stopped) return;
    clearTimeout(this._retryTimer);
    this._retryTimer = setTimeout(() => this._connect(), this._retryMs);
    this._retryMs = Math.min(this._retryMs * 2, 30000);
  }

  _drainTx() {
    const M = this.M;
    if (!M) return;
    // Pop everything that's queued; frames are dropped (counted) if no
    // transport is up — same as a NIC with no link.
    for (;;) {
      const len = M.ccall('wasm_net_tx_pop', 'number', [], []);
      if (!len) break;
      const up = this.bc || (this.ws && this.ws.readyState === 1);
      if (!up) continue; // drain + drop
      const buf = M.ccall('wasm_net_tx_buf', 'number', [], []);
      // copy out of the SharedArrayBuffer-backed heap — neither
      // WebSocket.send() nor structured clone accepts SAB views
      const frame = new Uint8Array(len);
      frame.set(M.HEAPU8.subarray(buf, buf + len));
      try {
        if (this.bc) this.bc.postMessage(frame);
        else this.ws.send(frame);
      } catch (e) { /* racing close */ }
    }
  }
}

export const net = new NetService();
