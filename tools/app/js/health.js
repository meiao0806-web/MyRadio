/* ============================================================
   health.js —— 台的健康档案（1.17.0）

   固件每播一个台就判一次（出没出声 / 连不连得上），结果存在机器上；
   这个文件负责：
     1. 每秒问一次 /api/health，把「正在试的这台」的实时电平画出来
     2. 电台页多一个「有问题 N」的筛选
     3. 每行右侧标出「连不上 / 没声音」
     4. 体检页去看全表统计、挨个试一遍

   数据格式（/api/health，固件 src/health.cpp 拼的）：
     n    台数
     s    每台一个字符：0 没测过 / 1 正常 / 2 连不上 / 3 没声音
     f    每台累计失败次数（0-9）
     c    { ok, bad, dim, unk } 各项台数
     cur  { i 台号, st 本次结论 0/1/2/3, lv 电平 0..100, sec 已播秒数, why }
   ============================================================ */
'use strict';

const HL_NAME = ['没测过', '正常', '连不上', '没声音'];

let _hlAt = 0, _hlBusy = false, _hlTimer = 0;

const hlSleep = ms => new Promise(r => setTimeout(r, ms));

/* ---------------- 读档 ---------------- */
function hlOf(i){
  const h = S.hl;
  if (!h || typeof h.s !== 'string') return 0;
  const c = h.s.charAt(i);
  return (c >= '0' && c <= '3') ? (+c) : 0;
}
function hlFailOf(i){
  const h = S.hl;
  if (!h || typeof h.f !== 'string') return 0;
  const c = h.f.charAt(i);
  return (c >= '0' && c <= '9') ? (+c) : 0;
}
function hlCountBad(){
  const c = S.hl && S.hl.c;
  return c ? (c.bad | 0) + (c.dim | 0) : 0;
}
// 正在播的这台用「本次会话」的结论，别的台用档案里的
function hlNow(i){
  const h = S.hl, c = h && h.cur;
  if (c && c.i === i && (c.st | 0) > 0) return c.st | 0;
  return hlOf(i);
}

/* ---------------- 轮询 ---------------- */
async function pollHealth(force){
  if (_hlBusy || S.offline) return;
  const gap = useCloud() ? 1800 : 900;
  if (!force && Date.now() - _hlAt < gap - 80) return;
  _hlAt = Date.now();
  _hlBusy = true;
  try{
    const h = await get('/api/health');
    if (h && typeof h.s === 'string'){
      S.hl = h;
      applyHealth();
    }
  }catch (e){
    // 老固件没有这个接口：静默关掉，页面照常
    if (e && /http 404/.test(String(e.message))) S.hlOff = true;
  }finally{
    _hlBusy = false;
  }
}

/* ---------------- 界面刷新 ---------------- */
function applyHealth(){
  const h = S.hl;
  if (!h) return;

  // 顶部「有问题」的数量
  const n = hlCountBad();
  const cnt = $('#chipBadCnt');
  if (cnt) cnt.textContent = n;
  const cb = $('#chipBad');
  if (cb) cb.classList.toggle('zero', n === 0);

  // 电台页每行的小标
  if (S.page === 'stations') syncRowHealth();

  // 主页：实时出声
  if (S.page === 'home') renderSound();

  // 体检页：统计
  if (S.page === 'diag') renderHlDiag();
}

function renderSound(){
  const bar = $('#lvBar'), tx = $('#sndTx');
  if (!bar || !tx) return;
  const c = S.hl && S.hl.cur;
  const ps = S.scr ? (S.scr.ps | 0) : 0;
  let lv = 0, s = '没在放', cls = '';

  if (S.offline){ s = '连不上收音机'; cls = 'bad'; }
  else if (ps === 0){ s = '没在放'; }
  else if (!c || c.i !== (S.scr ? S.scr.idx : -1)){ s = '正在连…'; }
  else {
    lv = Math.max(0, Math.min(100, c.lv | 0));
    if ((c.st | 0) === 1){ s = '在出声'; cls = 'ok'; }
    else if ((c.st | 0) === 2){ s = '连不上'; cls = 'bad'; }
    else if ((c.st | 0) === 3){ s = '没声音'; cls = 'dim'; }
    else { s = lv > 2 ? '在出声' : '等一下…'; cls = lv > 2 ? 'ok' : ''; }
  }
  bar.style.width = lv + '%';
  bar.className = cls;
  tx.textContent = s;
  tx.className = 'snd ' + cls;
}

