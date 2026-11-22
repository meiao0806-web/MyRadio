// 最小 MQTT-over-WebSocket broker（只在本地验证用，不是生产代码）
// 作用：替身扮演「公网服务器 + 收音机」，让 tools/app/js/cloud.js 在真浏览器里跑通。
//   收 myradio/<id>/c  → 按 src/api.cpp 的语义回 myradio/<id>/r
// 用法：node tools/mqtt_broker_sim.js [端口]
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = +(process.argv[2] || 8899);
// 固件版本别写死：从 src/config.h 的 FW_VER 读，跟真机/说明书同步
const FW = (() => {
  const p = path.join(__dirname, '..', 'src', 'config.h');
  const m = /#define\s+FW_VER\s+"([^"]+)"/.exec(fs.readFileSync(p, 'utf8'));
  return m ? m[1] : '0.0.0';
})();
// ---------- 1.19.0：两台假机器 ----------
// 第一台开着「免配对」（App 用算出来的码就能控），第二台关着（必须手输它的码）。
// 算法与 src/cloud.cpp 的 deriveFrom() / tools/app/js/cloud.js 的 deriveCode() 一致。
function derived(id) {
  const t = 'MyRadio-1' + String(id).toUpperCase();
  let h = 2166136261 >>> 0;
  for (let i = 0; i < t.length; i++) { h ^= t.charCodeAt(i) & 0xFF; h = Math.imul(h, 16777619) >>> 0; }
  return String(100000 + (h % 900000));
}

// 1.46.7：每台多一个 pin（4 位连接码）。第二台故意用 5678 ——
//   这样「输 1234」只会命中第一台，可以验「唯一命中就自动连」；
//   再临时造一台同码的机器，验「多台同码要列出来让用户挑」。
const DEVS = [
  { dev: 'A1B2C3', id: 'A1B2C3D4E5F6', name: 'MyRadio-A1B2C3', code: '482913', open: 1, batt: 82, pin: '1234' },
  { dev: 'FFFFFF', id: 'FFFFFFFFFFFF', name: 'MyRadio-FFFFFF', code: '135790', open: 0, batt: 51, pin: '5678' }
];
const DEVTOPIC = 'myradio/dev/';
const CODE = DEVS[0].code;
const DEVID = DEVS[0].dev;
// 1.15.0：gname 是「当前这台的标签」，不是分组名。下面 SCREEN 里按真台表取。

// ---------- 用真台表造 /api/stations 的响应（顺便测大消息分片）----------
// 1.15.0：固件不再分组，/api/stations 回的是**整张表** + 标签清单，
// 每台多一个 t（标签下标）。这里照 apiStationsJson() 的字段造，别再用 kPresetGrp。
function loadStations() {
  const p = path.join(__dirname, '..', 'src', 'preset_stations.h');
  const s = fs.readFileSync(p, 'utf8');
  const arr = (decl) => new RegExp(decl + '\\[\\] = \\{(.*?)\\n\\};', 's').exec(s)[1];

  const st = arr('static const PresetSt kPreset');
  const rows = [...st.matchAll(/\{\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}/g)]
    .map(m => ({ u: m[1], n: m[2] }));

  const tags = [...arr('static const PresetTag kPresetTag')
    .matchAll(/\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}/g)]
    .map((m, i) => ({ i, n: m[1], c: +m[2] }));

  const tagOf = arr('static const uint8_t kPresetTagOf')
    .match(/\d+/g).map(Number);

  const list = rows.map((r, i) => ({
    i, n: r.n, u: r.u, q: /^\d+$/.test(r.u) ? 1 : 0,
    ep: 0, t: tagOf[i] | 0
  }));
  return { count: list.length, tags, list };
}
const STATIONS = loadStations();
// 屏上顶栏那块 = 当前这台的标签（1.15.0）
const TAG0 = (STATIONS.tags[STATIONS.list[0].t] || {}).n || '';

const SCREEN = {
  on: 1, br: 100, clock: '21:30', wd: 6, grp: 0, gname: TAG0,
  idx: 0, cnt: STATIONS.list.length, name: STATIONS.list[0].n,
  ps: 2, vol: 45, mute: 0, wifi: 1, rssi: -58, mode: 'net',
  fmfreq: 980, fmrssi: 0, pv: -1, learn: 0, pc: 0, tip: '旋钮换台', mu: 0
};

