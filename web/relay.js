// Remote setup transport for the GitHub Pages copy of the setup page.
//
// The display's QR code links here with #r=<room>&k=<key>&b=<broker>. The
// part after '#' never leaves the browser. Each API call is sealed with
// AES-128-GCM under that key and published to the room's MQTT topic; the
// board answers on the reply topic. The MQTT server relays ciphertext only.
// See src/relay.cpp for the board's side; the two must agree on all of this.

const RELAY_BROKERS = {
  h: 'wss://broker.hivemq.com:8884/mqtt',
  m: 'wss://test.mosquitto.org:8081/mqtt',
};
const RELAY_TIMEOUT_MS = 30000;

function b64urlDecode(s) {
  s = s.replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  return Uint8Array.from(atob(s), c => c.charCodeAt(0));
}

// ---------------------------------------------------------------- MQTT 3.1.1, QoS 0

function mqttLen(n) {
  const out = [];
  do { let b = n % 128; n = Math.floor(n / 128); if (n) b |= 128; out.push(b); } while (n);
  return out;
}
function mqttStr(s) {
  const b = new TextEncoder().encode(s);
  return [b.length >> 8, b.length & 255, ...b];
}
function mqttPacket(first, body) {
  return new Uint8Array([first, ...mqttLen(body.length), ...body]);
}

class Mqtt {
  constructor(url, onMessage) { this.url = url; this.onMessage = onMessage; this.ready = null; }

  connect() {
    if (this.ready) return this.ready;
    this.ready = new Promise((resolve, reject) => {
      const ws = new WebSocket(this.url, 'mqtt');
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      this.buf = new Uint8Array(0);
      let subscribed = false;
      const fail = e => { this.ready = null; clearInterval(this.ping); reject(e); };
      ws.onopen = () => {
        const id = 'csp-' + Math.random().toString(36).slice(2, 12);
        ws.send(mqttPacket(0x10, [0, 4, 77, 81, 84, 84, 4, 0x02, 0, 30, ...mqttStr(id)]));
      };
      ws.onerror = () => fail(new Error("Couldn't reach the relay server"));
      ws.onclose = () => { this.ready = null; clearInterval(this.ping); if (!subscribed) fail(new Error('The relay server closed the connection')); };
      ws.onmessage = ev => {
        const add = new Uint8Array(ev.data), b = new Uint8Array(this.buf.length + add.length);
        b.set(this.buf); b.set(add, this.buf.length); this.buf = b;
        for (;;) {
          const p = this.take();
          if (!p) break;
          const type = p.bytes[0] >> 4;
          if (type === 2) {                                   // CONNACK
            if (p.body[1] !== 0) { fail(new Error('Relay server refused: ' + p.body[1])); ws.close(); return; }
            ws.send(mqttPacket(0x82, [0, 1, ...mqttStr(this.topicIn), 0]));
          } else if (type === 9) {                            // SUBACK
            subscribed = true;
            this.ping = setInterval(() => ws.send(new Uint8Array([0xC0, 0])), 20000);
            resolve();
          } else if (type === 3) {                            // PUBLISH
            const qos = (p.bytes[0] >> 1) & 3, tl = p.body[0] << 8 | p.body[1];
            const topic = new TextDecoder().decode(p.body.subarray(2, 2 + tl));
            this.onMessage(topic, p.body.subarray(2 + tl + (qos ? 2 : 0)));
          }
        }
      };
    });
    return this.ready;
  }

  // One whole packet off the front of the buffer, or null.
  take() {
    const b = this.buf;
    let len = 0, mul = 1, i = 1;
    for (;; i++) {
      if (i >= b.length) return null;
      len += (b[i] & 127) * mul; mul *= 128;
      if (!(b[i] & 128)) break;
    }
    const end = i + 1 + len;
    if (b.length < end) return null;
    const p = {bytes: b.subarray(0, end), body: b.subarray(i + 1, end)};
    this.buf = b.slice(end);
    return p;
  }

  publish(topic, payload) {
    this.ws.send(mqttPacket(0x30, [...mqttStr(topic), ...payload]));
  }
}

// ---------------------------------------------------------------- relay

function makeRelay({room, key, wss}) {
  const enc = new TextEncoder(), dec = new TextDecoder();
  const keyP = crypto.subtle.importKey('raw', key, 'AES-GCM', false, ['encrypt', 'decrypt']);
  const pending = new Map();
  let seq = 0;
  const mqtt = new Mqtt(wss, async (topic, data) => {
    if (topic !== 'cs1/' + room + '/a' || data.length < 28) return;
    try {
      const plain = await crypto.subtle.decrypt(
        {name: 'AES-GCM', iv: data.slice(0, 12), additionalData: enc.encode('a' + room)},
        await keyP, data.slice(12));
      const r = JSON.parse(dec.decode(plain)), p = pending.get(r.n);
      if (p) { pending.delete(r.n); p(r); }
    } catch (e) { /* not ours, or tampered with */ }
  });
  mqtt.topicIn = 'cs1/' + room + '/a';

  return async function call(path, body) {
    await mqtt.connect();
    seq = Math.max(Date.now(), seq + 1);                 // always rising, even across reloads
    const req = {n: seq, m: body === undefined ? 'GET' : 'POST', p: path};
    if (body !== undefined) req.b = body;
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const sealed = new Uint8Array(await crypto.subtle.encrypt(
      {name: 'AES-GCM', iv, additionalData: enc.encode('q' + room)}, await keyP, enc.encode(JSON.stringify(req))));
    const n = seq;
    const reply = new Promise((resolve, reject) => {
      pending.set(n, resolve);
      setTimeout(() => {
        if (pending.delete(n)) reject(new Error("The display didn't answer. Is it on, with the remote setup " +
          'code still showing? Its link ends after 15 idle minutes; scan a fresh one.'));
      }, RELAY_TIMEOUT_MS);
    });
    mqtt.publish('cs1/' + room + '/q', [...iv, ...sealed]);
    const r = await reply;
    if (r.s >= 400) throw new Error((r.b && r.b.error) || ('HTTP ' + r.s));
    return r.b || {};
  };
}

function relayFromHash(hash) {
  const q = new URLSearchParams(hash.replace(/^#/, ''));
  const room = q.get('r') || '', k = q.get('k') || '', b = q.get('b') || '';
  const wss = RELAY_BROKERS[b] || (b.startsWith('wss://') ? b : '');
  if (!/^[0-9a-f]{16}$/.test(room) || !wss) return null;
  const key = b64urlDecode(k);
  if (key.length !== 16) return null;
  return makeRelay({room, key, wss});
}

if (typeof window !== 'undefined') {
  window.csRelay = relayFromHash(location.hash);
  if (!window.csRelay) {
    window.csRelay = async () => {
      throw new Error('Open this page by scanning the Remote setup code on the display ' +
        '(hold the display for its menu, then Phone setup, Remote link).');
    };
  }
}
if (typeof module !== 'undefined') module.exports = {makeRelay, relayFromHash, Mqtt};
