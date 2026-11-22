// ============================================================
//  verify_radio.js —— 「一个旋钮走完 长波/中波/调频/短波」的回归（1.10.0）
//
//  两段验证：
//    A. 离线：把 core.js（手机 App 镜像）和 preview_template.html（预览页）
//       里的 radioWalk / radioGear 抠出来，跟 Python 复刻（tools/radio_sim.py
//       写出的 _radio_vectors.json）逐条对答案 —— 三份实现必须同一个数。
//    B. 真浏览器：连 CDP 打开 preview/index.html?fm=1&khz=981，
//       真的转旋钮、真的重绘，看 S.fmFreq 与画布有没有画出来。
//
//  用法：
//    python tools/radio_sim.py --vectors          # 先生成向量
//    chrome --headless=new --remote-debugging-port=9333 --user-data-dir=<临时> about:blank &
//    node tools/verify_radio.js
// ============================================================
const fs = require('fs');
const path = require('path');
const ROOT = path.dirname(__dirname);

let pass = 0, fail = 0;
function ok(cond, msg) {
  if (cond) { pass++; }
  else { fail++; console.log('  FAIL ' + msg); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ============================================================
//  A. 三份实现逐条对答案
// ============================================================
console.log('[A] 波段环数学：Python / core.js / preview 三份一致');

const vf = path.join(ROOT, 'tools', '_radio_vectors.json');
if (!fs.existsSync(vf)) {
  console.log('  先跑：python tools/radio_sim.py --vectors');
  process.exit(1);
}
const vectors = JSON.parse(fs.readFileSync(vf, 'utf8'));

// 从源码里抠模型：从 RBANDS 到 radioFreqText 结束（两份文件这一段是同一段文本）
function grabModel(rel) {
  const s = fs.readFileSync(path.join(ROOT, rel), 'utf8');
  const a = s.indexOf('const RBANDS = [');
  const b = s.indexOf('function radioFreqText(khz){');
  if (a < 0 || b < 0) throw new Error(rel + ' 里找不到 RBANDS / radioFreqText');
  const tail = s.indexOf('}\n', s.indexOf('return radioBandWord(khz)', b)) + 2;
  return s.slice(a, tail);
}

const modApp = grabModel('tools/app/js/core.js');
const modPrev = grabModel('tools/preview_template.html');
ok(modApp === modPrev, 'core.js 与 preview 的波段环代码逐字相同');
ok(modApp.indexOf('RBANDS') >= 0 && modApp.indexOf('function radioFreqText') > 0,
  '抠出来的是 RBANDS … radioFreqText 一整段（' + modApp.length + ' 字符）');

const apiApp = new Function(modApp + '\nreturn {radioWalk,radioGear,radioFreqText,radioBandOf,radioSnap};')();
const apiPrev = new Function(modPrev + '\nreturn {radioWalk,radioGear,radioFreqText,radioBandOf,radioSnap};')();

let badWalk = 0, badGear = 0;
for (const v of vectors) {
  if (v.ms !== undefined) {
    const a = apiApp.radioGear(v.ms), b = apiPrev.radioGear(v.ms);
    if (a !== v.want || b !== v.want) {
      badGear++;
      if (badGear < 4) console.log('  FAIL 转速 ' + v.ms + 'ms → app=' + a + ' prev=' + b + ' 期望 ' + v.want);
    }
    continue;
  }
  const a = apiApp.radioWalk(v.khz, v.clicks, v.gear);
  const b = apiPrev.radioWalk(v.khz, v.clicks, v.gear);
  if (a !== v.want || b !== v.want) {
    badWalk++;
    if (badWalk < 6) console.log('  FAIL ' + v.khz + 'kHz 走 ' + v.clicks + ' 格 ×' + v.gear + ' → app=' + a + ' prev=' + b + ' 期望 ' + v.want);
  }
}
ok(badWalk === 0, '转动向量 ' + vectors.filter(v => v.ms === undefined).length + ' 条全部对上（不一致 ' + badWalk + '）');
ok(badGear === 0, '转速档位全部对上（不一致 ' + badGear + '）');

// 显示格式
const fmt = [
  [98000, 'FM 98.0'], [64000, 'FM 64.0'], [108000, 'FM 108.0'],
  [981, '中波 981'], [1602, '中波 1602'], [198, '长波 198'], [6055, '短波 6055'],
];
for (const [khz, want] of fmt) {
  const a = apiApp.radioFreqText(khz), b = apiPrev.radioFreqText(khz);
  ok(a === want && b === want, khz + ' → App「' + a + '」预览「' + b + '」期望「' + want + '」');
}

// 老代码不许残留（残留就会在收音模式下用 fmfreq 的 100kHz 值画盘）
const prevSrc = fs.readFileSync(path.join(ROOT, 'tools', 'preview_template.html'), 'utf8');
const coreSrc = fs.readFileSync(path.join(ROOT, 'tools', 'app', 'js', 'core.js'), 'utf8');
ok(prevSrc.indexOf('S.fmMode ? S.fmFreq') < 0, '预览页没有残留「S.fmMode ? S.fmFreq」');
ok(coreSrc.indexOf('if (isFm) fcur = s.fmfreq') < 0, 'App 没有残留「isFm → fmfreq」');

// 生成物里也得有（防止改了模板忘了重跑 gen_webpreview / gen_webapp）
const built = fs.readFileSync(path.join(ROOT, 'preview', 'index.html'), 'utf8');
ok(built.indexOf('const RBANDS = [') >= 0, 'preview/index.html 已重新生成（含波段环）');
const webapp = fs.readFileSync(path.join(ROOT, 'src', 'webapp.h'), 'utf8');
ok(webapp.indexOf('RBANDS') >= 0, 'src/webapp.h 已重新生成（含波段环）');

console.log('[A] 通过 ' + pass + ' / 失败 ' + fail + '\n');

// ============================================================
//  B. 真浏览器：预览页里真的转旋钮
// ============================================================
const PORT = 9333;
const URL = 'file:///' + path.join(ROOT, 'preview/index.html').replace(/\\/g, '/') + '?fm=1&khz=981';

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
      let m; try { m = JSON.parse(typeof ev.data === 'string' ? ev.data : String(ev.data)); } catch (e) { return; }
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
  let cdp;
  try {
    cdp = await connect(await listPage());
  } catch (e) {
    console.log('[B] 跳过（没有 CDP）：' + e.message);
    console.log('\n通过 ' + pass + ' / 失败 ' + fail);
    process.exit(fail ? 1 : 0);
  }

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
  await cdp.send('Network.enable');
  await cdp.send('Network.setCacheDisabled', { cacheDisabled: true });
  await cdp.send('Page.navigate', { url: URL });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    try {
      if (await ev("typeof S === 'object' && typeof rotate === 'function' && typeof radioWalk === 'function'")) { ready = true; break; }
    } catch (e) { }
    await sleep(300);
  }
  if (!ready) { console.log('[B] 页面没就绪'); process.exit(1); }
  console.log('[B] 预览页已就绪（?fm=1&khz=981）');

  ok(await ev("S.fmMode === true"), '进了收音模式');
  ok(await ev("S.fmFreq") === 981, '?khz=981 落在中波 981');

  // 第一次转（S.lastTurnAt=0 → 当作慢转 ×1）→ 990
  await ev("rotate(1)");
  ok(await ev("S.fmFreq") === 990, '慢转一格：981 → ' + await ev("S.fmFreq") + '（期望 990）');

  // 手停下 500ms 再转，仍是慢转 ×1
  await ev("S.lastTurnAt = 0; rotate(-1)");
  ok(await ev("S.fmFreq") === 981, '再往回一格 → ' + await ev("S.fmFreq") + '（期望 981）');

  // 连点两下（脚本里间隔≈0ms）= 甩：第一下当作慢转 ×1（981→990），第二下 ×30（+270）
  await ev("S.lastTurnAt = 0; rotate(1); rotate(1)");
  ok(await ev("S.fmFreq") === 1260, '连点两下 981 → ' + await ev("S.fmFreq") + '（期望 1260：990 + 270）');

  // 跨波段：调频最低端往下 = 中波最高端
  await ev("S.fmFreq = 64000; S.lastTurnAt = 0; rotate(-1)");
  ok(await ev("S.fmFreq") === 1602, '调频 64.0 往下 → ' + await ev("S.fmFreq") + '（期望 1602 = 中波最高端）');

  // 短波最高端往上绕回长波
  await ev("S.fmFreq = 26100; S.lastTurnAt = 0; rotate(1)");
  ok(await ev("S.fmFreq") === 153, '短波 26100 往上 → ' + await ev("S.fmFreq") + '（期望 153 = 长波最低端）');

  // 真的重绘了：台名区（y 22..98）有墨
  const ink = await ev(`(() => {
    const cv = document.querySelector('canvas');
    if (!cv) return -1;
    const g = cv.getContext('2d');
    const d = g.getImageData(0, 22, cv.width, 76).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) {
      if (d[i] + d[i + 1] + d[i + 2] > 60) n++;
    }
    return n;
  })()`);
  ok(ink > 100, '台名区画出了「长波 153」这类字（非背景像素 ' + ink + '）');

  // 盘面刻度也在（y 134..170）
  const tickInk = await ev(`(() => {
    const cv = document.querySelector('canvas');
    const g = cv.getContext('2d');
    const d = g.getImageData(0, 134, cv.width, 36).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) {
      if (Math.abs(d[i] - 0x08) + Math.abs(d[i+1] - 0x4d) + Math.abs(d[i+2] - 0x5a) > 90) n++;
    }
    return n;
  })()`);
  ok(tickInk > 200, '调谐盘铺了刻度（异色像素 ' + tickInk + '）');

  cdp.close();
  console.log('\n通过 ' + pass + ' / 失败 ' + fail);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.log('异常：' + e.message); process.exit(1); });
