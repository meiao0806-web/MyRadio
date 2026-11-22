// verify_pincross_e2e.js —— 1.46.7 跨网「输 4 位码就连」的真浏览器端到端
//
// 和 verify_cloud.js 的区别：那个测 cloud.js 的接口，这个测**整扇门**：
//   起本地假 broker（两台假机器，一台 1234 / 一台 5678）
//   → 无头 Chrome 打开真的 MyRadio-远程版.html
//   → 像人一样在 4 格输入框里敲 1234
//   → 断言它**自己**连上、并且刷新后直接进界面（不再挡那扇门）
//
// 用法：node tools/verify_pincross_e2e.js
const { fork, spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

const DIR = __dirname;
const ROOT = path.join(DIR, '..');
const BROKER = 8898;
const PORT = 9334;
const PAGE = 'file:///' + path.join(ROOT, 'MyRadio-远程版.html').replace(/\\/g, '/');
const CHROME = 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe';

let oks = 0, fails = 0;
const ok = (c, m) => { c ? (oks++, console.log('  ✓ ' + m)) : (fails++, console.log('  ✗ ' + m)); };
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function findPage() {
  for (let i = 0; i < 40; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const j = await r.json();
      const p = j.find(x => x.type === 'page');
      if (p) return p.webSocketDebuggerUrl;
    } catch (e) {}
    await sleep(250);
  }
  throw new Error('CDP 没起来');
}

function connect(url) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(url);
    const pending = new Map();
    let seq = 1;
    ws.onopen = () => res({
      send(method, params) {
        const id = seq++;
        return new Promise((r2, j2) => { pending.set(id, { r2, j2 }); ws.send(JSON.stringify({ id, method, params })); });
      },
      close() { try { ws.close(); } catch (e) {} }
    });
    ws.onerror = e => rej(new Error('ws error'));
    ws.onmessage = ev => {
      const m = JSON.parse(ev.data);
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id); pending.delete(m.id);
        m.error ? p.j2(new Error(m.error.message)) : p.r2(m.result);
      }
    };
  });
}

