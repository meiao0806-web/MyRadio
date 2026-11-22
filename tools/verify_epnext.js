// 验 1.46.33 的三件事（走 preview/index.html，即固件里真正的 webapp.h）：
//   [1] 弹层里不再出现「算了」，一律是「取消」
//   [2] 打开「选集」**不打断播放**（不发 /api/play），弹层能开
//   [3] 点某一集：只发 /api/epis（带台号），而且**弹层不关**（留在选集页）
//   [4] 「下次从第几集开始」：只写标记（/api/epnext），不换台、不起播、不动进度
//   [5] 真的调到那个台时，才从设的那一集起播；并且这条设置用完作废
//
//   node tools/verify_epnext.js
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
    const v = await ev(`(function(){var d=${D};return !!(d&&d.getElementById('chipBad'))})()`).catch(() => false);
    if (v) { ready = true; break; }
    await sleep(400);
  }
  if (!ready) { console.log('页面/iframe 没就绪'); process.exit(2); }
  await ev("(function(){var b=document.getElementById('tabApp');if(b)b.click();return 1})()");
  await sleep(900);

  // iframe 里的 realm：App 的 const 挂不在 window 上，得用 contentWindow.eval
  const ae = code => ev(`${W}.eval(${JSON.stringify(code)})`);

  // 记录所有 /api/ 请求（把 fetch 包一层，别的都不动）
  await ev(`(function(){var w=${W};
    if(!w.__reqs){ w.__reqs=[]; var of=w.fetch;
      w.fetch=function(u,o){ try{ w.__reqs.push({u:String(u),m:(o&&o.method)||'GET',
        b:(o&&o.body)?String(o.body):''}); }catch(e){} return of.apply(w,arguments); };
    } else w.__reqs.length=0; return 1})()`);
  const reqs = () => ev(`${W}.__reqs.map(function(r){return r.m+' '+r.u+' '+(r.b||'')})`);
  const clearReqs = () => ev(`${W}.__reqs.length=0`);

  // ps: 0 暂停 / 1 连接中 / 2 播放中 / 3 缓冲 / 4 出错
  // 「没被打断」= 保持不变，或本来在连接中(1)、这会儿正常进了播放中(2)。
  // 从 2 掉回 1、或变成 3/4，才算被这次操作打断。
  const psOk = (b, a) => (b === a) || (b === 1 && a === 2);

  console.log('[1] 弹层里的取消按钮');
  await goTab('stations');
  await sleep(400);
  await ae("confirmSheet({title:'试一下', desc:''})");
  await sleep(300);
  const noTx = await q('.sheet .acts .btn.ghost');
  ok(String(noTx) === '取消', '确认弹层的取消键：' + noTx);
  await ae("closeSheet(true)");
  await ae("openAddStation()");
  await sleep(300);
  const addNo = await q('#aNo');
  ok(String(addNo) === '取消', '添加电台弹层的取消键：' + addNo);
  const bodyAll = await ev(`${D}.body.innerText`);
  ok(String(bodyAll).indexOf('算了') < 0, '当前页面里没有「算了」');
  await ae("closeSheet(true)");

  console.log('\n[2] 打开选集不打断播放');
  const gc = await ae("S.list.findIndex(function(x){return x.n==='官场现形记'})");
  ok(gc > 0, '台表里找得到官场现形记：第 ' + (gc + 1) + ' 台');
  // 先让它成为当前台（这一步本来就要发 /api/play）
  await ae("post('/api/play',{id:" + gc + "}).then(function(){return pollScreen()})");
  await sleep(700);
  await clearReqs();
  const before = await ae("({idx:S.scr.idx, epi:S.scr.epi, ps:S.scr.ps})");
  await ae("openEpisodes(" + gc + ")");
  await sleep(700);
  const r2 = await reqs();
  ok(!r2.some(x => x.indexOf('/api/play') >= 0), '没有发 /api/play（' + JSON.stringify(r2) + '）');
  const sheetOpen = await ev(`!!${D}.querySelector('.sheet')`);
  ok(sheetOpen === true, '选集弹层开着了');
  const epH = await q('.sheet h3');
  ok(epH === '官场现形记', '弹层标题：' + epH);
  const after = await ae("({idx:S.scr.idx, epi:S.scr.epi, ps:S.scr.ps})");
  ok(before.epi === after.epi && psOk(before.ps, after.ps),
     '打开弹层没打断播放（epi ' + before.epi + '→' + after.epi + '，ps ' + before.ps + '→' + after.ps + '）');

  console.log('\n[3] 点某一集：换集但弹层不关');
  await clearReqs();
  const target = await ev(`(function(){var d=${D};var rs=[].slice.call(d.querySelectorAll('#epList .row[data-e]'));
    var pick=null; for(var i=0;i<rs.length;i++){ if(+rs[i].dataset.e>3){pick=rs[i];break;} }
    if(!pick) pick=rs[0]; pick.id='pickRow'; return +pick.dataset.e})()`);
  await ev(`${D}.getElementById('pickRow').click()`);
  await sleep(800);
  const r3 = await reqs();
  const epCall = r3.find(x => x.indexOf('/api/epis') >= 0);
  ok(!!epCall, '发了 /api/epis：' + epCall);
  ok(/e=(\d+)/.test(String(epCall)) && Number(String(epCall).match(/e=(\d+)/)[1]) === target,
     '带的是点的那一集 e=' + target);
  ok(new RegExp('id=' + gc + '\\b').test(String(epCall)), '并且带了台号 id=' + gc);
  const stillOpen = await ev(`!!${D}.querySelector('.sheet')`);
  ok(stillOpen === true, '弹层还开着（留在选集页）');
  const hi = await ev(`(function(){var d=${D};var r=d.querySelector('#epList .row.cur');return r?+r.dataset.e:-1})()`);
  ok(hi === target, '高亮跟着走到第 ' + (hi + 1) + ' 集');

  console.log('\n[4] 「下次从第几集开始」只写标记');
  await ae("closeSheet(true)");
  await sleep(200);
  await clearReqs();
  const play0 = await ae("({idx:S.scr.idx, epi:S.scr.epi, ps:S.scr.ps})");
  await ae("openEpNext(" + gc + ")");
  await sleep(400);
  ok((await q('.sheet h3')) === '下次从第几集开始', '弹层标题：' + (await q('.sheet h3')));
  await ev(`(function(){var d=${D};var i=d.getElementById('enVal');i.value='11';return 1})()`);
  await ev(`${D}.getElementById('enYes').click()`);
  await sleep(900);
  const r4 = await reqs();
  const nc = r4.find(x => x.indexOf('/api/epnext') >= 0);
  ok(!!nc, '发了 /api/epnext：' + nc);
  ok(/e=10\b/.test(String(nc)), '填 11 → 发 e=10（固件 0 基）');
  ok(!r4.some(x => x.indexOf('/api/play') >= 0), '整个过程没有发 /api/play（不换台）');
  ok(!r4.some(x => x.indexOf('/api/epis') >= 0), '也没有发 /api/epis（不起播）');
  const play1 = await ae("({idx:S.scr.idx, epi:S.scr.epi, ps:S.scr.ps})");
  ok(play0.idx === play1.idx && play0.epi === play1.epi && psOk(play0.ps, play1.ps),
     '播放一点没被打断（idx ' + play0.idx + '/' + play1.idx + '，epi ' + play0.epi + '/' + play1.epi +
     '，ps ' + play0.ps + '→' + play1.ps + '）');
  const now = await ae("S.scr.epnext");
  ok(now === 10, '屏上 epnext = 10（第 11 集）');

  console.log('\n[5] 调到这个台时才生效，且用完作废');
  // 先转走，再转回来 —— 模拟"下次调到这个台"
  await ae("post('/api/play',{id:" + (gc - 1) + "}).then(function(){return pollScreen()})");
  await sleep(600);
  await ae("post('/api/play',{id:" + gc + "}).then(function(){return pollScreen()})");
  await sleep(900);
  const t = await ae("({epi:S.scr.epi, epnext:S.scr.epnext, idx:S.scr.idx})");
  ok(t.idx === gc, '调到这一台了（idx=' + t.idx + '）');
  ok(t.epi === 10, '从设的第 11 集起播（epi=' + t.epi + '）');
  ok(t.epnext === -1, '设置用完就作废（epnext=' + t.epnext + '）');

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  require('fs').writeFileSync(path.join(__dirname, '_shot_epnext.png'), Buffer.from(shot.data, 'base64'));

  cdp.close();
  console.log('\n' + oks + ' 通过 / ' + fails + ' 失败');
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('脚本异常：', e.message); process.exit(2); });