// 只改「不一样的行」，不整表重画 —— 否则翻着列表突然回顶
function syncRowHealth(){
  $$('#stList .row').forEach(r => {
    const i = +r.dataset.i;
    const box = r.querySelector('.m');
    if (!box) return;
    const st = hlNow(i);
    let el = box.querySelector('.hl');
    let cls = '', txt = '';
    if (st === 2){
      const f = Math.min(hlFailOf(i), 9);
      cls = 'bad'; txt = f > 0 ? '连不上 ' + f + ' 次' : '连不上';
    } else if (st === 3){
      cls = 'dim'; txt = '没声音';
    }
    if (!cls){ if (el) el.remove(); return; }
    if (!el){
      el = document.createElement('span');
      el.dataset.hl = i;
      el.onclick = e => { e.stopPropagation(); openHealth(i); };
      box.appendChild(el);
    }
    if (el.className !== 'hl ' + cls) el.className = 'hl ' + cls;
    if (el.textContent !== txt) el.textContent = txt;
  });
}

/* ---------------- 单台详情 ---------------- */
async function openHealth(i){
  const s = S.list[i] || {};
  let d = null;
  try { d = await post('/api/health', { i }); } catch (e){}
  const st = d ? (d.st | 0) : hlOf(i);
  const fail = d ? (d.fail | 0) : hlFailOf(i);
  const lastOk = d ? (d.lastOk | 0) : 0;
  const lastChk = d ? (d.lastChk | 0) : 0;

  const tip = st === 2 ? '机器连不上这个地址，播到它会自动跳过。'
            : st === 3 ? '连上了、数据也在流，但解出来一直是静音 —— 多半是音频格式被标错了。'
            : st === 1 ? '这台出过声，是好的。'
            : '还没在这台机器上试过。';

  openSheet(
    '<h3>' + esc(d ? d.n : s.n) + '</h3>' +
    '<p class="desc">' + tip + '</p>' +
    '<div class="list" style="box-shadow:none">' +
      '<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">这台的状态</div></div>' +
        '<span class="hl ' + ['', 'ok', 'bad', 'dim'][st] + '" style="position:static;margin-left:auto">' +
        HL_NAME[st] + '</span></div>' +
      '<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">失败次数</div></div>' +
        '<span class="sv" style="margin-left:auto">' + fail + ' 次</span></div>' +
      '<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">上次正常出声</div></div>' +
        '<span class="sv" style="margin-left:auto">' + (lastOk ? fmtRel(lastOk) : '没有过') + '</span></div>' +
      '<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">上次检查</div></div>' +
        '<span class="sv" style="margin-left:auto">' + (lastChk ? fmtRel(lastChk) : '没查过') + '</span></div>' +
    '</div>' +
    '<div class="acts">' +
      '<button class="btn ghost" id="hNo">知道了</button>' +
      '<button class="btn pri" id="hTry">试一下这台</button>' +
    '</div>',
    sheet => {
      $('#hNo', sheet).onclick = closeSheet;
      $('#hTry', sheet).onclick = async () => {
        closeSheet();
        try { await post('/api/play', { id: i }); toast('正在试「' + (s.n || '') + '」'); pollScreen(); }
        catch (e){ toast('切台失败', true); }
      };
    }
  );
}

/* ---------------- 挨个试一遍 ---------------- */
let SCAN = null;

function openScanSheet(){
  const c = (S.hl && S.hl.c) || { ok: 0, bad: 0, dim: 0, unk: 0 };
  const unkIds = [], badIds = [], allIds = [];
  S.list.forEach((s, i) => {
    allIds.push(i);
    const st = hlOf(i);
    if (st === 0) unkIds.push(i);
    if (st === 2 || st === 3) badIds.push(i);
  });
  if (!S.list.length){ toast('台表还没读出来', true); return; }

  const opt = (a, n, m, dis) =>
    '<button class="opt" data-a="' + a + '"' + (dis ? ' disabled style="opacity:.45"' : '') + '>' +
      '<span style="flex:1">' + n + '</span><span class="r">' + m + '</span></button>';

  openSheet(
    '<h3>挨个试一遍</h3>' +
    '<p class="desc">机器会把台一个个开几秒，看它<b>到底有没有出声</b>。' +
      '好的台 2 秒就过，坏的台要等 13 秒才判死，中间会听见断断续续的片段。</p>' +
    '<div class="optlist">' +
      opt('unk', '只试没测过的', unkIds.length + ' 个台', !unkIds.length) +
      opt('bad', '重试有问题的', badIds.length + ' 个台', !badIds.length) +
      opt('all', '全试一遍', allIds.length + ' 个台') +
    '</div>',
    sheet => {
      $$('.opt', sheet).forEach(b => b.onclick = () => {
        const a = b.dataset.a;
        if (b.disabled) return;
        closeSheet();
        if (a === 'unk') startScan(unkIds);
        if (a === 'bad') startScan(badIds);
        if (a === 'all') startScan(allIds);
      });
    }
  );
}

