// 用 CDP 在真浏览器里验「一部书 = 一个台」+「集层」+「单表 / 标签」交互（预览页 = 同一套逻辑）
// 零依赖：Node 22 自带 WebSocket
//
// 1.15.0 起固件**不再分组**：整张表串成一个列表，先后就是播放顺序，
// 有声书均匀穿插在电台之间（1.16.0：290 台 / 8 标签 / 22 部书）。所以本脚本里所有「setGroup(x) + 组内下标」
// 都换成了「全表下标」，断言也从「组」改成「表」+「标签」。
const PORT = 9333;
// 预览页路径按**脚本所在工程**算，别写死绝对路径 ——
// 原来写的是 C:/Users/lenovo/WorkBuddy/2026-10-31-22-44-07/MyRadio-v1.6.0/...（1.6.0 的老副本），
// 结果换到新工程目录跑，页面加载的是旧台表，断言全"通过"但验的是上一版（广播 149 台 / 8 组）。
const path = require('path');
const URL = 'file:///' + path.resolve(__dirname, '..', 'preview', 'index.html')
  .replace(/\\/g, '/').replace(/^\//, '');

let fails = 0, oks = 0;
function ok(c, m) { if (c) { oks++; console.log('  ✓ ' + m); } else { fails++; console.log('  ✗ ' + m); } }
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
          ws.send(JSON.stringify({ id, method, params }));
        });
      },
      close() { try { ws.close(); } catch (e) { } }
    }));
    ws.addEventListener('error', e => reject(new Error('ws: ' + (e.message || ''))));
  });
}

