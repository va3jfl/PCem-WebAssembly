#!/usr/bin/env node
// ============================================================================
// net_relay.mjs — a tiny self-hostable ethernet-over-WebSocket relay (hub).
//
// Speaks the v86/websockproxy protocol PCem-web's networking uses: every
// binary WebSocket message is one raw ethernet frame, and each frame is
// forwarded to every OTHER connected client — a virtual hub. Point two or
// more machines (PCem-web "Custom relay URL", v86, jor1k, ...) at the same
// instance and they share a LAN: IPX games, NetBIOS, the lot. No DHCP/NAT
// here — for guest internet use the stock websockproxy docker image, or
// give the guests static IPs / IPX (which needs no configuration at all).
//
// Zero dependencies (hand-rolled RFC 6455: no extensions, no compression;
// fragmented frames are reassembled; frames are capped at 4 KB — ethernet
// tops out at 1522 bytes anyway).
//
//   node tools/net_relay.mjs [port]     # default 8087 -> ws://host:8087/
//
// Also used as the fixture for tools/net_test.mjs, so the regression suite
// never depends on a public relay being reachable.
// ============================================================================
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';

const PORT = +(process.argv[2] || process.env.PORT || 8087);
const MAX_FRAME = 4096;
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const clients = new Set(); // sockets with completed handshakes

function wsAccept(key) {
  return createHash('sha1').update(key + GUID).digest('base64');
}

/** Build one unmasked binary server->client frame. */
function encodeFrame(payload) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x82, len]);
  } else { // 126..65535 (MAX_FRAME keeps us far below)
    header = Buffer.alloc(4);
    header[0] = 0x82;
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  }
  return Buffer.concat([header, payload]);
}

function broadcast(from, payload) {
  const frame = encodeFrame(payload);
  for (const sock of clients) {
    if (sock !== from && !sock.destroyed) sock.write(frame);
  }
}

const server = createServer((req, res) => {
  res.writeHead(426, { 'Content-Type': 'text/plain' });
  res.end('This is a WebSocket ethernet relay (upgrade required).\n');
});

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return;
  }
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    'Sec-WebSocket-Accept: ' + wsAccept(key) + '\r\n\r\n');

  socket.setNoDelay(true);
  clients.add(socket);
  const peer = `${socket.remoteAddress}:${socket.remotePort}`;
  log(`+ ${peer} connected (${clients.size} on the hub)`);

  let buf = Buffer.alloc(0);
  let msg = [];      // fragments of the current message
  let msgOp = 0;

  const close = (why) => {
    if (!clients.has(socket)) return;
    clients.delete(socket);
    log(`- ${peer} left (${why}; ${clients.size} on the hub)`);
    socket.destroy();
  };

  socket.on('data', (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;

    for (;;) {
      if (buf.length < 2) return;
      const fin = (buf[0] & 0x80) !== 0;
      const op = buf[0] & 0x0f;
      const masked = (buf[1] & 0x80) !== 0;
      let len = buf[1] & 0x7f;
      let off = 2;

      if (len === 126) {
        if (buf.length < 4) return;
        len = buf.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        if (buf.length < 10) return;
        const big = buf.readBigUInt64BE(2);
        if (big > BigInt(MAX_FRAME)) return close('oversized frame');
        len = Number(big);
        off = 10;
      }
      if (len > MAX_FRAME) return close('oversized frame');
      if (!masked) return close('unmasked client frame'); // RFC 6455 §5.1

      if (buf.length < off + 4 + len) return;
      const mask = buf.subarray(off, off + 4);
      const payload = Buffer.allocUnsafe(len);
      for (let i = 0; i < len; i++) payload[i] = buf[off + 4 + i] ^ mask[i & 3];
      buf = buf.subarray(off + 4 + len);

      switch (op) {
        case 0x0: // continuation
          if (!msgOp) return close('stray continuation');
          msg.push(payload);
          break;
        case 0x1: case 0x2: // text/binary
          msg = [payload];
          msgOp = op;
          break;
        case 0x8: return close('client close');
        case 0x9: socket.write(encodePong(payload)); continue; // ping
        case 0xa: continue; // pong
        default: return close(`bad opcode ${op}`);
      }

      if (fin) {
        const whole = msg.length === 1 ? msg[0] : Buffer.concat(msg);
        msg = []; msgOp = 0;
        // relay only plausible ethernet frames
        if (whole.length >= 14 && whole.length <= MAX_FRAME) broadcast(socket, whole);
      }
    }
  });

  socket.on('error', () => close('socket error'));
  socket.on('end', () => close('hangup'));
});

function encodePong(payload) {
  return Buffer.concat([Buffer.from([0x8a, payload.length]), payload]);
}

function log(s) {
  if (!process.env.QUIET) console.log(`[relay] ${s}`);
}

server.listen(PORT, () => {
  log(`ethernet hub listening on ws://0.0.0.0:${PORT}/`);
  log('point every machine that should share a LAN at this URL');
});
