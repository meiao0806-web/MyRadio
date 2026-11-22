// 验 1.46.51：「换一个 WiFi」和「已存的 WiFi」合并成一页，固定 4 个格子
//
// 用户要的语义：
//   · 已存的格子显示名字，空着的格子直接填名字+密码就存进机器
//   · 每个格子后面有搜索符号，点了扫描，选一个自动把名字填进格子
//   · 保存**不该**打断当前连接（只存不连），也不是「换一个」那种当场切换
//
// 跑法：python tools/run_page_test.py verify_wifi_slots.js
const PORT = Number(process.env.CDP_PORT || 9333);
const path = require('path');
const PAGE = 'file:///' + path.resolve(__dirname, '..', 'preview', 'index.html')
  .replace(/\\/g, '/').replace(/^\//, '');

let oks = 0, fails = 0;
const ok = (c, m, extra) => {
  c ? (oks++, console.log('  ✓ ' + m)) : (fails++, console.log('  ✗ ' + m + (extra ? '  << ' + extra : '')));
};
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
    if (r.exceptionDetails) {
      return { __err: (r.exceptionDetails.exception && (r.exceptionDetails.exception.description || '')) || 'err' };
    }
    return r.result.value;
  }
  // 等条件成立再断言（不要用固定 sleep —— 会读到渲染到一半的状态）
  async function waitFor(code, ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < (ms || 6000)) {
      if (await ev(code)) return true;
      await sleep(120);
    }
    return false;
  }
  const W = "document.getElementById('appFrame').contentWindow";
  const D = "document.getElementById('appFrame').contentDocument";
  const q = sel => ev(`(function(){var d=${D};var e=d&&d.querySelector(${JSON.stringify(sel)});return e?e.textContent.trim():null})()`);

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

  // 把 fetch 挂上探针，记录 App 到底发了哪些请求
  await ev(`(function(){
    var w = ${W};
    if (!w.__sent) {
      w.__sent = [];
      var of = w.fetch;
      w.fetch = function(u, o){
        try{
          var s = String(u);
          if (s.indexOf('/api/') >= 0) w.__sent.push(s.replace(/^https?:\\/\\/[^/]+/, '') + ((o && o.body) ? ('?' + o.body) : ''));
        }catch(e){}
        return of.apply(w, arguments);
      };
    }
    w.__sent.length = 0;
    return 1;
  })()`);

  console.log('[1] 设置页只剩一行 WiFi（两个入口合并）');
  const rows = await ev(`(function(){
    var d = ${D};
    return {
      w: !!d.getElementById('btnWifi'),
      old: !!d.getElementById('btnSavedWifi'),
      title: (d.getElementById('wifiTitle') || {}).textContent || '',
      sub: (d.getElementById('wifiSub') || {}).textContent || '',
      right: (d.getElementById('wifiNow') || {}).textContent || ''
    };
  })()`);
  ok(rows.w, '有「WiFi」这一行');
  ok(!rows.old, '旧的「已存的 WiFi」那行已经不在（合并掉了）');
  ok(/WiFi/.test(rows.title), '标题是 WiFi：' + rows.title);
  ok(/管理|点这里/.test(rows.sub), '副标题说清点进去能做什么：' + rows.sub);
  ok(/(\d)\s*\/\s*(\d)\s*组/.test(rows.right), '右边直接显示「已存 / 容量」：' + rows.right);

  console.log('\n[2] 点开是 4 个格子（2 个已存 + 2 个空）');
  await ev(`${D}.getElementById('btnWifi').click()`);
  // 等「已存 + 空槽 = 4」这个不变式成立（渲染完成）
  const ready1 = await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    if (!h) return false;
    var f = h.querySelectorAll('[data-forget]').length;
    var e = h.querySelectorAll('.wsName').length;
    return (f + e) === 4;
  })()`, 8000);
  ok(ready1, '弹层渲染完成（4 个格子都在）');
  const g = await ev(`(function(){
    var d = ${D}, host = d.getElementById('sheetHost'), t = host.textContent;
    var names = [].slice.call(host.querySelectorAll('.wsName')).map(function(x){ return x.dataset.i; });
    var saves = [].slice.call(host.querySelectorAll('[data-save]')).map(function(x){ return x.dataset.save; });
    var scans = [].slice.call(host.querySelectorAll('[data-scan]')).map(function(x){ return x.dataset.scan; });
    return {
      max: (d.getElementById('wfMax') || {}).textContent,
      names: names, saves: saves, scans: scans,
      first: t.indexOf('世界第一快') >= 0,
      second: t.indexOf('ChinaNet-home') >= 0,
      onTag: t.indexOf('正在用') >= 0,
      emptyHint: (t.match(/空着/g) || []).length,
      hasSearchIcon: host.querySelectorAll('[data-scan] svg').length
    };
  })()`);
  ok(g.first && g.second, '两组已存的都列出来了');
  ok(g.onTag, '正在用的那组标了「正在用」');
  ok(String(g.max) === '4', '页头写的是 4 组：' + g.max);
  ok(g.names.length === 2, '恰好 2 个空槽输入框（4 格 - 2 已存）：' + JSON.stringify(g.names));
  ok(JSON.stringify(g.saves) === JSON.stringify(g.names), '每个空槽都有「保存这组」');
  ok(JSON.stringify(g.scans) === JSON.stringify(g.names), '每个空槽都有 🔍 按钮');
  ok(g.hasSearchIcon === g.scans.length, '🔍 按钮里真的有图标：' + g.hasSearchIcon);
  ok(g.emptyHint >= 2, '空槽有「空着」提示，用户知道能填');

  console.log('\n[3] 空槽填名字+密码 → 保存：只发 add，不发 use/wifi');
  await ev(`(function(){
    var d = ${D};
    d.querySelector('.wsName[data-i="2"]').value = '公司WiFi';
    d.querySelector('.wsPass[data-i="2"]').value = 'pw2';
    d.querySelector('.wsName[data-i="3"]').value = '留着别丢';
    d.querySelector('.wsPass[data-i="3"]').value = 'pw3';
    d.querySelector('[data-save="2"]').click();
    return 1;
  })()`);
  await sleep(1200);
  const sent1 = await ev(`${W}.__sent`);
  const addc = String(sent1).split(',').filter(x => /^\/api\/wifi\/add/.test(x));
  const usec = String(sent1).split(',').filter(x => /^\/api\/wifi\/use/.test(x));
  const rawc = String(sent1).split(',').filter(x => /^\/api\/wifi\?/.test(x));
  ok(addc.length === 1, '保存只发了一次 /api/wifi/add：' + JSON.stringify(addc));
  ok(/ssid=/.test(String(addc[0])), '带上了名字：' + addc[0]);
  ok(usec.length === 0, '没发 /api/wifi/use（存 ≠ 切换）');
  ok(rawc.length === 0, '没发 /api/wifi（不会当场断网换台）');

  console.log('\n[4] 保存后重开页面 = 重新问机器，新组出现在列表里');
  await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return h && h.textContent.indexOf('公司WiFi') >= 0;
  })()`, 8000);
  const g2 = await ev(`(function(){
    var d = ${D}, host = d.getElementById('sheetHost'), t = host.textContent;
    return { has3: t.indexOf('公司WiFi') >= 0, slots: host.querySelectorAll('.wsName').length };
  })()`);
  ok(g2.has3, '新存的那组在列表里');
  ok(g2.slots === 1, '已存 3 组 → 只剩 1 个空框：' + g2.slots);

  console.log('\n[5] 点 🔍 → 扫描 → 点一个名字自动填回空框');
  await ev(`${D}.querySelector('[data-scan]').click()`);
  const ready2 = await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-p]').length > 0;
  })()`, 10000);
  ok(ready2, '扫描结果出来了');
  const scan = await ev(`(function(){
    var d = ${D}, host = d.getElementById('sheetHost');
    return {
      title: (host.querySelector('h3') || {}).textContent || '',
      opts: [].slice.call(host.querySelectorAll('[data-p]')).map(function(x){ return x.textContent.replace(/\\s+/g, ' ').trim(); })
    };
  })()`);
  ok(/选一个 WiFi/.test(scan.title), '弹的是「选一个 WiFi」：' + scan.title);
  ok(scan.opts.length >= 2, '扫到 ' + scan.opts.length + ' 个网络：' + JSON.stringify(scan.opts));
  ok(String(scan.opts.join('|')).indexOf('TP-LINK_5G') >= 0, '列表里有 TP-LINK_5G');

  await ev(`${D}.querySelector('[data-p="0"]').click()`);
  await sleep(1200);
  const filled = await ev(`(function(){
    var d = ${D};
    var el = d.querySelector('.wsName');
    return { v: el ? el.value : null, slots: d.querySelectorAll('.wsName').length };
  })()`);
  ok(filled.v === 'TP-LINK_5G', '名字自动填进空框：' + filled.v);
  ok(filled.slots === 1, '还是那 1 个空框（填了没存，位置不变）');

  console.log('\n[6] 密码留空也能存（开放网络 / 访客 WiFi）');
  await ev(`(function(){
    var d = ${D};
    d.querySelector('.wsPass').value = '';
    d.querySelector('[data-save]').click();
    return 1;
  })()`);
  await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return h && h.textContent.indexOf('TP-LINK_5G') >= 0;
  })()`, 8000);
  const g3 = await ev(`(function(){
    var d = ${D}, t = d.getElementById('sheetHost').textContent;
    return { n: d.querySelectorAll('.wsName').length, full: t.indexOf('TP-LINK_5G') >= 0 };
  })()`);
  ok(g3.full, '留空密码也存上了（TP-LINK_5G 在列表里）');
  ok(g3.n === 0, '4 组满了 → 一个空框都不剩：' + g3.n);

  console.log('\n[7] 满了再加：固件回 full，App 要照实说，不许静默');
  const full = await ev(`(function(){
    var w = ${W};
    return w.fetch('/api/wifi/add', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'ssid=' + encodeURIComponent('第五个') + '&pass=x'
    }).then(function(r){
      return r.json().then(function(j){ return { status: r.status, body: j }; });
    });
  })()`);
  ok(full && full.body && full.body.err === 'full',
     '满了回 full：' + JSON.stringify(full));
  ok(full && full.status === 409, '而且带 409（App 能按 e.code 分流）：' + (full && full.status));

  console.log('\n[8] 点已存的那组「连这个」→ 发 use，然后进等待条');
  await sleep(300);
  const useSent = await ev(`(function(){
    var w = ${W}, d = ${D};
    w.__sent.length = 0;
    var btns = [].slice.call(d.querySelectorAll('[data-use]'));
    if (!btns.length) return { n: 0 };
    var target = btns[0].dataset.use;
    btns[0].click();
    return { n: btns.length, target: target };
  })()`);
  ok(useSent.n >= 1, '有「连这个」按钮：' + JSON.stringify(useSent));
  await sleep(600);
  const cfm = await q('#sheetHost');
  ok(/连到/.test(String(cfm)), '先确认一句：' + String(cfm).slice(0, 40));
  await ev(`(function(){
    var d = ${D};
    var bs = [].slice.call(d.querySelectorAll('#sheetHost .btn'));
    for (var i = 0; i < bs.length; i++) if (bs[i].textContent.trim() === '连这个'){ bs[i].click(); return 1; }
    return 0;
  })()`);
  await sleep(1500);
  const s2 = await ev(`${W}.__sent`);
  const usec2 = String(s2).split(',').filter(x => /^\/api\/wifi\/use/.test(x));
  ok(usec2.length === 1, '发了一次 /api/wifi/use：' + JSON.stringify(usec2));
  const wait = await ev(`(function(){
    var d = ${D}, t = d.getElementById('sheetHost').textContent;
    return { waiting: /正在连/.test(t), msg: t.slice(0, 60) };
  })()`);
  ok(wait.waiting, '进了「正在连…」等待条：' + wait.msg);

  console.log('\n[9] 电量：App 跟机器同步，并且把「已充满 / 接着电源」说出来');
  await ev(`(function(){var d=${D};var m=d.getElementById('mask');if(m)m.click();return 1})()`);
  await sleep(400);
  const batt = await ev(`(function(){
    var w = ${W};
    w.renderBatt({ bok: 1, batt: 73, bmv: 3841, chg: 0, blow: 0, bfull: 1, bext: 1, bcal: 1.0 });
    var d = ${D};
    return {
      now: (d.getElementById('battNow') || {}).textContent || '',
      sub: (d.getElementById('battSub') || {}).textContent || '',
      hasPoll: typeof w.startBattPoll === 'function'
    };
  })()`);
  ok(batt.now === '73%', '百分比跟着机器走：' + batt.now);
  ok(/已充满/.test(batt.sub), '满电时写「已充满」（以前这里空白）：' + batt.sub);
  ok(/3\.84 V/.test(batt.sub), '电压也在：' + batt.sub);
  ok(batt.hasPoll, '有 20 秒一次的电量刷新（不是只在进页面时读一次）');

  console.log('\n[10] 忘掉一组：走 forget，且重开后列表真的少一组');
  // [8] 的等待条还在每 1.6 秒问 /api/wifi/status，连上后它会 closeSheet + loadSettings ——
  //   不先等它收尾，这里刚打开的弹层会被它关掉（实测「先问一句」读到空字符串）。
  await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return !h || h.children.length === 0;
  })()`, 12000);
  await sleep(600);
  await ev(`${D}.getElementById('btnWifi').click()`);
  await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-forget]').length > 0;
  })()`, 9000);
  await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    return h && h.querySelectorAll('[data-forget]').length > 0;
  })()`, 8000);
  const n0 = await ev(`(function(){
    var d = ${D}, h = d.getElementById('sheetHost');
    return h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length;
  })()`);
  // 记住「点的是哪一组」——[8] 里 use 过一组，列表顺序已经变了，不能硬写名字
  const victim = await ev(`(function(){
    var d = ${D};
    var b = d.querySelector('#sheetHost [data-forget]');
    if (!b) return null;
    var name = b.dataset.forget;
    b.click();
    return name;
  })()`);
  ok(!!victim, '点到某一组的「忘掉」：' + victim);
  await sleep(600);
  const warn = await q('#sheetHost');
  ok(/忘掉/.test(String(warn)), '先问一句：' + String(warn).slice(0, 40));
  await ev(`(function(){
    var d = ${D};
    var bs = [].slice.call(d.querySelectorAll('#sheetHost .btn'));
    for (var i = 0; i < bs.length; i++) if (bs[i].textContent.trim() === '忘掉'){ bs[i].click(); return 1; }
    return 0;
  })()`);
  // 忘掉后 App 会「重开一次页面」= 重新问机器（1.46.36 的修法），等它稳定
  await sleep(1600);
  const n1 = await waitFor(`(function(){
    var h = ${D}.getElementById('sheetHost');
    if (!h) return false;
    return (h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length) === 4;
  })()`, 8000);
  const after = await ev(`(function(){
    var d = ${D}, h = d.getElementById('sheetHost');
    return {
      total: h.querySelectorAll('[data-forget]').length + h.querySelectorAll('.wsName').length,
      slots: h.querySelectorAll('.wsName').length,
      hasVictim: h.textContent.indexOf(${JSON.stringify(String(victim))}) >= 0
    };
  })()`);
  ok(n1, '忘掉后重开，格子总数仍是 4（少一组就多一个空框）：' + JSON.stringify(after));
  ok(!after.hasVictim, '忘掉的那组（' + victim + '）真的不在了（重新问过机器）');

  console.log('\n———————— 汇总 ————————');
  console.log(oks + ' / ' + (oks + fails) + ' 通过');
  cdp.close();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('崩了：' + (e && e.stack || e)); process.exit(2); });