async function startScan(ids){
  if (SCAN){ toast('正在试，先停一下', true); return; }
  if (!ids.length){ toast('没有要试的台'); return; }

  SCAN = { ids, k: 0, ok: 0, bad: 0, dim: 0, late: 0, stop: false,
           back: S.scr ? (S.scr.idx | 0) : -1, t0: Date.now(), nm: '' };

  openSheet(
    '<h3>正在试台</h3>' +
    '<p class="desc" id="scNow">—</p>' +
    '<div class="prog"><i id="scBar"></i></div>' +
    '<div class="scrow"><span id="scNum">0 / 0</span><span id="scTally"></span></div>' +
    '<div class="acts one"><button class="btn ghost" id="scStop">停下来</button></div>',
    sheet => {
      $('#scStop', sheet).onclick = () => {
        if (SCAN) SCAN.stop = true;
        $('#scStop', sheet).textContent = '正在停…';
      };
    }
  );

  const S0 = SCAN;
  for (let k = 0; k < S0.ids.length; k++){
    if (S0.stop) break;
    const id = S0.ids[k];
    S0.k = k + 1;
    S0.nm = (S.list[id] || {}).n || ('第 ' + (id + 1) + ' 台');
    scanUi();

    let v = 0;
    try { await post('/api/play', { id }); } catch (e){}
    const t0 = Date.now();
    while (!S0.stop && Date.now() - t0 < 13000){
      await hlSleep(400);
      await pollHealth(true);
      const c = S.hl && S.hl.cur;
      if (c && (c.i | 0) === id && (c.st | 0) >= 1){ v = c.st | 0; break; }
      if (Date.now() - t0 > 4200 && (S.hl && S.hl.cur && (S.hl.cur.i | 0) === id)) scanUi();
    }
    if (v === 1) S0.ok++;
    else if (v === 2) S0.bad++;
    else if (v === 3) S0.dim++;
    else S0.late++;
    scanUi();
  }

  const done = S0.k, total = S0.ids.length;
  const txt = '试了 ' + done + '/' + total + ' 个：' +
              S0.ok + ' 个正常' +
              (S0.bad ? '，' + S0.bad + ' 个连不上' : '') +
              (S0.dim ? '，' + S0.dim + ' 个没声音' : '') +
              (S0.late ? '，' + S0.late + ' 个没测出来' : '');
  SCAN = null;
  closeSheet(true);

  // 回到试之前那个台，别把人正在听的换走
  if (S0.back >= 0 && S0.back < S.list.length) { try { await post('/api/play', { id: S0.back }); } catch (e){} }
  await pollHealth(true);
  if (S.page === 'stations') renderStList();
  if (S.page === 'diag') renderHlDiag();
  toast(txt);
}

function scanUi(){
  if (!SCAN) return;
  const now = $('#scNow');
  if (now) now.textContent = '正在试「' + SCAN.nm + '」';
  const bar = $('#scBar');
  if (bar) bar.style.width = Math.round(SCAN.k * 100 / Math.max(1, SCAN.ids.length)) + '%';
  const num = $('#scNum');
  if (num) num.textContent = SCAN.k + ' / ' + SCAN.ids.length;
  const tl = $('#scTally');
  if (tl) tl.textContent = SCAN.ok + ' 正常 · ' + (SCAN.bad + SCAN.dim) + ' 有问题';
}

/* ---------------- 体检页那张卡 ---------------- */
function renderHlDiag(){
  const h = S.hl;
  if (!h || !h.c) return;
  const c = h.c;
  $('#hlOk').textContent = c.ok | 0;
  $('#hlBad').textContent = c.bad | 0;
  $('#hlDim').textContent = c.dim | 0;
  $('#hlUnk').textContent = c.unk | 0;
  const bad = hlCountBad();
  $('#hlSum').textContent = bad ? bad + ' 个要留意' : '都没问题';
  $('#hlSum').style.color = bad ? 'var(--bad)' : '';
  const sub = $('#hlScanSub');
  if (sub) sub.textContent = (c.unk | 0) ? '还有 ' + (c.unk | 0) + ' 个台没试过' : '全表都试过了';
}

/* ---------------- 事件 ---------------- */
function bindHealth(){
  const b1 = $('#btnHlList');
  if (b1) b1.onclick = () => {
    S.tag = -2;
    go('stations');
    setTimeout(() => { renderStChips(); renderStList(); }, 30);
  };
  const b2 = $('#btnHlScan');
  if (b2) b2.onclick = openScanSheet;
  _hlTimer = setInterval(() => {
    if (document.hidden || SCAN) return;
    pollHealth();
  }, 900);

  pollHealth(true);
}

INIT.push(bindHealth);
