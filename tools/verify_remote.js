// 端到端：远程版网页 + 假 broker + 两台假机器
// 验的是「人在外地」这条完整链路：
//   1.19.1 起第一次打开不再只认手抄的 12 位码 —— 先自动列出家里的机器，点一下就连；
//   找不到才退回手输。填/选完 → 连上 → 首页显示台名 → 点下一台。
const { fork } = require('child_process');
const path = require('path');

const PORT = 9333;
const BROKER = 8899;
// 台数不写死：直接从固件台表读，台表一改这里自动跟上（1.46.29 起 235 台）
const PRESET_ST_N = (() => {
  const t = require('fs').readFileSync(path.join(__dirname, '..', 'src', 'preset_stations.h'), 'utf8');
  const m = t.match(/#define\s+PRESET_ST_N\s+(\d+)/);
  return m ? +m[1] : -1;
})();
const DIR = __dirname;
const PAGE = 'file:///' + path.join(DIR, '..', 'MyRadio-远程版.html').replace(/\\/g, '/');

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

  const cdp = await connect(await listPage());
  async function ev(expr) {
    const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) {
      const d = r.exceptionDetails.exception;
      throw new Error('页面内异常：' + ((d && (d.description || d.value)) || JSON.stringify(r.exceptionDetails)));
    }
    return r.result.value;
  }
  // 页面 reload 会让旧的执行上下文消失，这类调用要允许失败
  async function evSoft(expr) { try { return await ev(expr); } catch (e) { return undefined; } }

  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');

  // 本机假 broker（真机上是 broker-cn.emqx.io:8084 走 wss）
  const SETUP = "localStorage.setItem('mr_cloud', JSON.stringify({host:'127.0.0.1', wss:"
    + BROKER + ", secure:false, want:true}))";

  /* ---------- 第 1 段：免配对机器，自动发现 ---------- */
  await cdp.send('Page.navigate', { url: PAGE });
  await sleep(1200);
  await ev("localStorage.clear()");
  await ev(SETUP);
  await cdp.send('Page.navigate', { url: PAGE });

  let gate = false;
  for (let i = 0; i < 60; i++) {
    if (await evSoft("!document.getElementById('rmGate').hidden")) { gate = true; break; }
    await sleep(200);
  }
  ok(gate, '第一次打开弹出开门小窗');

  // 等自动发现（机器每 30 秒报一次到；假 broker 订阅时立刻补发保留消息）
  let rows = 0;
  for (let i = 0; i < 80; i++) {
    rows = await evSoft("document.querySelectorAll('#rmList .rmdev').length") || 0;
    if (rows >= 2) break;
    await sleep(250);
  }
  ok(rows === 2, '自动列出家里的机器：' + rows + ' 台（没输任何码）');

  const listTx = await evSoft("document.getElementById('rmList').innerText") || '';
  ok(listTx.indexOf('MyRadio-A1B2C3') >= 0, '列表里有 MyRadio-A1B2C3');
  // 1.46.7：入口改成「输 4 位码」，列表里标的是每台的码（两台码不同才好分）
  ok(listTx.indexOf('码 1234') >= 0, '列表里标出第一台的码（码 1234）');
  ok(listTx.indexOf('码 5678') >= 0, '列表里标出第二台的码（码 5678）');

  // 点第一台 → 自动算码 → 连上 → 页面自己重进
  await ev("document.querySelector('#rmList .rmdev[data-dev=\"A1B2C3\"]').click()");

  let up = false;
  for (let i = 0; i < 80; i++) {
    const v = await evSoft("typeof CLOUD==='object' && CLOUD.active() && CLOUD.S.dev");
    if (v === 'A1B2C3') { up = true; break; }
    await sleep(300);
  }
  ok(up, '点一下就选好了 A1B2C3 且连上了');

  // 上面那一帧可能还没轮到 location.reload()，等它重进完再看
  let gateAfter = false;
  for (let i = 0; i < 60; i++) {
    if (await evSoft("document.getElementById('rmGate') && document.getElementById('rmGate').hidden") === true) { gateAfter = true; break; }
    await sleep(250);
  }
  ok(gateAfter, '重进后小窗自己让开');

  const pair = await evSoft("CLOUD.pairText()");
  ok(pair === 'A1B2C3-274682', '遥控码是算出来的（不用抄）：' + pair);

  // 首页真的拿到了机器的屏幕
  let scr = null;
  for (let i = 0; i < 60; i++) {
    scr = await evSoft("S.scr && S.scr.name");
    if (scr) break;
    await sleep(300);
  }
  ok(scr === '中国之声', '首页屏幕显示台名：' + scr);
  ok(await evSoft("S.scr.gname") === '广播', '标签：广播（1.15.0 起 gname 是当前台的标签，不是分组名）');
  const cnt = await evSoft("S.scr.cnt");
  ok(cnt === PRESET_ST_N, '整表 ' + cnt + ' 台（PRESET_ST_N ' + PRESET_ST_N + '）');

  const sub = await evSoft("document.getElementById('hdrSub').textContent");
  ok(String(sub).indexOf('标签 · 广播') === 0, '顶部标题：「' + sub + '」');
  ok(String(sub).indexOf('外地控制') > 0, '顶部标明是外地控制');

  // 点「下一台」→ 云端 post → 设备执行
  await ev("document.getElementById('btnNext').click()");
  await sleep(900);
  const lat = await ev("NET.lat");
  ok(typeof lat === 'number', '往返耗时已记录：' + lat + ' ms（本机回环可能是 0）');

  // 台列表页（大消息）
  await ev("go('stations')");
  let n = 0;
  for (let i = 0; i < 40; i++) {
    n = await evSoft("(S.list||[]).length") || 0;
    if (n) break;
    await sleep(300);
  }
  ok(n === cnt, '电台页 ' + n + ' 条（跨网也拿全了，= 屏幕里的 cnt）');

  // 设置页的远程卡 + 我的机器
  await ev("go('settings')");
  await sleep(1400);
  const rc = await ev("document.getElementById('svRSt').textContent");
  ok(String(rc).indexOf('已连上') >= 0, '设置页状态：' + rc);
  // 1.46.7：设置页现在要等在线广播到齐才画（同一份 devs 给门和列表用），
  //   「立刻数」会数到 0。改成等最多 6 秒。
  let devs = 0;
  for (let i = 0; i < 24; i++) {
    devs = await evSoft("document.querySelectorAll('#devList .devrow').length") || 0;
    if (devs >= 1) break;
    await sleep(250);
  }
  ok(devs >= 1, '设置 → 我的机器里也列出了 ' + devs + ' 台');
  const devPins = await evSoft("Array.from(document.querySelectorAll('#devList .pn')).map(function(e){return e.textContent}).join(',')");
  ok(String(devPins).indexOf('1234') >= 0, '「我的机器」里那台还带着码标：' + devPins);

  /* ---------- 第 2 段：关了免配对的机器，退回手输 ---------- */
  await ev("localStorage.clear()");
  await ev(SETUP);
  await cdp.send('Page.navigate', { url: PAGE });

  let rows2 = 0;
  for (let i = 0; i < 80; i++) {
    rows2 = await evSoft("document.querySelectorAll('#rmList .rmdev').length") || 0;
    if (rows2 >= 2) break;
    await sleep(250);
  }
  ok(rows2 === 2, '重开一次还是能列出 ' + rows2 + ' 台');

  await ev("document.querySelector('#rmList .rmdev[data-dev=\"FFFFFF\"]').click()");
  await sleep(400);
  const pre = await ev("document.getElementById('rmIn').value");
  const tip2 = await ev("document.getElementById('rmTip').textContent");
  ok(pre === 'FFFFFF', '点关了免配对的那台 → 输入框预填机器码：' + pre);
  ok(String(tip2).indexOf('免配对') > 0, '并提示要补遥控码：' + tip2);

  await ev("document.getElementById('rmIn').value='ffffff135790'");
  await ev("document.getElementById('rmGo').click()");

  let up2 = false;
  for (let i = 0; i < 80; i++) {
    const v = await evSoft("typeof CLOUD==='object' && CLOUD.active() && CLOUD.S.dev");
    if (v === 'FFFFFF') { up2 = true; break; }
    await sleep(300);
  }
  ok(up2, '补完 6 位 → 连上关着免配对的那台（FFFFFF）');

  console.log('\n通过 ' + oks + ' / 失败 ' + fails);
  cdp.close();
  broker.kill();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('崩了：' + e.message); process.exit(2); });