// ---------- 设备语义 ----------
function device(d, cmd) {
  let j;
  try { j = JSON.parse(cmd); } catch (e) { return null; }
  const i = j.i;
  // 1.19.0：自己那串随机码永远认；开了免配对的，算出来的码也认
  const okCode = (j.k === d.code) || (d.open && j.k === derived(d.id));
  if (!okCode) return { i, s: 403, d: { err: 'code' } };
  switch (j.p) {
    case '/api/screen':   return { i, s: 200, d: SCREEN };
    case '/api/stations': return { i, s: 200, d: STATIONS };
    case '/api/vol':      return { i, s: 200, d: { ok: 1, vol: j.b } };
    case '/api/step':     return { i, s: 200, d: { ok: 1, dir: j.b } };
    // 1.15.0：/api/group 已是空壳，恒回 ok:0（照 net.cpp 的 hGroup）
    case '/api/group':    return { i, s: 200, d: { ok: 0, group: 0, msg: '1.15.0 起不再分组' } };
    case '/api/pause':    return { i, s: 200, d: { ok: 1 } };
    case '/api/cloud':    return { i, s: 200, d: { on: 1, conn: 1, devid: d.id,
                                        short: d.dev, code: d.code, open: d.open, pres: 12,
                                        host: 'broker-cn.emqx.io',
                                        port: 1883, cmds: 3, bad: 0 } };
    // 1.19.0：电量
    case '/api/batt':     return { i, s: 200, d: { bok: 1, batt: d.batt, bmv: 3940, chg: 0,
                                        blow: d.batt <= 5 ? 1 : 0, bpin: 4,
                                        vready: 2, vbusy: 0 } };
    case '/api/pair':     return { i, s: 200, d: { locked: 0, code: '' } };
    case '/api/state':    return { i, s: 200, d: { cur: 0, count: STATIONS.count, group: 0, gcount: 1,
                                        ps: 2, vol: 45, mute: 0, bmax: 100, offmin: 3,
                                        on: 1, wifi: 1, bok: 1, batt: d.batt, bmv: 3940,
                                        chg: 0, blow: d.batt <= 5 ? 1 : 0, bpin: 4 } };
    case '/api/diag':     return { i, s: 200, d: { fw: FW, up: 3600, heap: 180000,
                                        fsUsed: 30000, fsTotal: 300000, table: { tags: STATIONS.tags.length, count: STATIONS.count },
                                        wifi: { ok: 1, ssid: 'HOME', ip: '192.168.1.9', rssi: -58, ch: 6 },
                                        audio: { ps: 2, retry: 0, dead: 0, kbps: 64, sec: 120, url: '' },
                                        time: { ok: 1, now: '21:30' }, token: { set: 1, fp: 'abcd' },
                                        pair: { locked: 0 },
                                        batt: { bok: 1, batt: d.batt, bmv: 3940, chg: 0,
                                                blow: d.batt <= 5 ? 1 : 0, bpin: 4 },
                                        voice: { vready: 2, vbusy: 0 } } };
    default:              return { i, s: 400, d: { err: 'path' } };
  }
}

// ---------- 帧 / 编码工具 ----------
const rl = n => { const o = []; do { let d = n % 128; n = Math.floor(n / 128); if (n > 0) d |= 128; o.push(d); } while (n > 0); return Buffer.from(o); };
const mstr = s => { const b = Buffer.from(s); const h = Buffer.alloc(2); h.writeUInt16BE(b.length, 0); return Buffer.concat([h, b]); };

function takeMqtt(buf) {
  if (buf.length < 2) return null;
  let mult = 1, len = 0, i = 1, d;
  do { if (i >= buf.length) return null; d = buf[i++]; len += (d & 127) * mult; mult *= 128; } while (d & 128);
  if (buf.length < i + len) return null;
  return { type: buf[0] >> 4, flags: buf[0] & 15, body: buf.subarray(i, i + len), used: i + len };
}

