/* ============================================================
   cloud.js —— 跨网远程控制（1.8.0）

   为什么需要它：
     1.7.0 及以前，这个 App 只能在家里的 WiFi 里用 —— 因为页面本身就是
     收音机发出来的，离开那个网连页面都打不开。
     1.8.0 起收音机会自己挂到公网 MQTT 服务器上；手机无论在哪个城市，
     只要知道「遥控码」，就能顺着这条路让它换台、调音量、调亮度。

   为什么不用 mqtt.js：
     设备端零外链是这条产品的底线（机器在别人家，CDN 挂了也得能用）。
     这里手写一个 MQTT 3.1.1 over WebSocket，只实现用得到的四个包：
     CONNECT / SUBSCRIBE / PUBLISH / PINGREQ。

   协议（与 src/cloud.cpp 一一对应）：
     发  myradio/<设备码>/c   {"i":1,"k":"482913","m":"POST","p":"/api/vol","b":"v=50"}
     收  myradio/<设备码>/r   {"i":1,"s":200,"d":{"ok":1}}

   1.19.0 加的两件事（"免配对"）：
     ① 发现：机器每 30 秒往 myradio/dev/<短码> 发一条 retain 的"我在线"。
        这里订阅 myradio/dev/+，于是**任何一台手机**（不用是当初配网的那台）
        一打开 App 就能列出家里那台机器。
     ② 免输码：遥控码改由设备码算出来（deriveCode，与 src/cloud.cpp 的
        deriveFrom() 逐字节相同）。机器开着「免配对」时认这个算出来的码，
        所以换手机、重装 App、App 升级，都不用再抄那 12 位。

   !! 顺手修了 1.8.0 的一个错：原来 SUBSCRIBE 订的是 /c（命令主题），
      可回包走的是 /r —— 真 EMQX 上根本收不到（本地假 broker 不看订阅，
      所以测试一直是绿的）。现在订 /r。
   ============================================================ */

