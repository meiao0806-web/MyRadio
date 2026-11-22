// 在真浏览器里验 1.8.0 的跨网通道：cloud.js + 本地假 broker + 假设备
// 零依赖：Node 22 自带 WebSocket / fetch / child_process
const { fork } = require('child_process');
const path = require('path');

const PORT = 9333;
const BROKER = 8899;
const DIR = __dirname;
const FIX = 'file:///' + path.join(DIR, 'fixture_cloud.html').replace(/\\/g, '/');

let fails = 0, oks = 0;
const ok = (c, m) => { c ? (oks++, console.log('  ✓ ' + m)) : (fails++, console.log('  ✗ ' + m)); };
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function listPage() {
  for (let i = 0; i < 40; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const j = await r.json();
      const p = j.find(x => x.type === 'page');
      if (p && p.webSocketDebuggerUrl) return p.webSocketDebuggerUrl;
    } catch (e) { }
    await sleep(500);
  }
  throw new Error('CDP 端口没起来');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const pending = new Map(); let seq = 1;
    ws.addEventListener('message', ev => {
      let m; try { m = JSON.parse(String(ev.data)); } catch (e) { return; }
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id); pending.delete(m.id);
        m.error ? p.reject(new Error(JSON.stringify(m.error))) : p.resolve(m.result);
      }
    });
    ws.addEventListener('open', () => resolve({
      send(method, params) {
        const id = seq++;
        return new Promise((res, rej) => { pending.set(id, { resolve: res, reject: rej }); ws.send(JSON.stringify({ id, method, params })); });
      },
      close() { try { ws.close(); } catch (e) { } }
    }));
    ws.addEventListener('error', e => reject(new Error('ws: ' + (e.message || ''))));
  });
}