class Client {
  constructor(socket) {
    this.socket = socket;
    this.ws = Buffer.alloc(0);
    this.mq = Buffer.alloc(0);
    this.subs = [];
    this.closed = false;
    socket.on('data', d => this.onData(d));
    socket.on('error', () => { });
    socket.on('close', () => { this.closed = true; });
  }
  onData(d) {
    this.ws = Buffer.concat([this.ws, d]);
    for (;;) {
      const f = this.frame();
      if (!f) break;
      if (f.op === 2 || f.op === 1) this.onMqtt(f.payload);
      else if (f.op === 8) { this.socket.end(); this.closed = true; }
      else if (f.op === 9) this.send(f.op === 9 ? 0xA : 0xA, f.payload);
    }
  }
  frame() {
    const b = this.ws;
    if (b.length < 2) return null;
    const op = b[0] & 0x0f;
    const masked = (b[1] & 0x80) !== 0;
    let len = b[1] & 0x7f, off = 2;
    if (len === 126) { if (b.length < 4) return null; len = b.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (b.length < 10) return null; len = Number(b.readBigUInt64BE(2)); off = 10; }
    let mask = null;
    if (masked) { if (b.length < off + 4) return null; mask = b.subarray(off, off + 4); off += 4; }
    if (b.length < off + len) return null;
    let p = Buffer.from(b.subarray(off, off + len));
    if (mask) for (let i = 0; i < p.length; i++) p[i] ^= mask[i & 3];
    this.ws = b.subarray(off + len);
    return { op, payload: p };
  }
  // 保留消息：发布时带 retain 位（0x31），订阅时补发
  publishRetained(topic, payload) {
    const body = Buffer.concat([mstr(topic), Buffer.from(payload)]);
    this.send(2, Buffer.concat([Buffer.from([0x31]), rl(body.length), body]));
  }
  send(op, payload) {
    if (this.closed) return;
    const len = payload.length;
    let head;
    if (len < 126) head = Buffer.from([0x80 | op, len]);
    else if (len < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | op; head[1] = 126; head.writeUInt16BE(len, 2); }
    else { head = Buffer.alloc(10); head[0] = 0x80 | op; head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2); }
    this.socket.write(Buffer.concat([head, payload]));
  }
  onMqtt(chunk) {
    this.mq = Buffer.concat([this.mq, chunk]);
    for (;;) {
      const p = takeMqtt(this.mq);
      if (!p) break;
      this.mq = this.mq.subarray(p.used);
      if (p.type === 1) {                                  // CONNECT
        this.send(2, Buffer.from([0x20, 0x02, 0x00, 0x00]));
      } else if (p.type === 8) {                           // SUBSCRIBE
        const pid = p.body.readUInt16BE(0);
        const tl = p.body.readUInt16BE(2);
        const filter = p.body.subarray(4, 4 + tl).toString();
        this.subs.push(filter);
        this.send(2, Buffer.from([0x90, 0x03, pid >> 8, pid & 255, 0x00]));
        // 1.19.0：订阅 myradio/dev/+ 就把「保留的在线广播」补发一遍（真 broker 的行为）
        if (filter === DEVTOPIC + '+' || filter === DEVTOPIC + '#') {
          for (const d of DEVS) this.publishRetained(DEVTOPIC + d.dev, presence(d));
        }
      } else if (p.type === 12) {                          // PINGREQ
        this.send(2, Buffer.from([0xD0, 0x00]));
      } else if (p.type === 3) {                           // PUBLISH
        const tl = p.body.readUInt16BE(0);
        const topic = p.body.subarray(2, 2 + tl).toString();
        const qos = (p.flags >> 1) & 3;
        const off = 2 + tl + (qos > 0 ? 2 : 0);
        const payload = p.body.subarray(off).toString();
        // 1.19.0：主题里带的是短码，按它找机器
        const m = /^myradio\/([0-9A-Z]{6})\/c$/.exec(topic);
        const d = m ? DEVS.find(x => x.dev === m[1]) : null;
        const r = d ? device(d, payload) : null;
        if (r) {
          const rt = topic.replace(/\/c$/, '/r');
          const body = Buffer.concat([mstr(rt), Buffer.from(JSON.stringify(r))]);
          this.send(2, Buffer.concat([Buffer.from([0x30]), rl(body.length), body]));
        }
      }
    }
  }
}

// 在线广播正文：字段与 src/cloud.cpp 的 pubPresence() 一致
function presence(d) {
  // 1.46.7：带 pin（机器那 4 位连接码），与 cloud.cpp 的 pubPresence 字段一致
  return JSON.stringify({ d: d.dev, id: d.id, n: d.name, fw: FW,
                          open: d.open, up: 3600, batt: d.batt, pin: d.pin || '' });
}

const srv = http.createServer((req, res) => { res.writeHead(404); res.end('ws only'); });
srv.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'] || '';
  const acc = crypto.createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    'Sec-WebSocket-Accept: ' + acc + '\r\n' +
    'Sec-WebSocket-Protocol: mqtt\r\n\r\n');
  const c = new Client(socket);
  if (process.env.KICK_AFTER) {         // 用来测「断线重连」
    setTimeout(() => { try { socket.destroy(); } catch (e) { } }, +process.env.KICK_AFTER);
  }
});

srv.listen(PORT, '127.0.0.1', () => {
  console.log('[broker] ws://127.0.0.1:' + PORT + '/mqtt  设备码 ' + DEVID + ' / ' + CODE +
    '  台数 ' + STATIONS.list.length);
});