const CLOUD = (function () {
  const LSKEY = 'mr_cloud';

  // dev = 6 位设备码，code = 6 位遥控码，两个拼起来就是用户看到的「遥控码」
  // 主题前缀：与 src/config.h 的 CLOUD_PREFIX / CLOUD_DEV_PREFIX 必须一致
  const PREFIX = 'myradio';
  const DEVPRE = PREFIX + '/dev/';

  const S = {
    dev: '', code: '', host: 'broker-cn.emqx.io', wss: 8084, secure: true,
    name: '',           // 这台机器的名字（在线广播里带过来的）
    devs: [],           // 发现的机器：[{dev,n,fw,open,batt,seen}]
    want: false,        // 用户打开过「远程控制」
    conn: false,        // MQTT 已连上
    phase: 'idle',      // idle / connecting / online / err
    err: '',
    rx: 0, tx: 0,       // 收发条数，设置页显示着玩，排障时有用
    down: 0,            // 重连次数
    onDevs: null        // 设备列表变了就调一下（pages.js 挂的）
  };

  let ws = null;
  let rxbuf = new Uint8Array(0);
  let subId = 1;
  let seq = 1;
  const pending = new Map();
  let pingT = 0, retryT = 0, retryMs = 2000;

  const enc = s => new TextEncoder().encode(s);
  const dec = b => new TextDecoder().decode(b);

  /* ---------------- 存 ---------------- */
  function load() {
    try {
      const j = JSON.parse(localStorage.getItem(LSKEY) || '{}');
      if (j.dev)  S.dev  = String(j.dev);
      if (j.code) S.code = String(j.code);
      if (j.host) S.host = String(j.host);
      if (j.name) S.name = String(j.name);
      if (j.wss)  S.wss  = +j.wss || 8084;
      if (j.secure === false) S.secure = false;
      if (j.want) S.want = true;
    } catch (e) {}
    return S;
  }
  function save(patch) {
    if (patch) Object.assign(S, patch);
    try {
      localStorage.setItem(LSKEY, JSON.stringify({
        dev: S.dev, code: S.code, host: S.host, wss: S.wss,
        secure: S.secure, want: S.want, name: S.name
      }));
    } catch (e) {}
  }

  /* ---------------- 遥控码 ---------------- */
  // 用户输入 "A1B2C3-482913"（短码 + 密码），宽容一点：空格、大小写、横线随便
  function setPair(text) {
    const s = String(text || '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
    if (s.length !== 12) return false;
    S.dev = s.slice(0, 6);
    S.code = s.slice(6);
    save();
    return true;
  }
  function pairText() {
    return (S.dev && S.code) ? (S.dev + '-' + S.code) : '';
  }
  function paired() { return !!(S.dev && S.code); }

  /* ---------------- 1.19.0 派生遥控码 ---------------- */
  // 与 src/cloud.cpp 的 deriveFrom() 必须逐字节一致：
  //   FNV-1a 32，先吃盐 "MyRadio-1"，再吃 12 位设备码（大写），取模 900000 加 100000。
  // 测试向量见 tools/verify_cloud.js [10]：A1B2C3D4E5F6 → 274682
  function deriveCode(dev) {
    const t = 'MyRadio-1' + String(dev || '').toUpperCase();
    let h = 2166136261 >>> 0;
    for (let i = 0; i < t.length; i++) {
      h ^= t.charCodeAt(i) & 0xFF;
      h = Math.imul(h, 16777619) >>> 0;
    }
    return String(100000 + (h % 900000));
  }

  /* ---------------- 1.19.0 设备列表 ---------------- */
  // 收到一条保留消息就更新一条。空消息 = 机器自己撤了广播（关了免配对 / 下线清理）。
  function onDev(dev, text) {
    const i = S.devs.findIndex(d => d.dev === dev);
    if (!text) { if (i >= 0) S.devs.splice(i, 1); notifyDevs(); return; }
    let j;
    try { j = JSON.parse(text); } catch (e) { return; }
    if (!j || !j.d) return;
    const rec = {
      dev: dev,
      id: j.id || '',            // 12 位完整设备码：派生码是从它算出来的
      n: j.n || ('MyRadio-' + dev),
      fw: j.fw || '',
      open: j.open === undefined ? 1 : (j.open ? 1 : 0),
      batt: (j.batt === undefined || j.batt === null) ? -1 : +j.batt,
      // 1.46.7：机器那 4 位连接码。1.46.7 之前的固件不发这个字段 —— 那就是空串，
      //   输码找不到时界面要能说清「可能是机器固件太老」。
      pin: j.pin ? String(j.pin).replace(/\D/g, '').slice(0, 4) : '',
      seen: Date.now()
    };
    if (i >= 0) S.devs[i] = rec; else S.devs.push(rec);
    notifyDevs();
  }
  function notifyDevs() {
    if (typeof S.onDevs === 'function') { try { S.onDevs(S.devs); } catch (e) {} }
  }
  // 90 秒没再听到 = 离线（机器每 30 秒报一次）
  function devAlive(d) { return d && (Date.now() - d.seen) < 90000; }
  function findDev(dev) { return S.devs.find(d => d.dev === dev) || null; }

  // 选一台机器来控：免配对的机器直接算码，闭的留空等用户手输
  function pickDev(dev) {
    const d = findDev(dev);
    if (!d) return false;
    const changed = (S.dev !== dev);
    S.dev  = d.dev;
    S.name = d.n || '';
    if (d.open) S.code = deriveCode(d.id || d.dev);
    else if (changed) S.code = '';
    save();
    if (S.want) { if (!S.conn) connect(); else subscribeDev(); }
    return !!S.code;
  }
  function forgetDev() {
    S.dev = ''; S.code = ''; S.name = '';
    save();
  }

  /* ---------------- 1.46.7 输 4 位码连（跨网） ----------------
     机器每 30 秒往 myradio/dev/<短码> 广播一次，payload 里已经带了自己的 4 位码。
     所以跨网的「输码」就是**在已收到的广播里筛**，不用再问谁一句，也不用新主题：
       · 只有一台用这个码  → 直接选它（这就是用户要的手感）
       · 有多台（都用出厂 1234）→ 交给界面列出来点，机器不替人决定
       · 一台都没有        → 可能没上线，也可能固件 < 1.46.7（那些机器不发 pin）
     返回 {n, list, dev?, name?, needCode?}：
       n=1 且 needCode=false → 界面直接进；
       n=1 且 needCode=true  → 这台关了免配对，还得补 12 位码；
       n>1 / n=0             → 界面按 list 显示。 */
  function findByPin(pin) {
    const p = String(pin || '').replace(/\D/g, '').slice(0, 4);
    if (p.length !== 4) return [];
    return S.devs.filter(d => devAlive(d) && d.pin === p)
                 .sort((a, b) => (b.seen || 0) - (a.seen || 0));
  }
  function pinLogin(pin) {
    const hit = findByPin(pin);
    if (hit.length === 1) {
      const has = pickDev(hit[0].dev);       // 免配对机器顺手把派生码填好
      return { n: 1, dev: hit[0].dev, name: hit[0].n, needCode: !has, list: hit };
    }
    return { n: hit.length, list: hit };
  }
  // 界面上「下面这些机器里挑一台」用：挑完就跟点列表一样
  function pickPin(dev) {
    const d = findDev(dev);
    if (!d) return false;
    return pickDev(dev);
  }
  // 有没有任何一台报了码？用来区分「没上线」和「固件太老不发码」
  function anyPin() { return S.devs.some(d => devAlive(d) && d.pin); }

  /* ---------------- MQTT 编码 ---------------- */
  function pkt(type, body) {
    const head = [type & 0xFF];
    let x = body.length;
    do {
      let d = x % 128;
      x = Math.floor(x / 128);
      if (x > 0) d |= 0x80;
      head.push(d);
    } while (x > 0);
    const r = new Uint8Array(head.length + body.length);
    r.set(head, 0);
    r.set(body, head.length);
    return r;
  }
  function mstr(s) {                        // MQTT 里的字符串：2 字节长度 + UTF-8
    const b = enc(s);
    const r = new Uint8Array(2 + b.length);
    r[0] = b.length >> 8;
    r[1] = b.length & 255;
    r.set(b, 2);
    return r;
  }
  function cat(list) {
    let n = 0;
    for (const a of list) n += a.length;
    const r = new Uint8Array(n);
    let o = 0;
    for (const a of list) { r.set(a, o); o += a.length; }
    return r;
  }

  function connectPacket() {
    const cid = 'mrr' + Math.random().toString(36).slice(2, 9);
    return pkt(0x10, cat([
      mstr('MQTT'),
      new Uint8Array([4, 0x02, 0x00, 0x3C]),   // 协议级别 4 / cleanSession / keepalive 60s
      mstr(cid)
    ]));
  }
  function subscribePacket(topic) {
    const id = subId++;
    return pkt(0x82, cat([
      new Uint8Array([id >> 8, id & 255]),
      mstr(topic),
      new Uint8Array([0])                       // QoS 0
    ]));
  }
  function publishPacket(topic, payload) {
    return pkt(0x30, cat([mstr(topic), payload]));
  }
  function pingPacket() { return pkt(0xC0, new Uint8Array(0)); }

  // 从缓冲里切一个完整包；不够就返回 null 等下一片
  function takePacket(buf) {
    if (buf.length < 2) return null;
    let mult = 1, len = 0, i = 1, d;
    do {
      if (i >= buf.length) return null;
      d = buf[i++];
      len += (d & 127) * mult;
      mult *= 128;
      if (mult > 128 * 128 * 128 * 128) return null;
    } while (d & 128);
    if (buf.length < i + len) return null;
    return { type: buf[0] >> 4, flags: buf[0] & 15, body: buf.subarray(i, i + len), used: i + len };
  }

  /* ---------------- 收 ---------------- */
  function push(chunk) {
    const nx = new Uint8Array(rxbuf.length + chunk.length);
    nx.set(rxbuf, 0);
    nx.set(chunk, rxbuf.length);
    rxbuf = nx;

    for (;;) {
      const p = takePacket(rxbuf);
      if (!p) break;
      rxbuf = rxbuf.subarray(p.used);
      handle(p);
    }
    // 万一半包一直拼不齐，别把内存撑爆
    if (rxbuf.length > 200000) rxbuf = new Uint8Array(0);
  }

  function handle(p) {
    if (p.type === 2) {                       // CONNACK
      const rc = p.body.length > 1 ? p.body[1] : 9;
      if (rc === 0) {
        S.conn = true;
        S.phase = 'online';
        S.err = '';
        retryMs = 2000;
        // 1.19.0：订 /r（回包），并订设备广播。1.8.0 误订了 /c。
        // 还没选机器时别订 /r —— 否则订的是 "myradio//r" 这种空设备主题。
        if (S.dev) ws.send(subscribePacket(topicR()));
        ws.send(subscribePacket(DEVPRE + '+'));
        startPing();
      } else {
        S.conn = false;
        S.phase = 'err';
        S.err = rc === 4 ? '遥控码/服务器拒绝了连接' : ('服务器返回 ' + rc);
        try { ws.close(); } catch (e) {}
      }
      return;
    }
    if (p.type === 3) {                       // PUBLISH
      const bl = p.body;
      const tlen = (bl[0] << 8) | bl[1];
      if (2 + tlen > bl.length) return;
      const topic = dec(bl.subarray(2, 2 + tlen));
      let off = 2 + tlen;
      const qos = (p.flags >> 1) & 3;
      if (qos > 0) off += 2;                  // 带 packet id（我们订的是 QoS0，理论上不会来）
      if (off > bl.length) return;
      S.rx++;
      onMessage(topic, dec(bl.subarray(off)));
      return;
    }
    if (p.type === 13) return;                // PINGRESP
    if (p.type === 9) return;                 // SUBACK
  }

  function onMessage(topic, text) {
    // 设备广播（1.19.0）：主题就是设备身份，正文是那台机器的自述
    if (topic.indexOf(DEVPRE) === 0) { onDev(topic.slice(DEVPRE.length), text); return; }
    if (text.indexOf('"off"') >= 0) {         // 遗嘱：机器掉线了
      S.err = '收音机离线了';
      return;
    }
    let j;
    try { j = JSON.parse(text); } catch (e) { return; }
    if (!j || !j.i) return;
    const p = pending.get(j.i);
    if (!p) return;
    pending.delete(j.i);
    clearTimeout(p.t);
    if (j.s === 200) p.resolve(j.d || {});
    else {
      const e = new Error((j.d && j.d.err) || ('err ' + j.s));
      e.code = j.s;
      p.reject(e);
    }
  }

  /* ---------------- 连接 ---------------- */
  const topicC = () => 'myradio/' + S.dev + '/c';
  const topicR = () => 'myradio/' + S.dev + '/r';

  function startPing() {
    stopPing();
    pingT = setInterval(() => {
      if (ws && S.conn) { try { ws.send(pingPacket()); } catch (e) {} }
    }, 30000);
  }
  function stopPing() { if (pingT) clearInterval(pingT); pingT = 0; }

  function connect() {
    // 1.19.0：没配对也连 —— 得先连上才能"看见"机器（设备广播走同一条连接）
    close();
    S.phase = 'connecting';
    S.err = '';
    const url = (S.secure === false ? 'ws://' : 'wss://') + S.host + ':' + S.wss + '/mqtt';
    try {
      ws = new WebSocket(url, ['mqtt']);
    } catch (e) {
      S.phase = 'err';
      S.err = '地址不对';
      return;
    }
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { try { ws.send(connectPacket()); } catch (e) {} };
    ws.onmessage = ev => { try { push(new Uint8Array(ev.data)); } catch (e) {} };
    ws.onerror = () => { if (!S.conn) { S.err = '连不上服务器'; S.phase = 'err'; } };
    ws.onclose = () => {
      S.conn = false;
      stopPing();
      pending.forEach(p => { clearTimeout(p.t); p.reject(new Error('closed')); });
      pending.clear();
      if (S.want) {
        S.phase = 'connecting';
        S.down++;
        clearTimeout(retryT);
        retryT = setTimeout(connect, retryMs);
        retryMs = Math.min(retryMs * 2, 15000);
      } else {
        S.phase = 'idle';
      }
    };
  }

  function close() {
    clearTimeout(retryT);
    retryT = 0;
    stopPing();
    const w = ws;
    ws = null;
    S.conn = false;
    if (w) { try { w.onclose = null; w.close(); } catch (e) {} }
  }

  // 换了要控的机器：补订它的 /r（已订的重复 SUBSCRIBE 无害）
  function subscribeDev() {
    if (!ws || !S.conn || !S.dev) return;
    try { ws.send(subscribePacket(topicR())); } catch (e) {}
  }

  function enable(on) {
    S.want = !!on;
    save();
    if (S.want) connect();
    else { close(); S.phase = 'idle'; }
  }

  /* ---------------- 发请求 ---------------- */
  function request(path, body, method) {
    return new Promise((resolve, reject) => {
      if (!ws || !S.conn) { reject(new Error('offline')); return; }
      if (!S.dev) { reject(new Error('nodev')); return; }   // 还没选机器（1.19.0 会发现阶段）
      const i = seq++;
      const t = setTimeout(() => {
        pending.delete(i);
        reject(new Error('timeout'));
      }, 9000);
      pending.set(i, { resolve, reject, t });

      let b = '';
      if (body && typeof body === 'object') b = new URLSearchParams(body).toString();
      else if (typeof body === 'string') b = body;

      const js = JSON.stringify({ i: i, k: S.code, m: method || 'GET', p: path, b: b });
      try {
        ws.send(publishPacket(topicC(), enc(js)));
        S.tx++;
      } catch (e) {
        clearTimeout(t);
        pending.delete(i);
        reject(e);
      }
    });
  }

  /* ---------------- 给界面看 ---------------- */
  function active() { return S.want && S.conn; }
  function statusText() {
    if (S.phase === 'online' && !S.dev) return '已连服务器，等选机器';
    if (S.phase === 'online' && !S.code) return '这台要输遥控码';
    if (!S.dev) return S.want ? '正在找机器…' : '没开';
    if (S.phase === 'online') return '已连上，随时可控';
    if (S.phase === 'connecting') return '正在连服务器…';
    if (S.phase === 'err') return S.err || '连不上';
    return S.want ? '等 WiFi' : '没开';
  }

  return {
    S: S, load: load, save: save,
    setPair: setPair, pairText: pairText, paired: paired,
    connect: connect, close: close, enable: enable,
    request: request, active: active, statusText: statusText,
    deriveCode: deriveCode, findDev: findDev, devAlive: devAlive,
    pickDev: pickDev, forgetDev: forgetDev, subscribeDev: subscribeDev,
    findByPin: findByPin, pinLogin: pinLogin, pickPin: pickPin, anyPin: anyPin,
    _devpre: DEVPRE,
    // 测试用：拿底层编码结果做断言
    _pkt: pkt, _str: mstr, _take: takePacket, _topicC: topicC, _topicR: topicR
  };
})();
