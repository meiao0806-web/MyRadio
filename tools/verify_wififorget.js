// 验「忘掉 WiFi」这条路（1.46.36 起，1.46.51 改版后重写）
//
// 用户当年报：点了忘掉，界面上还在，不知道机器里到底删没删。两个根因：
//   A. App 往已被 openSheet 销毁的节点里写内容（界面不变）
//   B. 固件无条件回 ok:1（没人知道真删没删）
//
// 1.46.51 把「换一个 WiFi」和「已存 WiFi」合成一页、4 个格子，
// 所以选择器全换了：#sheetHost [data-forget] / .wsName，且「删完必须重开一页」。
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
      const j = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const p = j.find(x => x.type === 'page' && x.webSocketDebuggerUrl);
      if (p) return p.webSocketDebuggerUrl;
    } catch (e) { }
    await sleep(400);
  }
  throw new Error('CDP 没起来（' + PORT + '）');
}
function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url); const pending = new Map(); let seq = 1;
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
        return new Promise((res, rej) => { pending.set(id, { resolve: res, reject: rej }); ws.send(JSON.stringify({ id, method, params: params || {} })); });
      },
      close() { try { ws.close(); } catch (e) { } }
    }));
    ws.addEventListener('error', () => reject(new Error('ws error')));
  });
}