(async () => {
  const broker = fork(path.join(DIR, 'mqtt_broker_sim.js'), [String(BROKER)], { stdio: 'ignore' });
  await sleep(700);

  const prof = path.join(os.tmpdir(), 'mr-pin-e2e');
  fs.rmSync(prof, { recursive: true, force: true });
  const chrome = spawn(CHROME, [
    '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
    '--allow-file-access-from-files',
    `--remote-debugging-port=${PORT}`, `--user-data-dir=${prof}`, 'about:blank'
  ], { stdio: 'ignore' });

  let cdp;
  try {
    cdp = await connect(await findPage());
    const send = cdp.send;

    async function ev(expr) {
      const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) {
        const d = r.exceptionDetails.exception;
        throw new Error('JS 报错：' + (d && (d.description || d.value)));
      }
      return r.result.value;
    }
    async function goto(url) {
      await send('Page.enable');
      await send('Page.navigate', { url });
      await sleep(1200);
    }

    // ---------- 0. 把 cloud.js 指向本地假 broker ----------
    // 真机用 broker-cn.emqx.io:8084（wss）；这里改成 127.0.0.1:8898（ws），
    // 不然测试会去打公网服务器 —— 那既慢又测不到我们的机器。
    await goto(PAGE);
    await ev("localStorage.setItem('mr_cloud', JSON.stringify({host:'127.0.0.1',wss:" + BROKER +
            ",secure:false,want:false,dev:'',code:''})); 'ok'");
    await ev("localStorage.removeItem('mr_cloud_devs'); 'ok'");
    await goto(PAGE);
    await sleep(2200);      // 等 MQTT 连上 + 收到两条保留广播

    console.log('\n[1] 开门：4 格在、机器报到');
    ok(await ev("!!document.getElementById('rmPin')") === true, '门上有 4 位码输入框');
    ok(await ev("document.getElementById('rmGate').hidden") === false, '还没配过，门是打开的');
    ok(await ev("CLOUD.S.conn") === true, '已经连上（本地）服务器');
    const devs = await ev("CLOUD.S.devs.map(function(d){return [d.dev,d.pin,d.open]})");
    ok(Array.isArray(devs) && devs.length === 2, '收到 2 台机器的广播：' + JSON.stringify(devs));
    ok(JSON.stringify(devs).indexOf('"1234"') > 0, '广播里真的带着 4 位码');

    console.log('\n[2] 敲 1234 —— 要自己连上');
    await ev("(function(){var i=document.getElementById('rmPin');i.value='1234';" +
             "i.dispatchEvent(new Event('input',{bubbles:true}));return 1;})()");
    await sleep(600);
    const mid = await ev("CLOUD.S.dev + '|' + CLOUD.S.code + '|' + CLOUD.S.conn");
    ok(mid.indexOf('A1B2C3|') === 0, '输完码当前设备就切到 A1B2C3（实际 ' + mid + '）');
    ok(mid.split('|')[1] === '274682', '遥控码自动算好（派生码 274682，不用人输）');

    await sleep(3600);      // 等 waitUp 看到 active() 后 reload
    console.log('\n[3] 连上之后：门自己退开');
    ok(await ev("document.getElementById('rmGate') ? document.getElementById('rmGate').hidden : 'gone'") === true,
       '刷新后那扇门不再挡路（gate.hidden = true）');
    ok(await ev("CLOUD.S.dev") === 'A1B2C3', '刷新后还记得是哪台（A1B2C3）');
    const batt = await ev("CLOUD.request('/api/batt').then(function(r){return r.batt}).catch(function(e){return 'err:'+e.message})");
    ok(batt === 82, '刷新后能真的读到电量（82）—— 整条远程链路通了');

    console.log('\n[4] 输错码：不能说「已连上」');
    await ev("localStorage.setItem('mr_cloud', JSON.stringify({host:'127.0.0.1',wss:" + BROKER +
             ",secure:false,want:false,dev:'',code:''})); 1");
    await goto(PAGE);
    await sleep(2200);
    await ev("(function(){var i=document.getElementById('rmPin');i.value='9999';" +
             "i.dispatchEvent(new Event('input',{bubbles:true}));return 1;})()");
    await sleep(900);
    ok(await ev("document.getElementById('rmGate').hidden") === false, '码不对 → 门还开着');
    const tip = await ev("document.getElementById('rmTip').textContent");
    ok(/没找到|9999/.test(tip), '提示说清了没找到：' + String(tip).slice(0, 40));
    ok(await ev("(document.getElementById('rmList').innerHTML||'').length") > 0,
       '同时把在线的机器列出来（还能点一台直接连）');

    console.log('\n[5] 多台同码：要让人挑，不能替人决定');
    // 让第二台也用 1234（改模拟器的 DEVS 不可能，这里直接在页面里造一条）
    await ev("(function(){CLOUD.S.devs.push({dev:'DDDDDD',id:'DDDDDDDDDDDD',n:'MyRadio-DDDDDD'," +
             "open:1,batt:70,pin:'1234',seen:Date.now()});return 1;})()");
    const r2 = await ev("JSON.stringify(CLOUD.pinLogin('1234'))");
    const j2 = JSON.parse(r2);
    ok(j2.n === 2, '两台都用 1234 → n=2（不是随便挑一台）');
    ok(j2.list.length === 2, '把两台都给出来让界面显示');

  } catch (e) {
    console.log('\n!! 测试中断：' + (e && e.message));
    fails++;
  } finally {
    if (cdp) cdp.close();
    try { chrome.kill(); } catch (e) {}
    try { broker.kill(); } catch (e) {}
    console.log('\n通过 ' + oks + ' / 失败 ' + fails);
    process.exit(fails ? 1 : 0);
  }
})();