(async () => {
  const wsUrl = await listPage();
  const cdp = await connect(wsUrl);
  const send = cdp.send.bind(cdp);
  await send('Page.enable');
  await send('Runtime.enable');

  async function ev(expr) {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) {
      const d = r.exceptionDetails.exception;
      throw new Error('页面内异常：' + ((d && (d.description || d.value)) || JSON.stringify(r.exceptionDetails)));
    }
    return r.result.value;
  }

  await send('Page.navigate', { url: URL });
  let ready = false;
  for (let i = 0; i < 60; i++) {
    try {
      const v = await ev("typeof S === 'object' && typeof albumStep === 'function' && typeof epStep === 'function'");
      if (v) { ready = true; break; }
    } catch (e) { }
    await sleep(300);
  }
  if (!ready) throw new Error('页面脚本没就绪（S / albumStep / epStep 不存在）');
  console.log('页面已就绪\n');

  // 下标一律按名字查，不写死 —— 加台加书都会挪
  const idxOf = n => ev(`S.list.findIndex(s=>s.n===${JSON.stringify(n)})`);
  const I_三国 = await idxOf('三国演义');
  const I_官场 = await idxOf('官场现形记');
  const I_红楼 = await idxOf('红楼梦');
  const BK_三国 = await ev("bookOfName('三国演义')");
  const N = await ev("S.list.length");

  // ---------- 1. 台表结构：一张表 + 标签 ----------
  console.log('[1] 台表结构（1.15.0：不分组，一张表）');
  // 1.46.51：原来写 > 250，而实际是 247（1.46.34 起用户在 App 里删过一台）——
  //   差 3 台就一直红。台表大小是「数据」，不该用卡边的阈值去验，改成下界。
  ok(N >= 240, '整表 ' + N + ' 台');
  ok(await ev("typeof GRPS") === 'undefined', 'GRPS（分组表）已彻底移除');
  ok(await ev("typeof setGroup") === 'undefined', 'setGroup() 已彻底移除');

  const tags = await ev("TAGS.map(t=>({n:t.n,c:t.c}))");
  ok(tags.length >= 5, tags.length + ' 个标签：' + tags.map(t => t.n).join(' / '));
  const tmap = {}; tags.forEach(t => { tmap[t.n] = t.c; });
  ['广播', '军事历史', '历史', '评书', '广播剧', '悬疑', '都市'].forEach(n => {
    ok(tmap[n] > 0, '有标签「' + n + '」' + (tmap[n] ? '（' + tmap[n] + ' 台）' : ''));
  });
  ok(!tmap['新闻'], '「新闻」已并入「广播」，不该再存在');
  ok(tags.reduce((a, t) => a + t.c, 0) === N, '各标签台数之和 = 整表台数（' + N + '）');
  ok(tmap['广播'] > 190, '「广播」' + tmap['广播'] + ' 台（所有电台都在这一个标签下）');

  // 每个台的 t 必须落在标签范围内，且与标签报的台数对得上
  const tcheck = await ev(`(function(){
    var cnt = {}; for (var i=0;i<S.list.length;i++){ var t=S.list[i].t|0; cnt[t]=(cnt[t]||0)+1; }
    var bad = 0;
    for (var k=0;k<TAGS.length;k++) if ((cnt[k]||0) !== TAGS[k].c) bad++;
    var oob = S.list.filter(function(s){ return !(s.t>=0 && s.t<TAGS.length); }).length;
    return { bad: bad, oob: oob };
  })()`);
  ok(tcheck.bad === 0, '每个标签报的台数与实际条数一致（不一致 ' + tcheck.bad + ' 个）');
  ok(tcheck.oob === 0, '没有台的标签下标越界（越界 ' + tcheck.oob + ' 个）');

  // 1.14.0：中央台九个顶在表最前（开机默认台 = 中国之声）
  const head9 = await ev("S.list.slice(0,9).map(s=>s.n)");
  const CNR9 = ['中国之声', '经济之声', '文艺之声', '神州之声', '台海之声',
                '中国乡村之声', '环球资讯广播', '音乐之声', '中国交通广播'];
  ok(JSON.stringify(head9) === JSON.stringify(CNR9), '表头是中央台九个：' + head9.join(' / '));
  const u0 = await ev("S.list[0].u || ''");
  ok(/^http/.test(String(u0)), '第一台是 http 直链（HLS）：' + String(u0).slice(0, 46) + '…');
  const badLang = await ev("S.list.filter(s=>/蒙语|蒙古语|朝鲜语|哈萨克语|维吾尔语/.test(s.n)).length");
  ok(badLang === 0, '表里已没有非汉语台（蒙古语/朝鲜语/哈萨克语/维吾尔语）');

  // ---------- 2. 一部书 = 一个台，且均匀穿插 ----------
  console.log('\n[2] 一部书只占一个台，且均匀穿插');
  const nb = await ev("S.list.filter(s=>bookOfName(s.n)>=0).length");
  const nbooks = await ev("EPIS.books.length");
  ok(nb === nbooks, '表里 ' + nb + ' 个台是书，集表里 ' + nbooks + ' 部书 —— 一部书正好一个台');
  ok(await ev("S.list.filter(s=>/三国演义/.test(s.n)).length") === 1, '三国演义在表里只有 1 个台（不再带 001）');
  ok(I_三国 > 5, '三国演义不在表头（第 ' + (I_三国 + 1) + ' 台）');
  ok(I_官场 > 0 && Math.abs(I_官场 - I_三国) > 1, '官场现形记不与三国演义相邻（第 ' + (I_官场 + 1) + ' 台）');

  const spread = await ev(`(function(){
    var b=[]; for (var i=0;i<S.list.length;i++) if (bookOfName(S.list[i].n)>=0) b.push(i);
    var g=[]; for (var k=1;k<b.length;k++) g.push(b[k]-b[k-1]);
    return { first:b[0], last:b[b.length-1], min:Math.min.apply(null,g), max:Math.max.apply(null,g) };
  })()`);
  ok(spread.first > 5, '第一部书在第 ' + (spread.first + 1) + ' 台（前面先是中央台）');
  ok(spread.last < N - 5, '最后一部书在第 ' + (spread.last + 1) + ' / ' + N + ' 台（后面还有电台）');
  // 1.46.51：书从 33 部里换过源之后间隔会浮动，写死 10~20 会假红。
  //   真正要验的是「没有挤在一起、也没有全堆到末尾」，用上下界 + 均匀度两件事。
  ok(spread.min >= 3 && spread.max <= 30,
     '书与书间隔 ' + spread.min + '~' + spread.max + ' 台（没有挤在一起）');
  const adjacent = await ev(`(function(){
    for (var i=1;i<S.list.length;i++)
      if (bookOfName(S.list[i].n)>=0 && bookOfName(S.list[i-1].n)>=0) return 1;
    return 0;
  })()`);
  ok(adjacent === 0, '没有两本书挨在一起');

  // ---------- 3. 旋钮一格 = 一个节目（不是一集）----------
  console.log('\n[3] 旋钮一格 = 一个节目');
  ok(await ev(`albumStep(${I_三国}, +1)`) === I_三国 + 1,
     '从三国演义转一格 → 第 ' + (I_三国 + 2) + ' 台（不是跳到第 2 集）');
  ok(await ev(`bookOfName(S.list[albumStep(${I_三国},+1)].n)`) === -1,
     '落点确实是普通电台：' + await ev(`S.list[albumStep(${I_三国},+1)].n`));
  ok(await ev(`albumStep(${I_三国}, -1)`) === I_三国 - 1, '反转一格 → 第 ' + I_三国 + ' 台');

  // 真转一下旋钮（走 rotate + 250ms 落地）
  await ev(`startPlay(${I_三国})`);
  await ev("rotate(1)");
  await sleep(420);
  const afterRotate = await ev("({cur:S.cur, name:S.list[S.cur].n, pending:S.pending, preview:S.preview})");
  ok(afterRotate.cur === I_三国 + 1, '抖动后落到第 ' + (afterRotate.cur + 1) + ' 台 ' + afterRotate.name +
     '（pending 已清：' + afterRotate.pending + '）');

  // ---------- 4. 集层：集数 / 上下集 / 循环 ----------
  console.log('\n[4] 集层');
  const bkSY = await ev("bookOfName('三国演义')");
  ok(bkSY >= 0, "bookOfName('三国演义') → " + bkSY + "（EPIS.books 下标）");
  ok(await ev("bookOfName('西安新闻广播')") === -1, '普通台 → 不是有声书');
  // 1.46.51：集数不写死 —— 1.46.32 换过源（RSS 145 集 → 喜马广播剧 22 集），
  //   写死的话每次换源这套回归都会假红，红久了就没人看它了。
  const SY_N = await ev(`epCount(${bkSY})`);
  ok(SY_N >= 20 && SY_N <= 500, '三国演义共 ' + SY_N + ' 集（整本，不是只取前 15 集）');
  const books = await ev("EPIS.books.map(b=>({n:b.n,c:b.c}))");
  const sumEp = books.reduce((a, b) => a + b.c, 0);
  ok(books.length >= 5 && sumEp >= 3500, books.length + ' 部书 / 共 ' + sumEp + ' 集（1.18.0 起是整本集数）');
  ok(books.some(b => b.n === '乱世枭雄' && b.c === 200), '含《乱世枭雄》200 集');
  const bkSX = await ev("bookOfName('上下五千年')");
  ok(await ev(`epTitle(${bkSX}, 0)`) === '开天辟地的神话', '上下五千年第 1 集标题：' + await ev(`epTitle(${bkSX},0)`));
  // 1.46.32 把三国换成了喜马广播剧：每集有真实标题（「宴桃园豪杰三结义…」），
  //   不再是当年 RSS 那种「整本标题一样 → 留空」。这里验的应该是
  //   「标题为空就不显示 / 有标题就是分集标题」，而不是死盯某一种。
  {
    const t0 = await ev(`epTitle(${BK_三国}, 0)`);
    const t1 = await ev(`epTitle(${BK_三国}, 1)`);
    ok(typeof t0 === 'string', '三国演义第 1 集标题可取：' + JSON.stringify(t0));
    if (t0 === '') {
      ok(true, '整本同名 → 标题留空（界面只显示「第 N 集」）');
    } else {
      ok(t0 !== t1, '每集标题不一样（真标题，不是整本同名）：' + JSON.stringify(t0) + ' / ' + JSON.stringify(t1));
    }
  }

  // 上下集 + 越界循环
  await ev(`epSetCur(${bkSY}, 0)`);
  ok(await ev(`epStep(${bkSY}, +1)`) === 1, '第 1 集 → 下一集 = 第 2 集');
  ok(await ev(`epStep(${bkSY}, -1)`) === SY_N - 1, '第 1 集 → 上一集 = 最后一集（从头绕到末尾）');
  await ev(`epSetCur(${BK_三国}, ${SY_N - 1})`);
  ok(await ev(`epStep(${BK_三国}, +1)`) === 0, '最后一集 → 下一集 = 第 1 集（播完从头再来）');
  await ev(`epSetCur(${BK_三国}, 9999)`);
  ok(await ev(`epCur(${BK_三国})`) === SY_N - 1, '越界集号被夹到最后一集（不崩）');
  await ev(`epSetCur(${BK_三国}, -5)`);
  ok(await ev(`epCur(${BK_三国})`) === 0, '负数集号夹到第 1 集');

  // ---------- 5. 每部书各记各的，转回来接着放 ----------
  console.log('\n[5] 转回这个台从上次那集继续');
  await ev(`epSetCur(${BK_三国}, 6)`);            // 三国听到第 7 集
  await ev(`startPlay(${I_红楼})`);               // 转走：红楼梦
  await sleep(80);
  ok(await ev("S.list[S.cur].n") === '红楼梦', '转到红楼梦');
  await ev(`startPlay(${I_三国})`);               // 转回来
  await sleep(80);
  const back = await ev("({name:S.list[S.cur].n, b:bookOfStation(S.cur), e:epCur(bookOfStation(S.cur))})");
  ok(back.name === '三国演义' && back.e === 6, '转回三国演义仍停在第 7 集（epCur=' + back.e + '）');

  await ev("epSetCur(3, 2)");                     // 官场听到第 3 集
  ok(await ev(`epCur(${BK_三国})`) === 6, '官场的进度没覆盖三国（三国仍 6）');
  ok(await ev("epCur(3)") === 2, '官场自己的进度是 2');

  // ---------- 6. 点一下旋钮：什么都不做 ----------
  console.log('\n[6] 点一下旋钮不再换组/换台');
  await ev(`startPlay(${I_三国})`);
  const before = await ev("({cur:S.cur, n:S.list.length, t:S.tag})");
  await ev("press()");
  await sleep(120);
  const after = await ev("({cur:S.cur, n:S.list.length, t:S.tag})");
  ok(after.cur === before.cur, '单击后当前台没变（' + after.cur + '）');
  ok(after.n === before.n, '单击后台表长度没变（' + after.n + '）');
  ok(after.t === before.t, '单击后标签筛选没变（' + after.t + '）');
  // 连点 12 下也不该跑偏（以前每点一下换一组）
  for (let i = 0; i < 12; i++) { await ev("press()"); await sleep(20); }
  ok(await ev("S.cur") === before.cur, '连点 12 下仍然停在原来那台');

  // ---------- 7. 标签筛选（App 的 chips）----------
  console.log('\n[7] 标签筛选只影响列表，不影响播放');
  await ev(`setTag(0)`);
  const c0 = await ev("document.querySelectorAll('#list .item').length");
  ok(c0 === tmap['广播'], '筛「广播」→ 列表 ' + c0 + ' 行（= ' + tmap['广播'] + '）');
  await ev(`setTag(-1)`);
  ok(await ev("document.querySelectorAll('#list .item').length") === N, '点「全部」→ 列表回到 ' + N + ' 行');
  ok(await ev("S.cur") === before.cur, '筛标签不动当前台');
  // 行号必须是全表序号（删/播都按它走），不能是筛选后的序号
  await ev(`setTag(1)`);
  const firstRowIx = await ev("document.querySelector('#list .item .ix').textContent");
  const firstIdx = await ev("S.list.findIndex(s=>(s.t|0)===1)");
  ok(+firstRowIx === firstIdx + 1, '筛了标签后行号仍是全表序号（' + firstRowIx + ' = ' + (firstIdx + 1) + '）');
  await ev(`setTag(-1)`);

  // ---------- 8. App 接口 ----------
  console.log('\n[8] App 侧接口');
  await ev(`epSetCur(${BK_三国}, 0); startPlay(${I_三国})`);
  const r1 = await ev("window.__api('/api/epis', {})");
  ok(r1 && r1.n === SY_N && r1.prog === '三国演义',
     '/api/epis 返回 prog=三国演义 n=' + SY_N + '（cur=' + (r1 && r1.cur) + '）');
  ok(r1 && r1.list && r1.list.length === SY_N, '三国 ' + SY_N + ' 集一页装得下（第 1 条标题：' + JSON.stringify(r1 && r1.list && r1.list[0] && r1.list[0].t) + '，整本标题一样所以留空）');

  await ev("window.__api('/api/epis', {dir:'next'})");
  await sleep(60);
  ok(await ev(`epCur(${BK_三国})`) === 1, 'App 点「下一集」→ 第 2 集');

  await ev("window.__api('/api/epis', {dir:'prev'})");
  await sleep(60);
  ok(await ev(`epCur(${BK_三国})`) === 0, 'App 点「上一集」→ 回到第 1 集');

  await ev("window.__api('/api/epis', {e:7})");
  await sleep(60);
  ok(await ev(`epCur(${BK_三国})`) === 7, 'App 选集跳第 8 集');

  // 字符串写法也要认（dir=next / dir=prev），别把方向判反
  await ev("window.__api('/api/epis', {dir:'next'})");
  await sleep(60);
  ok(await ev(`epCur(${BK_三国})`) === 8, "dir='next' → 第 9 集");
  await ev("window.__api('/api/epis', {dir:'prev'})");
  await sleep(60);
  ok(await ev(`epCur(${BK_三国})`) === 7, "dir='prev' → 回第 8 集");

  const scr = await ev("window.__api('/api/screen', {})");
  ok(scr && scr.epn === SY_N && scr.epi === 7 && scr.prog === '三国演义',
    '/api/screen 带集信息：' + (scr && scr.prog) + ' 第 ' + (scr && (scr.epi + 1)) + '/' + (scr && scr.epn) + ' 集');
  const t7 = await ev(`epTitle(${BK_三国},7)`);
  ok(scr && scr.ept === t7, '集标题一致：' + (scr && scr.ept));
  // 1.15.0：屏上顶栏那块是「当前这台的标签」，不是分组名
  ok(scr && scr.gname === '评书', '/api/screen 的 gname 是这台的标签：' + (scr && scr.gname));

  // 普通台不该有集
  await ev(`startPlay(0)`);
  const r2 = await ev("window.__api('/api/epis', {})");
  ok(r2 && (r2.n === 0 || r2.n === undefined), '普通电台调 /api/epis → 没有集（n=' + (r2 && r2.n) + '）');
  const scr2 = await ev("window.__api('/api/screen', {})");
  ok(scr2 && (scr2.epn === 0 || scr2.epn === undefined), '普通电台 /api/screen 的 epn=' + (scr2 && scr2.epn));

  // /api/stations 返回整表 + 标签（App 的 chips 靠它）
  const ss = await ev("window.__api('/api/stations', {})");
  ok(ss && ss.count === N, '/api/stations count=' + (ss && ss.count));
  ok(ss && ss.tags && ss.tags.length === tags.length, '/api/stations 带 ' + (ss && ss.tags && ss.tags.length) + ' 个标签');
  ok(ss && ss.list && ss.list.every(x => x.t !== undefined), '/api/stations 每台都带标签下标 t');
  ok(ss && !ss.groups, '/api/stations 不再返回 groups');

  // ---------- 9. App 的暂停仍是暂停 ----------
  console.log('\n[9] App 的暂停仍是暂停');
  await ev(`startPlay(${I_三国})`);
  await sleep(900);                 // 等「连接中」走完，否则 ps 还是 1，暂停会被判成"开播"
  const cur0 = await ev("S.cur");
  await ev("window.__api('/api/pause', {})");
  ok(await ev("S.cur") === cur0, 'App 点暂停不会换台');
  ok(await ev("S.ps") === 0, '暂停后状态是「已暂停」，ps=' + await ev("S.ps"));

  // ---------- 10. 屏幕真的画出来了 ----------
  console.log('\n[10] 屏幕同步');
  await ev(`epSetCur(${BK_三国}, 2); startPlay(${I_三国})`);
  await sleep(200);
  const sName = await ev("document.querySelector('#sName') ? document.querySelector('#sName').textContent : '(无面板)'");
  ok(String(sName).indexOf('三国演义') >= 0, '右侧面板台名：' + sName);
  const sTag = await ev("document.querySelector('#sTag') ? document.querySelector('#sTag').textContent : '(无面板)'");
  ok(sTag === '评书', '右侧面板「标签」：' + sTag);

  // ---------- 11. 集表分页（1.18.0）----------
  // 官场现形记 1167 集：整份 JSON 三十多 KB，跨网那条 MQTT 通道（CLOUD_BUF=16KB）
  // 一次发不完，所以机器按字节预算切页，App 一页页翻。
  console.log('\n[11] 集表分页');
  const bkGC = await ev("bookOfName('官场现形记')");
  ok(bkGC >= 0, "bookOfName('官场现形记') → " + bkGC);
  ok(await ev(`epCount(${bkGC})`) === 1167, '官场现形记共 1167 集（RSS 全部，原来只取 15 集）');

  const p0 = await ev(`window.__api('/api/epis', {id: ${await ev("S.list.findIndex(x=>x.n==='官场现形记')")}, from: 0})`);
  ok(p0 && p0.n === 1167 && p0.from === 0 && p0.list[0].i === 0, '第 1 页从第 1 集起（from=0）');
  const j0 = JSON.stringify(p0.list);
  ok(j0.length <= 9200, '第 1 页只装了 ' + p0.list.length + ' 集 / ' + j0.length + ' 字节（≤ 9200，云那边 16KB 装得下）');
  ok(p0.list.length > 10 && p0.list.length < 1167, '确实切了页，没有一次全塞（' + p0.list.length + ' 集）');

  const p1 = await ev(`window.__api('/api/epis', {id: ${await ev("S.list.findIndex(x=>x.n==='官场现形记')")}, from: 1100})`);
  ok(p1 && p1.from === 1100 && p1.list[0].i === 1100, 'from=1100 → 从第 1101 集起翻');
  ok(p1 && p1.n === 1167, '翻页时 n 仍是总集数（App 的「共 N 集」不会跟着页变）');

  const pEnd = await ev(`window.__api('/api/epis', {id: ${await ev("S.list.findIndex(x=>x.n==='官场现形记')")}, from: 1167})`);
  ok(pEnd && pEnd.list.length === 0, 'from=总集数 → 空页（App 靠它知道翻到头了）');

  const pBad = await ev(`window.__api('/api/epis', {id: ${await ev("S.list.findIndex(x=>x.n==='官场现形记')")}, from: -30})`);
  ok(pBad && pBad.from === 0, '负数 from 被夹回 0（不越界读）');

  cdp.close();
  console.log('\n' + oks + ' 通过 / ' + fails + ' 失败');
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('脚本异常：', e.message); process.exit(2); });