(async () => {
  const cdp = await connect(await listPage());
  const send = cdp.send.bind(cdp);
  await send('Page.enable'); await send('Runtime.enable');

  async function ev(x) {
    const r = await send('Runtime.evaluate', { expression: x, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) return { __err: (r.exceptionDetails.exception && (r.exceptionDetails.exception.description || '')) || 'err' };
    return r.result.value;
  }
  async function waitFor(code, ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < (ms || 8000)) {
      if (await ev(code)) return true;
      await sleep(120);
    }
    return false;
  }
  const W = "document.getElementById('appFrame').contentWindow";
  const D = "document.getElementById('appFrame').contentDocument";
  const q = sel => ev(`(function(){var d=${D};var e=d&&d.querySelector(${JSON.stringify(sel)});return e?e.textContent.trim():null})()`);

  // 记下 App 发了什么
  const armSent = () => ev(`(function(){
    var w=${W};
    if(!w.__fgsent){
      w.__fgsent=[];
      var of=w.fetch;
      w.fetch=function(u,o){
        try{ var s=String(u); if(s.indexOf('/api/wifi/forget')>=0) w.__fgsent.push(String((o&&o.body)||'')); }catch(e){}
        return of.apply(w,arguments);
      };
    }
    w.__fgsent.length=0;
    return 1})()`);
  const gotSent = () => ev(`${W}.__fgsent`);

  // 当前弹层里的组名 + 空框数
  const state = () => ev(`(function(){
    var d=${D}, h=d.getElementById('sheetHost');
    if(!h) return { names:[], slots:0, total:0 };
    return {
      names: [].slice.call(h.querySelectorAll('[data-forget]')).map(function(x){ return x.dataset.forget; }),
      slots: h.querySelectorAll('.wsName').length,
      total: h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length
    };
  })()`);

  await send('Page.navigate', { url: PAGE });
  for (let i = 0; i < 60; i++) {
    if (await ev(`!!(${D}&&${D}.getElementById('btnWifi'))`)) break;
    await sleep(400);
  }
  await ev("(function(){var b=document.getElementById('tabApp');if(b)b.click();return 1})()");
  await sleep(900);
  await ev(`(function(){var d=${D};var b=d.querySelector('.tab[data-tab="settings"]');if(b)b.click();return 1})()`);
  await sleep(700);
  await ev(`(function(){try{${W}.bindPages();return 1}catch(e){return 'ERR:'+e.message}})()`);
  await sleep(300);

  console.log('[1] 打开 WiFi 页，能读到机器里的列表');
  await ev(`${D}.getElementById('btnWifi').click()`);
  await waitFor(`(function(){
    var h=${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-forget]').length > 0;
  })()`, 9000);
  const s0 = await state();
  ok(s0.names.length >= 2, '读到 ' + s0.names.length + ' 组：' + JSON.stringify(s0.names));
  ok(/世界第一快/.test(s0.names.join('|')), '列表里有「世界第一快」');
  const badge = await q('#wifiNow');
  ok(new RegExp(s0.names.length + '\\s*/\\s*4').test(String(badge)), '设置页那行显示「N / 4 组」：' + badge);

  console.log('\n[2] 点「忘掉」→ 先问一句，且说清后果');
  const victim = s0.names[0];
  await ev(`(function(){
    var d=${D};
    var bs=[].slice.call(d.querySelectorAll('#sheetHost [data-forget]'));
    for(var i=0;i<bs.length;i++){ if(bs[i].dataset.forget===${JSON.stringify(victim)}){ bs[i].click(); return 1 } }
    return 0})()`);
  await sleep(600);
  const tx = String(await q('#sheetHost'));
  ok(new RegExp('忘掉 ' + victim + '？').test(tx), '标题对得上（' + victim + '）');
  ok(/删掉这组名字和密码/.test(tx) || /最后一组/.test(tx), '说明白了会发生什么');
  const btns = await ev(`[...${D}.querySelectorAll('#sheetHost .btn')].map(b=>b.textContent.trim())`);
  ok(btns.indexOf('取消') >= 0 && btns.indexOf('忘掉') >= 0, '两个键：' + JSON.stringify(btns));

  console.log('\n[3] 确认之后：发的是这一组、并回了「还剩几组」');
  await armSent();
  await ev(`(function(){
    var d=${D};
    var bs=[...d.querySelectorAll('#sheetHost .btn')];
    for(var i=0;i<bs.length;i++){ if(bs[i].textContent.trim()==='忘掉'){ bs[i].click(); return 1 } }
    return 0})()`);
  await sleep(1500);
  const fg = await gotSent();
  ok(fg.length === 1, '发了 ' + fg.length + ' 次 /api/wifi/forget');
  const enc = 'ssid=' + encodeURIComponent(victim);
  ok(String(fg[0]) === enc || String(fg[0]).indexOf('ssid=' + victim) === 0,
     '带的是这一组：' + String(fg[0]));
  const tst = await q('#toastTx');
  ok(/已忘掉/.test(String(tst)), '提示「已忘掉」：' + tst);

  console.log('\n[4] ⭐ 界面必须自己刷新（这就是用户说「还在」的地方）');
  await waitFor(`(function(){
    var h=${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length === 4;
  })()`, 9000);
  const s1 = await state();
  ok(s1.names.length === s0.names.length - 1, '列表少了一组：' + s0.names.length + ' → ' + s1.names.length);
  ok(s1.names.indexOf(victim) < 0, '「' + victim + '」真的不在了：' + JSON.stringify(s1.names));
  ok(s1.total === 4, '空框补上那个位置，格子还是 4 个');
  const badge2 = await q('#wifiNow');
  ok(new RegExp(s1.names.length + '\\s*/\\s*4').test(String(badge2)), '设置页计数也跟着变：' + badge2);

  console.log('\n[5] 再问一次机器（关掉重开），确认不是界面骗人');
  await ev(`(function(){var d=${D};var m=d.getElementById('mask');if(m)m.click();return 1})()`);
  await sleep(600);
  await ev(`${D}.getElementById('btnWifi').click()`);
  await waitFor(`(function(){
    var h=${D}.getElementById('sheetHost');
    return h && (h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length) === 4;
  })()`, 9000);
  const s2 = await state();
  ok(s2.names.length === s1.names.length && s2.names.indexOf(victim) < 0,
    '重开后仍是 ' + s2.names.length + ' 组，且没有它：' + JSON.stringify(s2.names));

  console.log('\n[6] 机器说「本来就没有这一组」时，不许说成功');
  await ev(`(function(){
    var w=${W};
    w.__origF = w.__origF || w.fetch;
    w.fetch=function(u,o){
      if(String(u).indexOf('/api/wifi/forget')>=0){
        return Promise.resolve(new Response(JSON.stringify({err:'notfound'}),
          {status:404,headers:{'Content-Type':'application/json'}}));
      }
      return w.__origF.apply(w,arguments);
    };
    return 1})()`);
  await ev(`(function(){
    var d=${D}; var b=d.querySelector('#sheetHost [data-forget]');
    if(b) b.click(); return 1})()`);
  await sleep(500);
  await ev(`(function(){
    var d=${D}; var bs=[...d.querySelectorAll('#sheetHost .btn')];
    for(var i=0;i<bs.length;i++){ if(bs[i].textContent.trim()==='忘掉'){ bs[i].click(); return 1 } }
    return 0})()`);
  await sleep(1300);
  const t3 = await q('#toastTx');
  ok(/本来就没有/.test(String(t3)), '照实说「机器里本来就没有这一组」：' + t3);
  ok(!/已忘掉/.test(String(t3)), '没有假报成功');

  console.log('\n[7] 只剩最后一组时，警告要更重');
  // 先把多余的全忘掉（用真接口，走页面按钮会让弹层不停重建）
  await ev(`(function(){
    var w=${W};
    return w.fetch('/api/wifi/list').then(function(r){return r.json()}).then(function(j){
      var arr=(j.list||[]).slice(1);
      return arr.reduce(function(p,x){
        return p.then(function(){
          return w.fetch('/api/wifi/forget',{method:'POST',
            headers:{'Content-Type':'application/x-www-form-urlencoded'},
            body:'ssid='+encodeURIComponent(x.ssid)});
        });
      }, Promise.resolve());
    })}()`);
  await ev(`(function(){var d=${D};var m=d.getElementById('mask');if(m)m.click();return 1})()`);
  await sleep(400);
  await ev(`${D}.getElementById('btnWifi').click()`);
  await waitFor(`(function(){
    var h=${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-forget]').length === 1;
  })()`, 9000);
  const s3 = await state();
  if (s3.names.length === 1) {
    await ev(`(function(){var d=${D};var b=d.querySelector('#sheetHost [data-forget]');if(b)b.click();return 1})()`);
    await sleep(500);
    const t4 = String(await q('#sheetHost'));
    ok(/最后一组/.test(t4), '说了「这是机器记的最后一组」');
    ok(/开热点/.test(t4), '说了删完会开热点');
  } else {
    ok(false, '应该只剩 1 组，实际 ' + s3.names.length);
  }

  console.log('\n汇总：' + oks + ' / ' + (oks + fails));
  cdp.close();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('崩了：', e.message); process.exit(2); });
