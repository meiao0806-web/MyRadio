// 验手机 App 的「台的健康」这套东西（1.17.0）：有问题筛选 / 行内标 / 实时出声 / 换行
//
// 走的是 preview/index.html —— 那一页把**固件里真正的 webapp.h** 塞进 iframe，
// 再把它的 fetch 接到模拟器上（模拟器里有 /api/health）。所以这里验的就是真页面。
//
//   node tools/verify_app.js
// 需要本机有个带调试端口的 Chrome：
//   chrome --headless=new --remote-debugging-port=9333 --user-data-dir=<临时目录> preview/index.html
const PORT = Number(process.env.CDP_PORT || 9333);
const path = require('path');

const PAGE = 'file:///' + path.resolve(__dirname, '..', 'preview', 'index.html')
  .replace(/\\/g, '/').replace(/^\//, '');

let oks = 0, fails = 0;
const ok = (c, m) => { c ? (oks++, console.log('  ✓ ' + m)) : (fails++, console.log('  ✗ ' + m)); };
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function listPage() {
  for (let i = 0; i < 40; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const j = await r.json();
      const p = j.find(x => x.type === 'page' && x.webSocketDebuggerUrl);
      if (p) return p.webSocketDebuggerUrl;
    } catch (e) { }
    await sleep(400);
  }
  throw new Error('CDP 端口没起来（' + PORT + '）');
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
        return new Promise((res, rej) => {
          pending.set(id, { resolve: res, reject: rej });
          ws.send(JSON.stringify({ id, method, params: params || {} }));
        });
      },
      close() { try { ws.close(); } catch (e) { } }
    }));
    ws.addEventListener('error', () => reject(new Error('ws error')));
  });
}

