// 验「重新连接网络」这条流程（1.46.35）
//
// 用户报的毛病：App 点「重新连接网络」→ 弹「没成功」，其实机器在重连。
// 根因：固件当场 WiFi.disconnect()，回包发不出去；App 又把「链路断了」当成失败。
//
// 这里的断言分两段：
//   A. 正常路径：/api/fix 回 ok → 不发「没成功」，进等待页
//   B. 断链路径：把 /api/fix 改成「请求发出去但连接被掐」→ 仍然**不能**说失败，
//      而要进等待页，并且 /api/info 恢复后自己回到设置页
//
//   node tools/verify_reconn.js
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
    const v = await ev(`(function(){var d=${D};return !!(d&&d.getElementById('btnReconn'))})()`).catch(() => false);
    if (v) { ready = true; break; }
    await sleep(400);
  }
  if (!ready) { console.log('页面/iframe 没就绪'); process.exit(2); }
  await ev("(function(){var b=document.getElementById('tabApp');if(b)b.click();return 1})()");
  await sleep(900);
  await goTab('settings');
  await sleep(700);
  // 预览页 iframe 里 INIT 队列的执行时机不稳，设置页那批按钮常常还没绑上。
  // 直接跑一次（幂等，重复绑无害），否则测的就不是页面逻辑而是加载时序。
  await ev(`(function(){try{${W}.bindPages();return 1}catch(e){return 'ERR:'+e.message}})()`);
  await sleep(300);
  await sleep(300);

  // 请求记录：把 window.fetch 包一层，记下 /api/fix 的 body
  await ev(`(function(){
    var w=${W};
    if(!w.__reconn){ w.__reconn={reqs:[]}; }
    var of=w.fetch;
    w.fetch=function(u,o){
      try{ if(String(u).indexOf('/api/fix')>=0) w.__reconn.reqs.push(String((o&&o.body)||'')); }catch(e){}
      return of.apply(w,arguments);
    };
    return 1})()`);

  console.log('[1] 按钮在，点了先问一句');
  const btn = await q('#btnReconn');
  ok(/重新连接网络/.test(String(btn)), '按钮文案：' + btn);

  // 等待页可能只活 1.5 秒（预览里 /api/info 立刻通），靠「某一刻去读」会跟它赛跑。
  // 装个观察器，只要「曾经显示过」就记下来 —— 这才是要断言的语义。
  await ev(`(function(){
    var w=${W}, d=${D}, b=d.getElementById('boot');
    if(!w.__watchBoot){
      w.__netShow = 0;
      w.__watchBoot = new MutationObserver(function(){
        if(!b.classList.contains('gone') && /重新连 WiFi/.test(b.textContent)) w.__netShow = 1;
      });
      w.__watchBoot.observe(b, { attributes:true, childList:true, subtree:true, characterData:true });
    }
    w.__netShow = 0;
    return 1})()`);

  await ev(`${D}.getElementById('btnReconn').click()`);
  await sleep(500);
  const sheetTitle = await q('.sheet h3');
  const sheetTx = await q('#sheetHost');
  ok(!!sheetTx, '弹出了确认层');
  ok(/重新连接网络？/.test(String(sheetTx)), '问的是「重新连接网络？」');

  console.log('\n[2] 点「重连」→ 发的就是 action=reconnect');
  const oksBtns = await ev(`[...${D}.querySelectorAll('#sheetHost .btn')].map(b=>b.textContent.trim())`);
  ok(oksBtns.some(t => t === '重连'), '确认键叫「重连」：' + JSON.stringify(oksBtns));
  ok(oksBtns.some(t => t === '取消'), '取消键叫「取消」：' + JSON.stringify(oksBtns));
  const idx = oksBtns.indexOf('重连');
  await ev(`(function(){var d=${D};var bs=[...d.querySelectorAll('#sheetHost .btn')];bs[${idx}].click();return 1})()`);
  await sleep(900);

  const reqs = await ev(`${W}.__reconn.reqs`);
  ok(reqs.length >= 1, '发了 ' + reqs.length + ' 次 /api/fix');
  ok(reqs.some(b => /action=reconnect/.test(String(b))), '参数是 action=reconnect：' + String(reqs[0] || ''));

  console.log('\n[3] 不说「没成功」，而是进等待页');
  const boot = await ev(`(function(){var w=${W},d=${D};var b=d.getElementById('boot');return {shown:w.__netShow, tx:b?b.textContent.trim().slice(0,40):''}})()`);
  ok(boot.shown === 1, '等待页出现过：' + boot.tx);
  ok(/重新连 WiFi/.test(boot.tx), '文案是「正在重新连 WiFi…」');

  const toasts = await ev(`(function(){var d=${D};var t=d.getElementById('toastTx');return t?t.textContent.trim():''})()`);
  ok(!/没成功/.test(String(toasts)), '没出现「没成功」：' + toasts);

  console.log('\n[4] 网络回来后自己关掉等待页');
  let closed = false;
  for (let i = 0; i < 20; i++) {
    const v = await ev(`(function(){var d=${D};var b=d.getElementById('boot');return b&&b.classList.contains('gone')})()`).catch(() => false);
    if (v) { closed = true; break; }
    await sleep(500);
  }
  ok(closed, '等待页自己收起了（没让人手动关）');
  const back = await ev(`(function(){var d=${D};return d.getElementById('wifiCard')?1:0})()`);
  ok(back === 1, '回到了设置页');

  console.log('\n[5] 断链那条路：请求被掐也不能说失败');
  // 把 /api/fix 变成「发出去就断」——抛出没有 code 的 TypeError，模拟真的链路断
  await ev(`(function(){
    var w=${W};
    w.__origFetch = w.__origFetch || w.fetch;
    var base=w.__origFetch;
    w.fetch=function(u,o){
      if(String(u).indexOf('/api/fix')>=0){
        return Promise.reject(new TypeError('Failed to fetch'));
      }
      return base.apply(w,arguments);
    };
    return 1})()`);
  await ev(`(function(){var w=${W};w.__netShow=0;return 1})()`);
  await ev(`${D}.getElementById('btnReconn').click()`);
  await sleep(400);
  await ev(`(function(){var d=${D};var bs=[...d.querySelectorAll('#sheetHost .btn')];for(var i=0;i<bs.length;i++){if(bs[i].textContent.trim()==='重连'){bs[i].click();return 1}}return 0})()`);
  await sleep(900);
  const t2 = await ev(`(function(){var d=${D};var t=d.getElementById('toastTx');return t?t.textContent.trim():''})()`);
  ok(!/没成功/.test(String(t2)), '链路断掉时也不弹「没成功」：' + t2);
  const boot2 = await ev(`(function(){var w=${W},d=${D};var b=d.getElementById('boot');return {shown:w.__netShow, tx:b?b.textContent.trim().slice(0,30):''}})()`);
  ok(boot2.shown === 1, '照样进等待页等它回来：' + boot2.tx);

  console.log('\n汇总：' + oks + ' / ' + (oks + fails));
  cdp.close();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('崩了：', e.message); process.exit(2); });