(async () => {
  const broker = fork(path.join(DIR, 'mqtt_broker_sim.js'), [String(BROKER)], { stdio: 'pipe' });
  broker.stdout.on('data', d => process.stdout.write('    [broker] ' + d));
  await sleep(600);

  const wsUrl = await listPage();
  const cdp = await connect(wsUrl);
  async function ev(expr) {
    const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) {
      const d = r.exceptionDetails.exception;
      throw new Error('页面内异常：' + ((d && (d.description || d.value)) || JSON.stringify(r.exceptionDetails)));
    }
    return r.result.value;
  }

  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Page.navigate', { url: FIX });

  let ready = false;
  for (let i = 0; i < 40; i++) {
    try { if (await ev("typeof CLOUD === 'object' && typeof CLOUD.request === 'function'")) { ready = true; break; } } catch (e) { }
    await sleep(200);
  }
  ok(ready, '夹具加载 cloud.js');

  // ---------- 1. 遥控码解析 ----------
  console.log('\n[1] 遥控码');
  const p1 = await ev("CLOUD.setPair('a1b2c3-482913')");
  ok(p1 === true, 'setPair 接受 "a1b2c3-482913"');
  ok(await ev("CLOUD.S.dev === 'A1B2C3'") === true, '设备码 = A1B2C3');
  ok(await ev("CLOUD.S.code === '482913'") === true, '遥控码 = 482913');
  ok(await ev("CLOUD.pairText()") === 'A1B2C3-482913', '回显 A1B2C3-482913');
  ok(await ev("CLOUD.setPair('123')") === false, '位数不够要拒绝');
  ok(await ev("CLOUD.setPair('zzzzzz-482913')") === false, '非法字符要拒绝');

  // ---------- 2. 协议编码 ----------
  console.log('\n[2] MQTT 编码');
  const hdr = await ev("(function(){var p=CLOUD._pkt(0x10, new Uint8Array(10)); return [p[0], p[1], p.length];})()");
  ok(hdr[0] === 0x10, 'CONNECT 首字节 0x10');
  ok(hdr[2] === 12, '剩余长度 10 → 总长 12');
  const big = await ev("(function(){var b=new Uint8Array(300); var p=CLOUD._pkt(0x30,b); return [p[0], p[1], p[2], p.length];})()");
  ok(big[1] === 0xAC && big[2] === 0x02, '剩余长度 >127 用两字节变长编码（300 → AC 02）');
  ok(big[3] === 303, '长包总长 303');
  const tk = await ev("CLOUD._take(new Uint8Array([0x30, 0x05, 1,2,3,4,5, 9,9]))");
  ok(tk && tk.type === 3 && tk.used === 7, 'takePacket 只吃一个包、不越界');

  // ---------- 3. 连上假 broker ----------
  console.log('\n[3] 连接');
  await ev("CLOUD.save({host:'127.0.0.1', wss:" + BROKER + ", secure:false})");
  await ev("CLOUD.enable(true)");
  let up = false;
  for (let i = 0; i < 40; i++) {
    if (await ev("CLOUD.active()")) { up = true; break; }
    await sleep(200);
  }
  ok(up, 'CONNECT → SUBSCRIBE 走通，状态 online');
  ok(await ev("CLOUD.S.phase") === 'online', 'phase = online');
  const stTxt = await ev("CLOUD.statusText()");
  ok(String(stTxt).indexOf('已连上') >= 0, '状态文案「已连上」：' + stTxt);

  // ---------- 4. 小请求 ----------
  console.log('\n[4] 发命令 / 收结果');
  const r1 = await ev("CLOUD.request('/api/vol', {v: 50}, 'POST')");
  ok(r1 && r1.ok === 1, '音量命令有回 {ok:1}');
  ok(r1 && String(r1.vol) === 'v=50', 'form 参数原样送到（v=50）');

  const r2 = await ev("CLOUD.request('/api/screen')");
  // 1.15.0：gname 不再是「分组名」，而是**当前这台的标签**（第 1 台中国之声 → 广播）
  ok(r2 && r2.gname === '广播', '屏幕 JSON 的标签名 = 广播');
  ok(r2 && r2.cnt > 250, '整表 ' + (r2 && r2.cnt) + ' 台（1.15.0 不再按组切）');
  ok(r2 && typeof r2.name === 'string' && r2.name.length > 0, '台名到位：' + (r2 && r2.name));

  // ---------- 5. 大消息（9KB，要分片）----------
  console.log('\n[5] 大消息');
  const r3 = await ev("CLOUD.request('/api/stations')");
  // 整表约 60KB，比分片阈值大得多，正好压一压分片重组。
  // 台数不写死：mock 是从真台表 src/preset_stations.h 读的，以 /api/screen 的 cnt 为准
  ok(r3 && r3.list && r3.list.length === (r2 && r2.cnt),
     '台表 ' + (r3 && r3.list && r3.list.length) + ' 条完整收下（= /api/screen 的 cnt）');
  ok(r3 && r3.tags && r3.tags.length >= 5, (r3 && r3.tags ? r3.tags.length : 0) + ' 个标签');
  ok(r3 && r3.list.every(x => x.t !== undefined), '每台都带标签下标 t（App 的 chips 靠它筛选）');
  ok(r3 && !r3.groups, '不再有 groups 字段');
  ok(r3 && r3.list[0].n === '中国之声', '第一条是中国之声');
  ok(r3 && r3.list[r3.list.length - 1].n === '苏州交通广播', '最后一条是苏州交通广播');

  // ---------- 6. 遥控码错 ----------
  console.log('\n[6] 遥控码不对');
  await ev("CLOUD.S.code = '000000'");
  const bad = await ev("CLOUD.request('/api/vol', {v: 1}, 'POST').then(function(){return 'ok'}).catch(function(e){return 'rejected:' + e.code})");
  ok(bad === 'rejected:403', '被设备拒绝（403）：' + bad);
  await ev("CLOUD.S.code = '482913'");

  // ---------- 7. 不存在的路径 ----------
  console.log('\n[7] 未知路径');
  const nf = await ev("CLOUD.request('/api/nope').then(function(){return 'ok'}).catch(function(e){return 'rejected:' + e.code})");
  ok(nf === 'rejected:400', '返回 400：' + nf);

  // ---------- 8. 并发 ----------
  console.log('\n[8] 并发序号');
  const many = await ev(`(async function(){
    var a = CLOUD.request('/api/vol', {v: 10}, 'POST');
    var b = CLOUD.request('/api/vol', {v: 20}, 'POST');
    var c = CLOUD.request('/api/vol', {v: 30}, 'POST');
    var r = await Promise.all([a, b, c]);
    return r.map(function(x){ return x.vol; }).join('|');
  })()`);
  ok(many === 'v=10|v=20|v=30', '三条并发各回各的：' + many);

  // ---------- 9. 免配对发现（1.19.0）----------
  console.log('\n[9] 免配对：设备广播');
  ok(await ev("CLOUD.S.devs.length") === 2, '订到 myradio/dev/+ 后看见 2 台机器');

  const d0 = await ev("JSON.stringify(CLOUD.S.devs.map(function(x){return [x.dev,x.n,x.open,x.batt]}))");
  ok(d0 === '[["A1B2C3","MyRadio-A1B2C3",1,82],["FFFFFF","MyRadio-FFFFFF",0,51]]',
     '两台机器的短码/名字/免配对/电量都对：' + d0);
  ok(await ev("CLOUD.devAlive(CLOUD.S.devs[0])") === true, '刚收到的广播算「在线」');
  ok(await ev("CLOUD.devAlive({dev:'X',seen:Date.now()-120000})") === false, '两分钟前的算「离线」');
  ok(await ev("CLOUD.findDev('FFFFFF') ? CLOUD.findDev('FFFFFF').open : -1") === 0,
     '第二台 open=0（关了免配对）');
  ok(await ev("CLOUD._devpre") === 'myradio/dev/', '广播主题前缀与 config.h 的 CLOUD_DEV_PREFIX 一致');

  // ---------- 10. 派生遥控码（1.19.0）----------
  console.log('\n[10] 派生遥控码');
  // 向量由 FNV-1a 32 + 盐 "MyRadio-1" 算好写死；C++ 那边（cloud.cpp deriveFrom）同一份算法
  ok(await ev("CLOUD.deriveCode('A1B2C3D4E5F6')") === '274682', 'A1B2C3D4E5F6 → 274682');
  ok(await ev("CLOUD.deriveCode('FFFFFFFFFFFF')") === '665288', 'FFFFFFFFFFFF → 665288');
  ok(await ev("CLOUD.deriveCode('000000000000')") === '781920', '000000000000 → 781920');
  ok(await ev("CLOUD.deriveCode('a1b2c3d4e5f6')") === '274682', '小写设备码算出来一样');
  ok(/^[1-9]\d{5}$/.test(await ev("CLOUD.deriveCode('0123456789AB')")), '派生码永远是 6 位、不以 0 开头');

  // 选一台开着免配对的机器：不用手输，直接能控
  ok(await ev("CLOUD.pickDev('A1B2C3')") === true, 'pickDev 免配对机器返回 true');
  ok(await ev("CLOUD.S.dev") === 'A1B2C3', '当前设备切到 A1B2C3');
  ok(await ev("CLOUD.S.code") === '274682', '遥控码自动变成派生码 274682（不用人输）');
  const rb = await ev("CLOUD.request('/api/batt')");
  ok(rb && rb.batt === 82, '免配对下直接读到电量：' + (rb && rb.batt));
  const rs = await ev("CLOUD.request('/api/state')");
  ok(rs && rs.bok === 1 && rs.batt === 82, '/api/state 里也带电量（App 设置页用）');

  // 关掉免配对的机器：pickDev 只切设备，不编码，要用户手输
  ok(await ev("CLOUD.pickDev('FFFFFF')") === false, 'open=0 的机器需要先输遥控码');
  ok(await ev("CLOUD.S.dev") === 'FFFFFF' && await ev("CLOUD.S.code") === '',
     '设备切过去了，但码是空的，等用户输入');
  const noCode = await ev("CLOUD.request('/api/vol', {v: 1}).then(function(){return 'ok'}).catch(function(e){return 'rejected:' + (e.code || e.message)})");
  ok(noCode === 'rejected:403' || noCode === 'rejected:nodev' || noCode === 'rejected:code',
     '没码就发不出去（被拒）：' + noCode);
  ok(await ev("CLOUD.setPair('ffffff-135790')") === true, '手输 "ffffff-135790" 能被收下');
  const r2b = await ev("CLOUD.request('/api/batt')");
  ok(r2b && r2b.batt === 51, '输对码后能控第二台（电量 51）：' + (r2b && r2b.batt));

  // 回到第一台，顺便确认「换机器不用重连」
  ok(await ev("CLOUD.pickDev('A1B2C3')") === true, '切回 A1B2C3');
  ok(await ev("CLOUD.S.conn") === true, '切机器没有把连接搞断（还是 online）');
  const r2c = await ev("CLOUD.request('/api/vol', {v: 55}, 'POST')");
  ok(r2c && String(r2c.vol) === 'v=55', '切回来还能发命令');

  console.log('\n通过 ' + oks + ' / 失败 ' + fails);
  cdp.close();
  broker.kill();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('崩了：' + e.message); process.exit(2); });