(async () => {
  const cdp = await connect(await listPage());
  const send = cdp.send.bind(cdp);
  await send('Page.enable');
  await send('Runtime.enable');
  // 模拟一台普通手机（宽度按 iPhone 的 390 算）
  await send('Emulation.setDeviceMetricsOverride',
    { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });

  async function ev(expr) {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) {
      const d = r.exceptionDetails.exception;
      throw new Error('页面内异常：' + ((d && (d.description || d.value)) || ''));
    }
    return r.result.value;
  }
  const W = "document.getElementById('appFrame').contentWindow";
  const D = "document.getElementById('appFrame').contentDocument";
  const q = sel => ev(`(function(){var d=${D};var e=d&&d.querySelector(${JSON.stringify(sel)});return e?e.textContent.trim():null})()`);
  const goTab = t => ev(`(function(){var d=${D};var b=d.querySelector('.tab[data-tab="${t}"]');if(b)b.click();return 1})()`);

  await send('Page.navigate', { url: PAGE });
  let ready = false;
  for (let i = 0; i < 60; i++) {
    // 等 iframe 里的 App 起来：chips 里已经有「有问题」这颗就算好
    const v = await ev(`(function(){var d=${D};return !!(d&&d.getElementById('chipBad'))})()`).catch(() => false);
    if (v) { ready = true; break; }
    await sleep(400);
  }
  if (!ready) { console.log('页面/iframe 没就绪'); process.exit(2); }
  // 切到「手机 App」这一栏，iframe 才有尺寸
  await ev("(function(){var b=document.getElementById('tabApp');if(b)b.click();return 1})()");
  await sleep(900);

  console.log('[1] App 真的装进去了');
  const iw = await ev(`${W}.innerWidth`);
  ok(iw > 300 && iw < 460, 'iframe 视口宽 ' + iw + 'px');

  console.log('\n[2] 「有问题」这个筛选在');
  const chips = await ev(`[...${D}.querySelectorAll('#stChips .chip')].map(b=>b.textContent.trim())`);
  ok(chips.some(t => t.indexOf('全部') === 0), '有「全部」：' + chips[0]);
  const bi = chips.findIndex(t => t.indexOf('有问题') === 0);
  ok(bi > 0, '「有问题」排在全部后面：' + chips[bi]);
  const cnt = Number((chips[bi] || '').replace(/[^0-9]/g, ''));
  ok(cnt > 0, '预览里预置了问题台，数量 = ' + cnt);

  console.log('\n[3] 点它只留问题台');
  await goTab('stations');
  await sleep(400);
  const total0 = await ev(`${D}.querySelectorAll('#stList .row').length`);
  await ev(`${D}.getElementById('chipBad').click()`);
  await sleep(400);
  const badRows = await ev(`[...${D}.querySelectorAll('#stList .row')].map(r=>({
      n: r.querySelector('.t').textContent.trim(),
      m: r.querySelector('.m').textContent.trim(),
      hl: (r.querySelector('.m .hl')||{}).textContent || ''
    }))`);
  ok(badRows.length === cnt, '列表只剩 ' + badRows.length + ' 行（原来 ' + total0 + '）');
  ok(badRows.every(r => /连不上|没声音/.test(r.m)), '每一行都带原因：' + badRows.map(r => r.m).join(' / '));
  ok(badRows.every(r => r.hl), '每一行都有 .hl 小标');
  const hint = await q('#stHint');
  ok(/有问题的台/.test(String(hint)), '标题说清了：' + hint);

  console.log('\n[4] 行内小标两种颜色都出得来');
  ok(badRows.some(r => /连不上/.test(r.hl)), '有「连不上」');
  ok(badRows.some(r => /没声音/.test(r.hl)), '有「没声音」');
  // 一排标签里一颗都不许折成两行（高度必须是同一个 40px）
  const chipH = await ev(`[...${D}.querySelectorAll('#stChips .chip')].map(b=>Math.round(b.getBoundingClientRect().height))`);
  ok(chipH.every(h => h === 40), 'chips 高度一致：' + [...new Set(chipH)].join('/'));

  console.log('\n[5] 主页那条实时出声');
  await goTab('home');
  await sleep(1400);
  const snd = await q('#sndTx');
  ok(['在出声', '等一下…', '正在连…', '没在放'].indexOf(String(snd)) >= 0, '状态文字：' + snd);
  const lvw = await ev(`${D}.getElementById('lvBar').style.width`);
  ok(/^\d+(\.\d+)?%$/.test(lvw), '电平条宽度：' + lvw);

  console.log('\n[6] 换行：台名不许折成两行');
  // 量的时候必须同步做完：主页每 0.9 秒会把 #tipTx 改回真台名
  const tip = await ev(`(function(){var d=${D};var t=d.getElementById('tipTx');var save=t.textContent;
    t.textContent='播放中 · 中央广播电视总台中国之声 · 第 123 集';
    var cs=getComputedStyle(t);var lh=parseFloat(cs.lineHeight);
    var h=t.getBoundingClientRect().height;
    var r={lines:Math.round(h/lh),h:h,lh:lh,ws:cs.whiteSpace,sw:t.scrollWidth,cw:t.clientWidth,
           rowH:t.parentElement.getBoundingClientRect().height};
    t.textContent=save;return r})()`);
  ok(tip.lines === 1, '一行（高 ' + Math.round(tip.h) + 'px / 行高 ' + Math.round(tip.lh) + '）');
  ok(tip.ws === 'nowrap', 'white-space = ' + tip.ws);
  ok(tip.sw > tip.cw, '超出部分打省略号（' + tip.sw + ' > ' + tip.cw + '）');
  ok(tip.rowH < 40, '整行没被顶高：' + Math.round(tip.rowH) + 'px');

  console.log('\n[7] 体检页那张卡');
  await goTab('diag');
  await sleep(1200);
  const hl = {
    ok: await q('#hlOk'), bad: await q('#hlBad'), dim: await q('#hlDim'), unk: await q('#hlUnk'),
    sum: await q('#hlSum')
  };
  const sum = (+hl.ok) + (+hl.bad) + (+hl.dim) + (+hl.unk);
  ok(sum === total0, '四格加起来 = 台数 ' + total0 + '（' + [hl.ok, hl.bad, hl.dim, hl.unk].join(' / ') + '）');
  ok(/要留意|都没问题/.test(String(hl.sum)), '右上角：' + hl.sum);

  console.log('\n[8] 「只看有问题的台」能跳过去');
  await ev(`${D}.getElementById('btnHlList').click()`);
  await sleep(600);
  const pageNow = await ev(`${D}.querySelector('.page.on').dataset.page`);
  ok(pageNow === 'stations', '落在电台页：' + pageNow);
  const n2 = await ev(`${D}.querySelectorAll('#stList .row').length`);
  ok(n2 === cnt, '列表就是问题台：' + n2);

  console.log('\n[9] 「挨个试一遍」弹层能开');
  await goTab('diag');
  await sleep(400);
  await ev(`${D}.getElementById('btnHlScan').click()`);
  await sleep(500);
  const sheet = await q('.sheet h3');
  ok(sheet === '挨个试一遍', '弹层标题：' + sheet);
  const opts = await ev(`[...${D}.querySelectorAll('.sheet .opt')].map(b=>b.textContent.trim())`);
  ok(opts.length === 3, '三个选项：' + opts.join(' / '));
  await ev(`${D}.querySelector('.sheet .opt').click()`);
  await sleep(1500);
  const scNow = await q('#scNow');
  ok(/正在试/.test(String(scNow)), '试台开始跑：' + scNow);
  const scNum = await q('#scNum');
  ok(/^\d+ \/ \d+$/.test(String(scNum)), '进度：' + scNum);
  await ev(`${D}.querySelector('#scStop').click()`);
  await sleep(600);

  // ---------- 10. 选集翻页（1.18.0）----------
  // 官场现形记 1167 集，机器按字节预算切页：弹层顶部要有「上一段 / 第 a–b 集 / 下一段」，
  // 翻页要真的换内容 —— 这是能不能翻到第 800 集的关键。
  //
  // App 的 JS 是 const/let 顶格写的（在脚本作用域里，不挂在 window 上），
  // 所以必须用 contentWindow.eval 在**那个 realm** 里求值，不能直接写 W.S。
  console.log('\n[10] 选集翻页');
  const ae = code => ev(`${W}.eval(${JSON.stringify(code)})`);

  await goTab('stations');
  await sleep(300);
  const gcIdx = await ae("S.list.findIndex(function(x){return x.n==='官场现形记'})");
  ok(gcIdx > 0, '台表里找得到官场现形记：第 ' + (gcIdx + 1) + ' 台');
  await ae("post('/api/play',{id:" + gcIdx + "}).then(function(){return openEpisodes()}).then(function(){return 1})");
  await sleep(800);

  const epH = await q('.sheet h3');
  ok(epH === '官场现形记', '弹层标题：' + epH);
  const rng1 = await q('.eppage span');
  ok(/第 1–\d+ 集 · 共 1167 集/.test(String(rng1)), '翻页条：' + rng1);
  const rowN = await ev(`${D}.querySelectorAll('#epList .row').length`);
  ok(rowN > 10 && rowN < 1167, '只画了一页（' + rowN + ' 行），没有一次铺 1167 行');
  const firstIx = await q('#epList .row .ix');
  ok(String(firstIx).trim() === '1', '第一行是第 1 集');
  const prevDis = await ev(`${D}.getElementById('epgPrev').disabled`);
  ok(prevDis === true, '第一页时「上一段」按不了');

  await ev(`${D}.getElementById('epgNext').click()`);
  await sleep(800);
  const rng2 = await q('.eppage span');
  const m2 = String(rng2).match(/第 (\d+)–(\d+) 集 · 共 (\d+) 集/);
  ok(m2 && Number(m2[3]) === 1167, '点「下一段」→ ' + rng2);
  ok(m2 && Number(m2[1]) === rowN + 1, '第二页从第 ' + (m2 && m2[1]) + ' 集起（= 上一页行数 + 1）');
  const prevOn = await ev(`${D}.getElementById('epgPrev').disabled`);
  ok(prevOn === false, '翻到第二页后「上一段」可以按了');

  await ev(`${D}.getElementById('epgPrev').click()`);
  await sleep(800);
  const rng3 = await q('.eppage span');
  ok(rng3 === rng1, '翻回来还是第一页：' + rng3);

  // 直接跳到很后面的一集：从弹层点行 → 关弹层 → 真的切集
  await ae("openEpisodes()");
  await sleep(800);
  await ev(`${D}.getElementById('epgNext').click()`);
  await sleep(800);
  await ev(`${D}.querySelector('#epList .row').click()`);
  await sleep(600);
  const curAfter = await ae("({cur:(S.scr&&S.scr.idx!==undefined)?S.scr.idx:-1, epi:(S.scr&&S.scr.epi!==undefined)?S.scr.epi:-1, epn:(S.scr&&S.scr.epn!==undefined)?S.scr.epn:-1})");
  const gcStation = await ae("S.list.findIndex(function(x){return x.n==='官场现形记'})");
  ok(curAfter && curAfter.cur === gcStation, '点第二页第 1 行：还在这一台（S.scr.idx=' + (curAfter && curAfter.cur) + '）');
  ok(curAfter && curAfter.epi === rowN, '并且真的跳到第 ' + (curAfter && (curAfter.epi + 1)) + ' 集（= 第二页首集）');
  ok(curAfter && curAfter.epn === 1167, '这台共 1167 集（屏上 epn）');

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  require('fs').writeFileSync(path.join(__dirname, '_shot_app.png'), Buffer.from(shot.data, 'base64'));

  cdp.close();
  console.log('\n' + oks + ' 通过 / ' + fails + ' 失败');
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('脚本异常：', e.message); process.exit(2); });
