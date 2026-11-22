/* ============================================================
   pages.js —— 历史 / 体检 / 设置（含 WiFi 配网、配对、恢复出厂）
   ============================================================ */
'use strict';

/* ============================================================
   一、收听记录
   ============================================================ */
async function loadHistory(){
  const box = $('#histList');
  if (!S.hist) box.innerHTML = '<div class="empty">' + ic('clock') + '正在读记录…</div>';
  try{
    S.hist = await get('/api/history');
    renderHistory();
  }catch (e){
    box.innerHTML = '<div class="empty">' + ic('cloud-off') + '读不到记录</div>';
  }
}

function renderHistory(){
  const h = S.hist;
  if (!h) return;

  // 近 7 天（接口给的是「新→旧」，不足 7 天就补空）
  const days = (h.days || []).slice(0, 7);
  while (days.length < 7) days.push({ d: '', s: 0, p: 0 });
  const week = days.reduce((a, b) => a + (b.s || 0), 0);

  $('#stats').innerHTML =
    '<div class="stat hero"><div class="k">今天</div><div class="v">' + fmtShort(h.today) + '</div></div>' +
    '<div class="stat"><div class="k">近 7 天</div><div class="v">' + fmtShort(week) + '</div></div>' +
    '<div class="stat"><div class="k">一共</div><div class="v">' + fmtShort(h.total) + '</div></div>';

  $('#weekSum').textContent = days.filter(x => x.s > 0).length + ' 天有收听';

  const max = Math.max(60, ...days.map(x => x.s || 0));
  const WD = ['日','一','二','三','四','五','六'];
  $('#bars').innerHTML = days.map((d, k) => {
    const pct = Math.max(4, Math.round((d.s || 0) / max * 100));
    let lb = '—';
    if (d.d){
      const dt = new Date(d.d + 'T00:00:00');
      lb = k === 0 ? '今天' : '周' + WD[dt.getDay()];
    }
    return '<div class="b' + (k === 0 ? ' now' : '') + '" style="animation-delay:' + (k * 40) + 'ms">' +
      '<div class="bar' + (k === 0 ? ' now' : '') + '"><i style="height:' + pct + '%"></i></div>' +
      '<div class="lb">' + lb + '</div></div>';
  }).join('');

  const list = h.list || [];
  $('#histHint').textContent = list.length ? '一共听过 ' + list.length + ' 个台' : '机器自动记账';

  if (!list.length){
    $('#histList').innerHTML = '<div class="empty">' + ic('history') +
      '还没有记录<br>听一会儿，这里就会自己长出来</div>';
    return;
  }

  $('#histList').innerHTML = list.map((it, k) =>
    '<div class="row" data-k="' + k + '" style="--i:' + Math.min(k, 12) + '">' +
      '<span class="ix">' + (k + 1) + '</span>' +
      '<div class="nm"><div class="t">' + esc(it.n) + '</div>' +
        '<div class="m"><span class="tag gray">' + esc(it.g || '未打标签') + '</span>' +
          '<span>' + fmtRel(it.last) + '</span></div></div>' +
      '<div class="rt"><b>' + fmtDur(it.s) + '</b>' +
        '<span>听过 ' + it.p + ' 次</span></div>' +
    '</div>'
  ).join('');

  $$('#histList .row').forEach(r => {
    r.onclick = () => replayHist(list[+r.dataset.k]);
  });
}

// 再听一次：整张表就在本机，直接按台名找（1.15.0 起不用先切分类）
async function replayHist(it){
  if (!it) return;
  toast('正在找「' + it.n + '」…');
  try{
    if (!S.list || !S.list.length){ await loadStations(); }
    const idx = S.list.findIndex(x => x.n === it.n);
    if (idx < 0){ toast('这个台已经被删了', true); return; }
    await post('/api/play', { id: idx });
    toast('正在播 ' + it.n);
    pollScreen();
  }catch (e){ toast('没切过去', true); }
}

/* ============================================================
   二、体检
   ============================================================ */
async function loadDiag(){
  try{
    S.diag = await get('/api/diag');
    renderDiag();
    renderHlDiag();          // 1.17.0：台的健康（进来就先画一次，别等下一轮轮询）
    pollHealth(true);
    const badge = $('#diagBadge');
    const score = healthScore(S.diag).score;
    badge.classList.toggle('hide', score >= 80);
  }catch (e){
    $('#chkList').innerHTML = '<div class="empty">' + ic('cloud-off') + '读不到机器状态</div>';
  }
}

function healthScore(d){
  const items = [];
  const push = (name, icon, ok, val, sub) => items.push({ name, icon, ok, val, sub });

  const wifiOk = d.wifi && d.wifi.ok;
  const rssi = d.wifi ? d.wifi.rssi : -127;
  const ps = d.audio ? d.audio.ps : 0;
  const heap = d.heap || 0;
  const fsPct = d.fsTotal ? Math.round(d.fsUsed / d.fsTotal * 100) : 0;

  // 1 网络
  push('网络连接', 'wifi', wifiOk, wifiOk ? '已连' : '断开',
       wifiOk ? (d.wifi.ssid || '') + ' · 信道 ' + d.wifi.ch : '机器没连上路由器');
  // 2 信号
  const sigOk = rssi > -78;
  push('信号强度', 'signal', wifiOk && sigOk, rssi > -127 ? rssi + ' dBm' : '—',
       !wifiOk ? '连上网才能测' : rssi > -55 ? '很强' : rssi > -67 ? '够用' : rssi > -78 ? '偏弱，离路由器远' : '太弱了，容易断');
  // 3 播放
  const playOk = ps === 2;
  push('播放状态', ps === 2 ? 'play' : 'pause', playOk,
       ps === 2 ? '正在播' : ps === 1 ? '连接中' : ps === 3 ? '缓冲中' : ps === 4 ? '出错' : '停着',
       ps === 2 ? (d.audio.kbps ? d.audio.kbps + ' kbps' : '') : '点「重开这一个台」试试');
  // 4 存储
  const fsOk = fsPct < 88;
  push('存储空间', 'hard-drive', fsOk, fsPct + '%',
       Math.round((d.fsTotal - d.fsUsed) / 1024) + ' KB 可用');
  // 5 内存
  const heapOk = heap > 45000;
  push('运行内存', 'cpu', heapOk, Math.round(heap / 1024) + ' KB',
       heap > 90000 ? '富余' : heapOk ? '够用' : '偏少，重启一下更稳');
  // 6 时钟
  const timeOk = d.time && d.time.ok;
  push('时间同步', 'clock', timeOk, timeOk ? '正常' : '未同步',
       timeOk ? '屏上时间准时' : '连上网会自动校准');
  // 7 台表
  const tabOk = (d.table.total || 0) > 0 && (d.table.count || 0) > 0;
  push('台表', 'list-music', tabOk, (d.table.total || 0) + ' 台',
       (d.table.tags || 0) + ' 个标签，共 ' + (d.table.count || 0) + ' 台');
  // 8 上次关机
  const rst = d.reset || '';
  const rstOk = rst === '上电' || rst === '软件重启' || rst === '深睡唤醒';
  push('上次关机', 'power', rstOk, rst || '—',
       rstOk ? '正常' : '上次是非正常退出，留意一下');

  let score = 100;
  items.forEach(it => { if (!it.ok) score -= (it.name === '网络连接' || it.name === '播放状态') ? 18 : 10; });
  score = Math.max(5, Math.min(100, score));
  return { items, score };
}

function renderDiag(){
  const d = S.diag;
  if (!d) return;
  const { items, score } = healthScore(d);
  const bad = items.filter(x => !x.ok);

  // 环
  const C = 2 * Math.PI * 38;
  const fg = $('#ringFg');
  fg.setAttribute('stroke-dasharray', C.toFixed(1));
  fg.setAttribute('stroke-dashoffset', (C * (1 - score / 100)).toFixed(1));
  fg.setAttribute('stroke', score >= 80 ? '#4a8a63' : score >= 60 ? '#bb7546' : '#ab5147');
  $('#ringVal').innerHTML = score + '<small>分</small>';

  $('#healthTitle').textContent =
    score >= 90 ? '一切正常' : score >= 75 ? '基本没问题' : score >= 55 ? '有点小毛病' : '需要处理一下';
  $('#healthDesc').textContent =
    bad.length === 0 ? '网络、声音、存储都正常，放心用。'
    : bad.length === 1 ? '发现 1 个问题：' + bad[0].name + '。' + bad[0].sub
    : '发现 ' + bad.length + ' 个问题，点下面「一键修复」试试。';

  // 顶部提示
  const alertBox = $('#diagAlert');
  if (!bad.length){
    alertBox.innerHTML = '';
  } else {
    const worst = bad[0];
    const hard = !bad.every(x => ['播放状态', '信号强度'].indexOf(x.name) >= 0);
    alertBox.innerHTML =
      '<div class="alertcard' + (hard ? '' : ' warn') + '">' +
        ic(hard ? 'triangle-alert' : 'circle-alert') +
        '<div><div class="t">' + esc(worst.name) + (bad.length > 1 ? ' 等 ' + bad.length + ' 项' : '') + '</div>' +
        '<div class="m">' + esc(worst.sub) + '</div></div>' +
      '</div>';
  }

  // 检查项
  $('#chkList').innerHTML = items.map(it =>
    '<div class="chk ' + (it.ok ? 'ok' : 'warn') + '">' +
      '<span class="ci">' + ic(it.icon, 'ic sm') + '</span>' +
      '<span class="ct"><span class="t">' + it.name + '</span>' +
        '<span class="m">' + esc(it.sub || '') + '</span></span>' +
      '<span class="cv">' + esc(it.val) + '</span>' +
    '</div>'
  ).join('');

  $('#fixAllTx').textContent = bad.length ? '一键修复这 ' + bad.length + ' 个问题' : '全部正常，再检查一遍';
}

// 一键修复：按问题类型排出动作顺序，逐个执行
async function fixAll(){
  const d = S.diag;
  if (!d){ toast('还没读到状态', true); return; }
  const { items } = healthScore(d);
  const bad = items.map(x => x.name).filter(n => items.find(y => y.name === n && !y.ok));
  if (!items.some(x => !x.ok)){ await loadDiag(); toast('都正常，不用修'); return; }

  const steps = [];
  if (items.find(x => x.name === '网络连接' && !x.ok)) steps.push('reconnect');
  if (items.find(x => x.name === '播放状态' && !x.ok)) steps.push('replay');
  if (items.find(x => x.name === '台表' && !x.ok)) steps.push('fix_table');
  if (!steps.length) steps.push('replay');

  $('#fixAllTx').textContent = '正在修…';
  try{
    for (const s of steps){
      const j = await post('/api/fix', { action: s });
      toast(j.msg || '已处理');
      await new Promise(r => setTimeout(r, 900));
    }
    await loadDiag();
    await pollScreen();
  }catch (e){ toast('修复请求没成功', true); }
  $('#fixAllTx').textContent = '一键修复';
}

async function runFix(action){
  const needConfirm = ['factory', 'reset_stations', 'forget_wifi'].indexOf(action) >= 0;
  const run = async (confirm) => {
    try{
      const j = await postOk('/api/fix', { action, confirm: confirm ? 1 : 0 });
      toast(j.msg || '已处理');
      if (j.reboot) rebootWait();
      else { setTimeout(loadDiag, 800); setTimeout(pollScreen, 900); }
    }catch (e){ toast('没成功', true); }
  };
  if (needConfirm){
    confirmSheet({
      title: '确定要这样吗？',
      desc: '这一步做完就回不去了。', ok: '确定', danger: true,
      onOk: () => run(true)
    });
    return;
  }
  run(false);
}

function rebootWait(){
  const b = $('#boot');
  b.style.display = '';
  b.classList.remove('gone');
  b.innerHTML =
    '<div class="logo">' + ic('radio') + '</div>' +
    '<div class="msg">正在重启收音机…</div>' +
    '<div class="bar2"><i></i></div>' +
    '<div style="font-size:12.5px;color:var(--ink3);max-width:250px;text-align:center;line-height:1.7">' +
      '大概 15 秒，页面会自己回来</div>';
  let n = 0;
  const t = setInterval(async () => {
    n++;
    try{
      const r = await fetch('/api/info', { cache: 'no-store' });
      if (r.ok){ clearInterval(t); location.reload(); }
    }catch (e){}
    if (n > 40){ clearInterval(t); location.reload(); }
  }, 1500);
}

// 1.46.35：点「重新连接网络」时，机器会**先回包、再断链**（700ms）。
//   但断链那一瞬间 TCP 仍可能正好被掐，App 这边偶尔还是会收到 fetch 失败。
//   那不是「没成功」—— 机器确实在重连。所以这里分两种：
//     · 带回业务错误码（err / ok:0）→ 真失败，直接说；
//     · 纯网络错（没有 code）      → 当已发出，等它自己回来。
async function doReconnect(){
  toast('正在让机器重连 WiFi…');
  try{
    const j = await postOk('/api/fix', { action: 'reconnect' });
    if (j && j.msg) toast(j.msg);
  }catch (e){
    if (e && e.code){                 // 机器明确回了个错误
      toast('没成功（' + e.code + '）', true);
      return;
    }
    // 没 code = 请求发出去之后链路断了，这正是预期
  }
  waitNetBack();
}

// 等机器把网络接回来（最多约 24 秒），回来就刷新设置页
function waitNetBack(){
  const b = $('#boot');
  b.style.display = '';
  b.classList.remove('gone');
  b.innerHTML =
    '<div class="logo">' + ic('wifi') + '</div>' +
    '<div class="msg">正在重新连 WiFi…</div>' +
    '<div class="bar2"><i></i></div>' +
    '<div style="font-size:12.5px;color:var(--ink3);max-width:250px;text-align:center;line-height:1.7">' +
      '大概 10 秒。连上后这页会自己回来</div>';
  let n = 0;
  const t = setInterval(async () => {
    n++;
    try{
      const r = await fetch('/api/info', { cache: 'no-store' });
      if (r.ok){
        clearInterval(t);
        b.classList.add('gone');
        toast('网络已经重新连上');
        loadSettings();
        pollScreen();
        return;
      }
    }catch (e){}
    if (n > 16){                      // 16 × 1.5s ≈ 24s
      clearInterval(t);
      b.innerHTML =
        '<div class="logo">' + ic('wifi-off') + '</div>' +
        '<div class="msg">还是没连上</div>' +
        '<div style="font-size:12.5px;color:var(--ink3);max-width:270px;text-align:center;line-height:1.75">' +
          '机器可能还在找网络，或者这个 WiFi 现在连不上。<br>' +
          '过一会儿点「换一个 WiFi」重新配一次。</div>' +
        '<button class="btn" id="btnNetRetry" style="margin-top:14px">重试一次</button>' +
        '<button class="btn ghost" id="btnNetBack" style="margin-top:8px">先回去</button>';
      const r1 = $('#btnNetRetry'); if (r1) r1.onclick = () => { b.classList.add('gone'); doReconnect(); };
      const r2 = $('#btnNetBack');  if (r2) r2.onclick = () => { b.classList.add('gone'); location.reload(); };
    }
  }, 1500);
}

/* ============================================================
   三、设置
   ============================================================ */
/* ---------------- 1.19.0 电量 ---------------- */
// /api/state 里带的字段：bok(有没有电池) / batt(%) / bmv(毫伏) / chg / blow
let _lowTold = false;
function renderBatt(st){
  const row = $('#battRow');
  if (!row) return;
  const ok = !!(st && st.bok);
  row.style.display = ok ? '' : 'none';        // 没接电量检测就整行不出现
  if (!ok) return;
  const pct = +st.batt, mv = +st.bmv || 0;
  $('#battNow').textContent = (pct >= 0 ? pct + '%' : '—');
  // 1.38.2：把校准系数也显示出来，用户一眼能看出"机器自己调过"。
  //   系数 = 1.0000 说明从没校准过，读数只靠分压比推 —— 这时候最容易不准。
  const cal = +(st.bcal || 1);
  // 1.46.51：机器顶栏「满电 / 插着电」是有显示的（batteryFull / batteryOnExt），
  //   App 以前只认 chg（电压上行），充满后就一片空白 —— 用户看着像「没同步」。
  // 1.46.55：bfull 是「电压到 4.18 且稳住」，充电时电压被顶高，插上 20 分钟
  //   就会为真 —— 但电量可能才 50%。所以「已充满」还必须电量接近满，
  //   否则会看到「已充满 · 57%」这种自相矛盾的组合。
  const chgTx = st.chg ? ' · 正在充电'
              : ((st.bfull && pct >= 95) ? ' · 已充满'
              : (st.bext ? ' · 接着电源'
              : (st.blow ? ' · 快没电了，插上充电器' : '')));
  $('#battSub').textContent = (mv / 1000).toFixed(2) + ' V' + chgTx +
    (cal.toFixed(4) !== '1.0000' ? ' · 已校准 ×' + cal.toFixed(3) : '');
  const calBtn = $('#btnBattCal');
  if (calBtn) calBtn.style.display = '';
  if (st.blow && !_lowTold){ _lowTold = true; toast('电量低，请充电'); }
}

/* ---------------- 1.38.2 电量校准 ----------------
   为什么需要用户自己填：读数 = 引脚电压 × 分压比(1.71) × 校准系数。
   1.71 是推出来的，不是量出来的 —— 差 3% 就是 100mV，在锂电平台区那是十几到三十个百分点。
   唯一能"和真实一致"的办法：拿万用表量电池两端，把真实值填进来，机器自己缩放系数。

   这里不要求用户输入"4.02"这种精确值，给他一个滑条 + 几个常用档位，
   因为万用表读数本身就有 ±0.01V 的误差，填到 10mV 精度已经够了。 */
async function openBattCal(){
  // 注意：/api/screen 里**没有**电量字段，电量只在 /api/state 里（平铺的 bok/batt/bmv）。
  // 这里现取一次，别图省事从 S.scr 读 —— 那样会拿到 undefined，弹层里全是 0.00。
  let st = {};
  try{ st = await get('/api/state'); }catch (e){}
  const cur = +st.bmv || 0;                    // 机器现在认为的电压
  const raw = +st.braw || 0;                   // 引脚原始读数（不含补偿）
  const cal = +(st.bcal || 1);
  openSheet(
    '<h3>校准电量</h3>' +
    '<p class="desc">机器的电量是按电压算的，天生会有偏差。<br>' +
    '拿万用表量一下电池两端（或者看充电器上的电压显示），填进去就行。</p>' +
    '<div class="field"><label>机器现在认为</label>' +
      '<div style="font-size:22px;font-weight:800">' + (cur / 1000).toFixed(2) + ' V</div>' +
      '<div style="font-size:12px;color:var(--ink3);margin-top:2px">' +
        '引脚原始 ' + raw + ' mV · 校准系数 ×' + cal.toFixed(3) + '</div></div>' +
    '<div class="field"><label>万用表量到的是多少伏</label>' +
      '<input type="number" id="bcMv" step="0.01" min="3" max="4.4" ' +
        'value="' + (cur / 1000).toFixed(2) + '" placeholder="4.02"></div>' +
    '<div class="acts"><button class="btn ghost" id="bcNo">取消</button>' +
      '<button class="btn pri" id="bcYes">按这个校准</button></div>',
    sheet => {
      $('#bcNo', sheet).onclick = closeSheet;
      $('#bcYes', sheet).onclick = async () => {
        const v = parseFloat($('#bcMv', sheet).value);
        if (!(v >= 2.5 && v <= 4.5)) { toast('填 2.5 ~ 4.5 之间的数', true); return; }
        try{
          await post('/api/battcal', { mv: Math.round(v * 1000) });
          toast('已校准到 ' + v.toFixed(2) + ' V');
          closeSheet();
          setTimeout(loadSettings, 400);
        }catch (e){ toast('没发出去，重试一下', true); }
      };
    }
  );
}

/* ---------------- 1.19.0 我的机器 ---------------- */
// 数据来自 MQTT 的设备广播（CLOUD.S.devs）。云没连上时列表是空的，
// 但已经在控的那台仍然显示出来 —— 不然用户会以为"机器丢了"。
function renderDevs(list){
  const box = $('#devList');
  if (!box) return;
  const hint = $('#devHint');
  const arr = (list || CLOUD.S.devs || []).slice().sort((a, b) => {
    const ca = a.dev === CLOUD.S.dev ? 0 : 1, cb = b.dev === CLOUD.S.dev ? 0 : 1;
    if (ca !== cb) return ca - cb;
    return (b.seen || 0) - (a.seen || 0);
  });
  if (!arr.length){
    if (hint) hint.textContent = CLOUD.S.phase === 'online' ? '没找到' : '正在找…';
    box.innerHTML = '<div class="empty" style="padding:14px 0 6px">' + ic('radio') +
      '还没看到机器<br><span style="font-size:12px;color:var(--ink3)">机器连上家里 WiFi 后会自动出现在这里</span></div>';
    return;
  }
  if (hint) hint.textContent = arr.length + ' 台';
  box.innerHTML = arr.map(d => {
    const cur = d.dev === CLOUD.S.dev;
    const alive = CLOUD.devAlive(d);
    // 1.46.7：把机器报上来的 4 位码也显示出来 —— 否则两台都叫 1234 时，
    //   用户看着两个一模一样的名字不知道该点哪个。
    return '<button class="devrow' + (cur ? ' cur' : '') + '" data-d="' + esc(d.dev) + '">' +
      '<span class="dot ' + (alive ? 'ok' : '') + '"></span>' +
      '<span class="nm">' + esc(d.n || ('MyRadio-' + d.dev)) + '</span>' +
      (d.pin ? '<span class="pn">' + esc(d.pin) + '</span>' : '') +
      '<span class="mt">' + (d.batt >= 0 ? d.batt + '%' : '') +
        (cur ? ' · 正在控' : (alive ? '' : ' · 离线')) + '</span>' +
      '</button>';
  }).join('');
  $$('.devrow', box).forEach(b => b.onclick = () => useDev(b.dataset.d));
}

/* 1.46.7 修：设备广播到了要自己刷「我的机器」。
   原来 cloud.js 的注释写着「onDevs: pages.js 挂的」，可 pages.js 从来没挂过 ——
   于是这份列表只在点「重新找一遍」时才动：打开设置页看到的是「还没看到机器」，
   等 30 秒机器报到了也不出现，用户会以为机器丢了（1.19.0 起就有的老问题）。
   做三件事：300ms 合并（一次报到会连着来两台）、内容没变就不动 DOM
   （每次重建 innerHTML 会让正在点的按不到）、新机器一到立刻出现。 */
let devPaintT = 0, devSig = '\u0000';
function devSignature(list){
  return (list || []).map(d =>
    d.dev + ':' + (d.pin || '') + ':' + (d.batt >= 0 ? d.batt : '-') + ':' +
    (CLOUD.devAlive(d) ? 1 : 0)).join('|');
}
CLOUD.S.onDevs = function(list){
  clearTimeout(devPaintT);
  devPaintT = setTimeout(function(){
    const sig = devSignature(list);
    if (sig === devSig) return;
    devSig = sig;
    renderDevs(list);
  }, 300);
};

// 选一台机器来控：免配对的直接算码连上；关了免配对的要它那串遥控码
// 连中转服务器（预览器里不连 —— 那会真的往公网发 WebSocket）
function cloudStart(){
  if (typeof window !== 'undefined' && window.MR_PREVIEW) return;
  CLOUD.enable(true);
}

function useDev(dev){
  const ok = CLOUD.pickDev(dev);
  cloudStart();
  if (!ok){
    toast('这台关了免配对，需要它的遥控码', true);
    // 1.39.0：「手动输入遥控码」已删，这里不再自动弹它
    return;
  }
  toast('已切到 ' + (CLOUD.S.name || dev));
  setTimeout(() => loadSettings().then(() => pollScreen()), 500);
}

/* ---------------- 1.46.7 连接码：显示 + 修改 ----------------
   固件侧 /api/pair?op=pin&code=5678 在 1.46.5 就有了（改完还会在机器屏上显示一次，
   省得用户忘记），只是一直没有界面。这里补上，因为说明书和界面都在说「输 1234」——
   不能改的话，家里两台都叫 1234 就永远分不开。 */
function openPinEdit(){
  // ⚠️ 这里**不能**先 await /api/pair：跨网那条通道一次请求最长 9 秒，
  //   点了没反应、9 秒后才弹层，用户会以为按钮坏了。
  //   loadSettings 早就把 S.pair 读回来了，直接用；万一没有就先弹（显示 —），
  //   再在后台补一次，补到了顺手填进输入框。
  const cur = (S.pair && S.pair.pin) ? S.pair.pin : '';
  if (!cur) get('/api/pair').then(p => {
    if (!p || !p.pin) return;
    S.pair = p;
    const sv = $('#svPin'); if (sv) sv.textContent = p.pin;
    const el = document.getElementById('peIn');
    if (el && !el.value) el.value = p.pin;
  }).catch(() => {});
  openSheet(
    '<h3>连接码</h3>' +
    '<p class="desc">手机输这 4 位数字就能找到这台机器 —— 同一个 WiFi 里是这样，' +
    '人在外地用远程版也是输它。<br>' +
    '<b>出厂是 1234。</b>家里有两台机器时，把其中一台改成别的，就不会分不清了。</p>' +
    '<div class="field"><label>新的一串（4 个数字）</label>' +
      '<input id="peIn" inputmode="numeric" maxlength="4" autocomplete="off" ' +
        'value="' + esc(cur) + '" placeholder="1234" ' +
        'style="font-size:24px;letter-spacing:8px;text-align:center;font-weight:800"></div>' +
    '<p class="desc" style="margin-top:6px">改完机器屏幕上会显示一次，方便记下来。' +
    '已经在用的手机会自动跟着换，不用重新连。</p>' +
    '<div class="acts"><button class="btn ghost" id="peNo">取消</button>' +
      '<button class="btn pri" id="peYes">改成这个</button></div>',
    sheet => {
      const inp = $('#peIn', sheet);
      inp.oninput = () => {
        const v = inp.value.replace(/\D/g, '').slice(0, 4);
        if (v !== inp.value) inp.value = v;
      };
      $('#peNo', sheet).onclick = closeSheet;
      $('#peYes', sheet).onclick = async () => {
        const v = String(inp.value || '').replace(/\D/g, '');
        if (v.length !== 4) { toast('要 4 个数字，比如 5678', true); return; }
        try{
          const r = await post('/api/pair', { op: 'pin', code: v });
          if (!r || !r.ok) { toast('没改成，重试一下', true); return; }
          if (S.pair) S.pair.pin = r.pin || v;
          $('#svPin').textContent = r.pin || v;
          closeSheet();
          toast('连接码已改成 ' + (r.pin || v));
          setTimeout(loadSettings, 300);
        }catch (e){ toast('没改成，重试一下', true); }
      };
    }
  );
}

/* ---------------- 1.46.7 跨网输 4 位码 ----------------
   机器每 30 秒往 myradio/dev/<短码> 广播一次，payload 里带了自己的 4 位码。
   所以这里「查」就是本地筛（见 cloud.js 的 pinLogin）：
     唯一命中 → 直接切过去（和点列表一样）
     多台同码 → 只把这几台留下，让用户在列表里点
     零台     → 说清是「没上线」还是「固件太老不报码」 */
function devPinGo(){
  const el = $('#devPin');
  if (!el) return;
  const v = String(el.value || '').replace(/\D/g, '').slice(0, 4);
  if (v.length !== 4) { toast('要 4 个数字，比如 1234', true); return; }
  const r = CLOUD.pinLogin(v);
  if (r.n === 1){
    if (r.needCode){
      toast('这台的码对上了，但它关了免配对 —— 请用它的 12 位遥控码', true);
      return;
    }
    CLOUD.enable(true);
    toast('已连到 ' + (r.name || r.dev));
    setTimeout(() => { loadSettings().then(() => pollScreen()); }, 400);
    return;
  }
  if (r.n > 1){
    renderDevs(r.list);                       // 只把同码的这几台留下
    const h = $('#devHint'); if (h) h.textContent = r.n + ' 台同码，点一台';
    toast('有 ' + r.n + ' 台都用 ' + v + '，点一台');
    return;
  }
  toast(CLOUD.anyPin()
    ? '没找到用 ' + v + ' 的机器，确认码没打错'
    : '没找到：这些机器固件太老，不报连接码', true);
}

function initDevPin(){
  const el = $('#devPin'), go = $('#devPinGo');
  if (!el) return;
  go.onclick = devPinGo;
  el.addEventListener('keydown', e => { if (e.key === 'Enter') devPinGo(); });
  el.addEventListener('input', () => {
    const v = el.value.replace(/\D/g, '').slice(0, 4);
    if (v !== el.value) el.value = v;
    if (v.length === 4) setTimeout(devPinGo, 120);
  });
}

async function loadSettings(){
  try{
    const [d, p, st] = await Promise.all([get('/api/diag'), get('/api/pair'), get('/api/state')]);
    S.diag = d; S.pair = p;
    // 1.46.7：把当前 4 位连接码显示出来（改码弹层和这一行共用同一个值）
    const pv = $('#svPin'); if (pv) pv.textContent = p.pin || '—';
    $('#svFw').textContent = 'v' + (d.fw || '—');
    $('#svIp').textContent = (d.wifi && d.wifi.ip) || '—';
    $('#svUp').textContent = fmtUp(d.up);
    $('#setHint').textContent = (d.wifi && d.wifi.ok ? d.wifi.ssid : '未连网');

    const wf = d.wifi || {};
    const off = !wf.ok;
    $('#wifiNow').textContent = wf.ok ? (wf.rssi + ' dBm') : '未连接';
    $('#wifiSub').textContent = wf.ok
      ? ('当前：' + (wf.ssid || ''))
      : '收音机还没上网，点这里 3 步连上';
    const wt = $('#wifiTitle');
    if (wt) wt.textContent = wf.ok ? 'WiFi' : '给收音机配网';
    const wc = $('#wifiCard');
    if (wc) wc.classList.toggle('needset', off);

    // 1.39.0：配对码这套 UI 已经删掉（app 只绑这一台、不需要码）。
    //   这里保留成「有元素才更新」的写法 —— 万一以后又加回来，也不会因为
    //   取到 null 而抛 TypeError 把整个设置页打空。

    renderBatt(st);                  // 1.19.0：电量（没接检测就自动隐藏）

    // 屏幕与声音：亮度上限 / 两路音量 / 熄屏延时（值来自收音机，改了会真的生效）
    // 1.44.0：两路量程**不一样了**（原厂口径：网络 netvolume 6..21、收音 volume 6..63），
    //   所以两条滑条各用各的 min/max。之前两条共用 volmin/volmax 是因为
    //   那时两边都是 3..10 档，现在共用会把 FM 那条卡在 21。
    setVolRange($('#rgVol'),   st.volmin,  st.volmax);
    setVolRange($('#rgFmVol'), st.fvolmin, st.fvolmax);
    setRange($('#rgVol'),   st.vol   != null ? st.vol   : 18);
    setRange($('#rgFmVol'), st.fmvol != null ? st.fmvol : 35);
    setRange($('#rgBr'),    st.bmax  != null ? st.bmax  : 100);
    $$('#segOff button').forEach(b =>
      b.classList.toggle('on', +b.dataset.v === +(st.offmin != null ? st.offmin : 3)));

    // ---- 1.41.0 收音机那一路（SI4732）：波段 / 静噪 / 半步进 / 搜台 ----
    renderFm(st);

    // 已存 WiFi 的组数直接写在行上，不用点进去才知道
    try{
      const wl = await get('/api/wifi/list');
      S.wifiJ = wl;
      const n = (wl && wl.list) ? wl.list.length : 0;
      const max = wifiSlotCount(wl);
      const nEl = $('#wifiNow');
      if (nEl) nEl.textContent = n ? (n + ' / ' + max + ' 组') : '—';
      const sEl = $('#wifiSub');
      if (sEl) sEl.textContent = n
        ? ('已存 ' + wl.list[0].ssid + (n > 1 ? ' 等 ' + n + ' 组' : '') + ' · 点这里管理')
        : ('机器一个 WiFi 都没记 · 点这里填一个');
      // 老页面上的「已存的 WiFi」那一行（新版已合并，取不到就算了）
      const n0 = $('#savedN');
      if (n0) n0.textContent = n + ' 组';
      const s0 = $('#savedSub');
      if (s0) s0.textContent = n ? ('第 1 组：' + wl.list[0].ssid + '（开机先连它）')
                                 : '机器一个 WiFi 都没记';
    }catch (e){}
  }catch (e){}

  // ---- 1.8.0 远程控制 ----
  try{
    const c = await get('/api/cloud');
    S.cloud = c;
    // 1.42.0：固件恒开，这里只读状态。on 字段老固件可能有 0，一律显示「已开启」
    $('#svRHost').textContent = (c.host || '—') + (c.port ? ':' + c.port : '');
    $('#svRSt').textContent = c.conn ? '已连上' : (c.on === 0 ? '连不上服务器' : '连接中…');
    // 顺手把遥控码记到本机：以后在外地打开远程版，不用再抄一遍
    if (c.short && c.code) CLOUD.save({ dev: c.short, code: c.code, host: c.host || CLOUD.S.host });
    // 1.19.0：免配对开关（开着才广播、才认算出来的码）
    // 1.39.0：「免配对」开关已删（app 默认就绑这一台，不需要开关）
  }catch (e){}
  // ⚠️ 1.41.0 修 P0：上面这一行 `}catch (e){}` 和下面的 `}` 原来是**丢的**。
  //   1.39.0 删「免配对」那一块时把 loadSettings 的收尾一起删掉了，
  //   于是 `try{` 一直开着，后面的 setRange / setLock / openWifiSheet…
  //   全被吞进 loadSettings 体内，文件到最后少两个 `}` ⇒ 整份 pages.js 语法错误
  //   ⇒ App 所有按钮失灵。改这里的任何东西，改完必须
  //   `node --check tools/app/js/pages.js`。
}

// 收音机那一路的状态。state 里 sq/hs/fmstn 是 1.41.0 新加的只读字段；
// 老固件没有就从 localStorage 里取上次设过的值，界面上不假装知道真实值。
function renderFm(st){
  const on = st.mode === 'fm';
  const bandWord = { fm: '调频', am: '中波', lw: '长波', sw: '短波' }[st.band] || '';
  // 单位同上：fmfreq = kHz/10，换 MHz 要 /100
  const freq = on ? (st.fmfreq ? (st.fmfreq / 100).toFixed(1) + ' MHz'
                               : (st.khz ? st.khz + ' kHz' : '—')) : '—';

  const now = $('#fmNow');
  if (now) now.textContent = on ? '正在收音' : '网络模式';
  const fq = $('#fmFreq');
  if (fq) fq.textContent = freq;
  const fs = $('#fmBandSub');
  if (fs) fs.textContent = on
    ? (bandWord + ' · 信号 ' + (st.fmrssi != null ? st.fmrssi : '—'))
    : '现在是网络电台；没网时机器会自动切到收音';

  // 静噪：0 = 关
  const sq = st.sq != null ? st.sq : (+(localStorage.getItem('mr.sq') || 0));
  setRange($('#rgSq'), sq);
  const sqNow = $('#sqNow');
  if (sqNow) sqNow.textContent = sq > 0 ? ('门限 ' + sq) : '关';

  // 半步进
  const hs = st.hs != null ? (!!st.hs) : (localStorage.getItem('mr.hs') === '1');
  const swHalf = $('#swHalf');
  if (swHalf) swHalf.classList.toggle('on', hs);

  // 搜台
  const busy = !!st.fmscan;
  const cnt = st.fmstn != null ? st.fmstn : null;
  const cEl = $('#fmCount');
  if (cEl) cEl.textContent = busy ? ('搜到 ' + (cnt == null ? 0 : cnt) + ' 个') : (cnt == null ? '—' : cnt + ' 个台');
  const sub = $('#fmScanSub');
  if (sub){
    if (busy){
      const p = st.fmprog != null ? st.fmprog : 0;
      sub.textContent = '正在搜 ' + (p > 100 ? 100 : p) + '%…';
    } else {
      sub.textContent = '收音机自己扫一遍，十几秒';
    }
  }
  const btn = $('#btnFmScan');
  if (btn) btn.disabled = busy;
}

// 滑条：设值同时更新填充比例（--fill 给 track 的渐变用）
function setRange(el, v){
  if (!el) return;
  el.value = v;
  el.style.setProperty('--fill', ((v - el.min) / (el.max - el.min) * 100) + '%');
}

// 1.39.0：配对码保护这套 UI 已删（app 只绑这一台、不需要码）。
// 函数留着是因为固件侧还有 /api/pair 接口，万一以后要恢复设置项可以直接接回来。
// 注意：这里**不再碰任何 DOM** —— 元素已经不在了，写 $('#swLock') 会拿到 null。
//
// ⚠️ 1.41.0 修 P0：原来这里写的是 `async` + 换行 + 三行注释 + `function setLock(on)`。
//    注释把 `async` 和 `function` 拆开了 ⇒ `async` 成了一个单独的表达式语句，
//    `setLock` 变成**普通函数**，里面的 `await` 直接 SyntaxError。
//    pages.js 整份解析失败 ⇒ App 全部按钮失灵（不只是这个设置项）。
//    教训：`async function` 中间绝不能断开，注释要写在前面。
async function setLock(on){
  try{
    const j = await post('/api/pair', { op: 'lock', on: on ? 1 : 0 });
    toast(on ? '已开启配对码保护' : '已关闭配对码保护');
    if (on && j.code) pollScreen();      // 让屏上显示码
  }catch (e){ toast('设置没成功', true); }
}

// 1.41.0：把搜到的台列出来（原厂就是这里能看见「搜到哪几个」）。
//   点一行 = 直接调过去（/api/fmtune），不用靠上一台/下一台一个个试。
function openFmStationsSheet(){
  openSheet(
    '<h3>搜到的台</h3>' +
    '<p class="desc">收音机自己扫出来的，按「波段 + 信号强弱」排。点一行就调过去。</p>' +
    '<div id="fsBody"><div class="empty" style="padding:22px 0">' + ic('refresh-cw') + '正在读…</div></div>',
    sheet => { paintFmStations($('#fsBody', sheet)); }
  );
}

async function paintFmStations(box){
  let j = null;
  try{ j = await get('/api/fmstations'); }catch (e){}
  const arr = (j && j.list) || [];
  if (!arr.length){
    box.innerHTML = '<div class="empty" style="padding:22px 0">' + ic('radio') +
      '还没搜到台<br><span style="font-size:12px;color:var(--ink3)">点上面的「重新搜台」扫一遍</span></div>';
    return;
  }
  const bandName = { FM: '调频', AM: '中波', LW: '长波', SW: '短波' };
  box.innerHTML = '<div class="optlist">' + arr.map(x => {
    // FM 存的是 kHz（98000），其他波段本来就是 kHz（999）；
    // 万一以后有人按「Hz」送进来也不至于显示成 999000 kHz
    const f = x.band === 'FM'
      ? (x.khz / 1000).toFixed(1) + ' MHz'
      : (x.khz >= 100000 ? (x.khz / 1000) : x.khz) + ' kHz';
    const bw = x.rssi >= 60 ? 4 : x.rssi >= 40 ? 3 : x.rssi >= 20 ? 2 : 1;
    let bars = '';
    for (let i = 0; i < 4; i++) bars += '<i class="' + (i < bw ? 'on' : '') + '"></i>';
    return '<button class="opt" data-khz="' + x.khz + '">' +
      ic('radio', 'ic sm') +
      '<span style="flex:1;text-align:left">' + (bandName[x.band] || x.band) + '　<b>' + f + '</b></span>' +
      '<span class="r">' + x.rssi + '</span></button>';
  }).join('') + '</div>';

  $$('[data-khz]', box).forEach(b => b.onclick = async () => {
    if (S.scr && S.scr.mode !== 'fm'){
      toast('机器现在是网络模式，先让它切到收音（断网会自动切）', true);
      return;
    }
    try{
      await post('/api/fmtune', { khz: +b.dataset.khz });
      toast('已调到 ' + b.textContent.trim().split('\u3000')[1]);
      closeSheet();
      setTimeout(pollScreen, 400);
    }catch (e){ toast('没调过去，重试一下', true); }
  });
}

// 搜台是异步的：每 1.2 秒问一次 /api/state，搜完自己停。
// 上限 40 次（约 48 秒）—— 比固件的搜台耗时留足余量，卡住了也不会一直轮询。
function waitFmScan(){
  let n = 0;
  const tick = async () => {
    n++;
    try{
      const st = await get('/api/state');
      renderFm(st);
      if (!st.fmscan || n > 40){
        if (n <= 40) toast('搜完了，共 ' + (st.fmstn || 0) + ' 个台');
        return;
      }
    }catch (e){}
    if (n <= 40) setTimeout(tick, 1200);
  };
  setTimeout(tick, 1200);
}

// ============================================================
//  WiFi —— 1.46.51 起「换一个 WiFi」和「已存的 WiFi」合成一页
//
//  用户原话：「已存的 wifi 改为三组，显示已保存的，没有的空着输入名字密码可以给收音机
//  添加 wifi，跟换一个 wifi 两个合并…框后面放一个搜索符号，点击可以扫描到当前有的
//  wifi 点击自动在框里填入名字」。
//
//  所以这一页就是 **N 个格子**（N = 固件容量，1.46.51 起是 4）：
//    · 有东西的格子 → 显示名字 + 「正在用 / 第 N 组」+ [连这个] [忘掉]
//    · 空着的格子   → 名字框 + 🔍 + 密码框 + [保存这组]
//  「保存」只写进 /wifi.txt，**不动当前连接**（见固件 apiWifiAdd 的注释）——
//  填一组备用网络不该把正在听的台打断。
//
//  ⚠️ 扫描回来一定要「重开一页」：openSheet() 会 sheetHost.innerHTML=''，
//     原来抓到的 box 立刻游离，往它里面写什么都看不见（1.46.36 就栽在这）。
//     所以用 _wfDraft 记草稿，重开时回填。
// ============================================================
const _wfDraft = { name: {}, pass: {} };   // 每个格子正在填的内容（扫描/重开时保留）

// 一页里最多画几个格子：固件说几个就几个（老固件没有 max 字段 → 按 3 个）
function wifiSlotCount(j){
  const m = +(j && j.max);
  return (m >= 1 && m <= 8) ? m : 3;
}

function wifiSlotHtml(i, x, max){
  const no = ['①','②','③','④','⑤','⑥','⑦','⑧'][i] || String(i + 1);
  if (x){
    // ---- 已存 ----
    return '<div style="border:1px solid var(--line);border-radius:12px;padding:10px 12px;margin-bottom:8px">' +
      '<div style="display:flex;align-items:center;gap:8px">' +
        ic('wifi', 'ic sm') +
        '<b style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:14.5px">' +
          no + ' ' + esc(x.ssid) + '</b>' +
        (x.on ? '<span style="font-size:12px;color:var(--ok);font-weight:700">正在用</span>'
              : '<span style="font-size:12px;color:var(--ink3)">第 ' + (i + 1) + ' 组</span>') +
      '</div>' +
      '<div style="display:flex;gap:8px;margin-top:8px">' +
        (x.on ? '' : '<button class="btn ghost" data-use="' + esc(x.ssid) + '" ' +
                     'style="padding:6px 12px;font-size:12.5px">连这个</button>') +
        '<button class="btn ghost" data-forget="' + esc(x.ssid) + '" ' +
          'style="padding:6px 12px;font-size:12.5px;color:var(--bad)">忘掉</button>' +
      '</div>' +
    '</div>';
  }
  // ---- 空着 ----
  const nm = _wfDraft.name[i] || '', pw = _wfDraft.pass[i] || '';
  return '<div style="border:1px dashed var(--line);border-radius:12px;padding:10px 12px;margin-bottom:8px">' +
    '<div style="font-size:12.5px;color:var(--ink3);margin-bottom:6px">' +
      no + ' 空着 —— 填上就存进机器，当备用网络</div>' +
    '<div style="display:flex;gap:6px">' +
      '<input class="wsName" data-i="' + i + '" value="' + esc(nm) + '" placeholder="WiFi 名字" ' +
        'autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" ' +
        'style="flex:1;padding:10px;border:1px solid var(--line);border-radius:9px;font-size:14px;background:var(--bg2,#fff);color:inherit">' +
      '<button class="btn ghost" data-scan="' + i + '" title="扫描附近的 WiFi" ' +
        'style="padding:0 12px;min-width:44px">' + ic('search', 'ic sm') + '</button>' +
    '</div>' +
    '<input class="wsPass" data-i="' + i + '" type="password" value="' + esc(pw) + '" ' +
      'placeholder="密码（没有密码就留空）" autocomplete="off" ' +
      'style="width:100%;margin-top:6px;padding:10px;border:1px solid var(--line);border-radius:9px;font-size:14px;background:var(--bg2,#fff);color:inherit">' +
    '<button class="btn pri" data-save="' + i + '" ' +
      'style="width:100%;margin-top:8px;padding:9px;font-size:13.5px">保存这组</button>' +
  '</div>';
}

function openWifiSheet(){
  const needSetup = !(S.diag && S.diag.wifi && S.diag.wifi.ok);
  openSheet(
    '<h3>' + (needSetup ? '给收音机连 WiFi' : 'WiFi') + '</h3>' +
    '<p class="desc">机器最多记 <b id="wfMax">4</b> 组，开机按顺序挨个试，连上就停。' +
    '空着的格子填上就存进机器当备用，<b>不会打断现在正在听的台</b>。</p>' +
    (needSetup
      ? '<div class="steps">' +
          '<div class="step"><b>1</b><span>手机连上热点 <b>我的收音机</b>（<b>没有密码</b>，连上会自动弹配网页）</span></div>' +
          '<div class="step"><b>2</b><span>在下面某个空格子里填 WiFi 名字和密码，点保存</span></div>' +
          '<div class="step"><b>3</b><span>连上后手机会自动回到原来的 WiFi，页面打不开就重连一下</span></div>' +
        '</div>'
      : '') +
    '<div id="wfSlots"><div class="empty" style="padding:22px 0">' + ic('refresh-cw') + '正在读机器…</div></div>' +
    '<button class="btn ghost" id="wfScanAll" style="width:100%;margin-top:6px">' +
      ic('scan-line', 'ic sm') + '重新扫描附近网络</button>' +
    '<p class="desc" style="margin-top:12px;font-size:12.5px">扫描是<b>收音机</b>在做，' +
      '所以手机得先够得着它：连它的热点，或者跟它在同一个 WiFi 下。</p>',
    sheet => {
      $('#wfScanAll', sheet).onclick = () => pickWifiForSlot(null);
      loadWifiSlots($('#wfSlots', sheet));
    }
  );
}

// 老名字留着：别的地方（以及旧回归）还在用
function openSavedWifiSheet(){ openWifiSheet(); }

async function loadWifiSlots(box){
  let j = null;
  try{ j = await get('/api/wifi/list'); }catch (e){}
  const arr = (j && j.list) || [];
  const max = wifiSlotCount(j);

  const mx = $('#wfMax');
  if (mx) mx.textContent = String(max);

  // 按槽位序号排（固件回的是文件里的顺序，也就是开机尝试顺序）
  const bySlot = {};
  arr.forEach((x, k) => { bySlot[+x.i >= 0 ? +x.i : k] = x; });

  let html = '';
  for (let i = 0; i < max; i++) html += wifiSlotHtml(i, bySlot[i], max);
  box.innerHTML = html;

  // 名字/密码改动随时记进草稿（扫描回来要保留）
  $$('.wsName', box).forEach(el => el.oninput = () => { _wfDraft.name[+el.dataset.i] = el.value; });
  $$('.wsPass', box).forEach(el => el.oninput = () => { _wfDraft.pass[+el.dataset.i] = el.value; });

  $$('[data-scan]', box).forEach(b => b.onclick = () => pickWifiForSlot(+b.dataset.scan));
  $$('[data-save]', box).forEach(b => b.onclick = () => saveWifiSlot(+b.dataset.save, box));
  $$('[data-use]', box).forEach(b => b.onclick = () => useSavedWifi(b.dataset.use));
  $$('[data-forget]', box).forEach(b => b.onclick = () => forgetWifi(b.dataset.forget, arr.length));

  // 设置页那两行也顺手更新（组数写在外面，不用点进来才知道）
  const nEl = $('#wifiNow');
  if (nEl && arr.length) nEl.textContent = arr.length + ' / ' + max + ' 组';
  const sub = $('#wifiSub');
  if (sub && arr.length) sub.textContent = '已存 ' + arr[0].ssid +
    (arr.length > 1 ? ' 等 ' + arr.length + ' 组' : '') + ' · 点这里管理';
}

// 扫描后把名字填进第 idx 个格子；idx 为 null = 只是想看看，不填
async function pickWifiForSlot(idx){
  openSheet(
    '<h3>选一个 WiFi</h3>' +
    '<p class="desc">' + (typeof idx === 'number'
      ? '点一个名字，会自动填进第 ' + (idx + 1) + ' 个格子'
      : '点一个名字，填进第一个空格子') + '</p>' +
    '<div id="pkBody"><div class="empty" style="padding:26px 0">' + ic('scan-line') +
      '正在扫描…<br><span style="font-size:12px;color:var(--ink3)">扫描是收音机在做，约 5 秒</span></div></div>' +
    '<button class="btn ghost" id="pkAgain" style="width:100%;margin-top:6px">' +
      ic('refresh-cw', 'ic sm') + '重新扫描</button>' +
    '<div class="acts"><button class="btn ghost" id="pkNo">取消</button>' +
      '<button class="btn ghost" id="pkType" style="margin-left:auto">直接手填</button></div>',
    sheet => {
      const again = () => scanIntoPick($('#pkBody', sheet), idx);
      $('#pkAgain', sheet).onclick = again;
      $('#pkNo', sheet).onclick = closeSheet;
      $('#pkType', sheet).onclick = () => { closeSheet(); setTimeout(openWifiSheet, 120); };
      again();
    }
  );
}

async function scanIntoPick(box, idx){
  box.innerHTML = '<div class="empty" style="padding:26px 0">' + ic('scan-line') +
    '正在扫描…<br><span style="font-size:12px;color:var(--ink3)">约 5 秒</span></div>';
  let arr = [];
  let fail = '';
  try{ arr = await get('/api/scan'); }catch (e){ fail = '连不上收音机'; }
  const bad = (arr || []).some(x => x && x.err);
  arr = (arr || []).filter(x => x && x.s).sort((a, b) => b.r - a.r);
  if (!arr.length){
    const why = fail || (bad ? '收音机刚才正忙着（在连网/在播放），稍等几秒再点「重新扫描」'
                             : '收音机周围没有可见的 WiFi');
    box.innerHTML = '<div class="empty" style="padding:26px 0">' + ic('wifi-off') + why +
      '<br><span style="font-size:12px;color:var(--ink3)">也可以点「直接手填」自己打名字</span></div>';
    return;
  }
  box.innerHTML = '<div class="optlist">' + arr.map((x, i) =>
    '<button class="opt" data-p="' + i + '">' + ic(x.e === 0 ? 'wifi' : 'lock', 'ic sm') +
      '<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + esc(x.s) + '</span>' +
      '<span class="r">' + x.r + ' dBm</span></button>'
  ).join('') + '</div>';
  $$('[data-p]', box).forEach(b => b.onclick = () => {
    const ssid = arr[+b.dataset.p].s;
    let slot = idx;
    if (typeof slot !== 'number'){                 // 没指定 → 填第一个空格子
      slot = 0;
      while (_wfDraft.name[slot]) slot++;
    }
    _wfDraft.name[slot] = ssid;
    closeSheet();
    setTimeout(openWifiSheet, 150);
  });
}

// 保存一个格子（只写盘，不切换当前连接）
async function saveWifiSlot(i, box){
  const nameEl = $('.wsName[data-i="' + i + '"]', box);
  const passEl = $('.wsPass[data-i="' + i + '"]', box);
  const name = (nameEl ? nameEl.value : '').trim();
  const pass = passEl ? passEl.value : '';
  if (!name){ toast('先填 WiFi 名字（可以点右边的放大镜扫一下）', true); return; }
  if (name.length > 32){ toast('名字太长了，检查一下', true); return; }
  try{
    const j = await postOk('/api/wifi/add', { ssid: name, pass });
    toast('已存进机器：' + name + (j && j.n ? '（现共 ' + j.n + ' 组）' : ''));
  }catch (e){
    if (e && e.code === 'full')       toast('机器只能记 ' + wifiSlotCount(S.wifiJ) + ' 组，先「忘掉」一组再加', true);
    else if (e && e.code === 'write') toast('机器存储写不进去，没存上', true);
    else if (e && e.code)             toast('没存上（' + e.code + '）', true);
    else                              toast('没收到回执，重开一次看看实际存没存');
  }
  _wfDraft.name[i] = '';
  _wfDraft.pass[i] = '';
  setTimeout(openWifiSheet, 320);         // 重开 = 重新问机器
  setTimeout(loadSettings, 700);
}

// 用已存的密码去连（不用重新输密码）
async function useSavedWifi(ssid){
  confirmSheet({
    title: '连到 ' + ssid + '？',
    desc: '机器会用存好的密码去连。正在播的台会中断十几秒；连不上会自动退回现在这个网络。',
    ok: '连这个',
    onOk: async () => {
      try{ await post('/api/wifi/use', { ssid }); }
      catch (e){
        if (e && e.code === 'notfound'){ toast('机器里找不到这一组了', true); setTimeout(openWifiSheet, 300); return; }
        // 换网本来就会把自己的链路掐断，没回执不代表没发出去
      }
      waitWifiSwap(ssid);
    }
  });
}

// 换网后的等待条：1.6 秒问一次状态，连上/失败都给结论
function waitWifiSwap(ssid){
  openSheet(
    '<h3>正在连 ' + esc(ssid) + '…</h3>' +
    '<div id="wsState" style="font-size:13.5px;color:var(--ink2);min-height:22px;margin:6px 0 4px">' +
      '最长 25 秒。连不上会自动退回原来的网络，机器不会失联。</div>' +
    '<div class="acts"><button class="btn ghost" id="wsBack">先回去</button></div>',
    sheet => {
      $('#wsBack', sheet).onclick = closeSheet;
      let n = 0;
      const t = setInterval(async () => {
        n++;
        try{
          const s = await get('/api/wifi/status');
          if (s && s.state === 2){
            clearInterval(t);
            const st = $('#wsState', sheet);
            if (st) st.innerHTML = '<span style="color:var(--ok);font-weight:700">连上了 · ' + esc(s.ip || '') + '</span>';
            toast('已换到 ' + ssid);
            setTimeout(() => { closeSheet(); loadSettings(); pollScreen(); }, 900);
          } else if (s && s.state === 3){
            clearInterval(t);
            const st = $('#wsState', sheet);
            if (st) st.innerHTML = '<span style="color:var(--bad);font-weight:700">' +
              esc(s.msg || '连不上') + '</span><div style="margin-top:6px;color:var(--ink3)">' +
              '已经退回原来的网络，机器没失联。</div>';
          } else {
            const st = $('#wsState', sheet);
            if (st) st.textContent = '正在连接… ' + n * 2 + 's';
          }
        }catch (e){}
        if (n > 16) clearInterval(t);
      }, 1600);
    }
  );
}

// 忘掉一组
function forgetWifi(ssid, total){
  const only = total <= 1;
  confirmSheet({
    title: '忘掉 ' + ssid + '？',
    desc: only
      ? '这是机器记的<b>最后一组</b> WiFi。删掉之后它就没网了，会自己开热点 —— '
        + '手机连上那个热点（没密码）就能重新配网。'
      : '机器会删掉这组名字和密码。如果它正在用这一组，会断开后改连剩下的那组。',
    ok: '忘掉', danger: true,
    onOk: async () => {
      try{
        const j = await postOk('/api/wifi/forget', { ssid });
        toast(j && j.left ? ('已忘掉 ' + ssid + '，机器里还剩 ' + j.left + ' 组')
                          : ('已忘掉 ' + ssid + '，机器里一组都不剩了'));
      }catch (e){
        if (e && e.code === 'notfound')   toast('机器里本来就没有这一组', true);
        else if (e && e.code === 'write') toast('机器存储写不进去，没删掉', true);
        else if (e && e.code)             toast('没删成（' + e.code + '）', true);
        else                              toast('没收到回执，重新读一遍机器的真实状态');
      }
      // 不能往旧 box 里写（它已随弹层销毁），重开一次 = 重新问机器
      setTimeout(() => { openWifiSheet(); setTimeout(loadSettings, 700); }, 320);
    }
  });
}

function bindPages(){
  // ---- 屏幕与声音：全部走 /api，收音机真的会变 ----
  const debounce = (fn, ms) => {
    let t = 0;
    return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
  };
  const sendSet = debounce((k, v, el) => {
    post('/api/set', { k, v }).then(j => {
      if (j && j.ok) toast({ bright: '亮度已设为 ' + v, offmin: '熄屏延时已保存' }[k] || '已设置');
      if (el) setRange(el, el.value);           // 刷新填充
      setTimeout(pollScreen, 500);
    }).catch(() => toast('设置没成功', true));
  }, 400);

  const rgVol = $('#rgVol'), rgBr = $('#rgBr');

  // 1.41.0：两路音量分开设（原厂 volume / netvolume）。
  //   ch=net 改「网络电台」那路，ch=fm 改「收音机」那路；
  //   目标那一路不是当前模式时，机器只记下来，切过去才生效（固件侧也是这么做的）。
  //   ⚠️ 原来写的是 `debounce(fn,300)()` —— 每次 input 都新建一个包装再立刻调用，
  //   防抖等于没有，拖一次滑条会连发几十个请求。改成先建实例、再调用。
  const sendVol = debounce((ch) => {
    const el = ch === 'fm' ? $('#rgFmVol') : $('#rgVol');
    if (!el) return;
    post('/api/vol', { v: +el.value, ch }).catch(() => {});
  }, 260);

  const rgFmVol = $('#rgFmVol');
  if (rgVol) rgVol.oninput = () => { setRange(rgVol, rgVol.value); sendVol('net'); };
  if (rgFmVol) rgFmVol.oninput = () => { setRange(rgFmVol, rgFmVol.value); sendVol('fm'); };
  if (rgBr) rgBr.oninput = () => setRange(rgBr, rgBr.value);
  if (rgBr) rgBr.onchange = () => sendSet('bright', +rgBr.value, rgBr);

  $$('#segOff button').forEach(b => b.onclick = () => {
    $$('#segOff button').forEach(x => x.classList.remove('on'));
    b.classList.add('on');
    post('/api/set', { k: 'offmin', v: +b.dataset.v })
      .then(j => toast(j && j.ok ? (b.dataset.v === '0' ? '已改为从不熄屏' : '已改为 ' + b.dataset.v + ' 分钟熄屏') : '设置没成功', !(j && j.ok)))
      .catch(() => toast('设置没成功', true));
  });

  $('#btnScreenOff').onclick = async () => {
    try{
      await post('/api/set', { k: 'screen', v: 0 });
      toast('已熄屏，转一下旋钮就亮');
      setTimeout(pollScreen, 600);
    }catch (e){ toast('没成功', true); }
  };

  $('#btnReboot').onclick = () => confirmSheet({
    title: '重启收音机？',
    desc: '大概 15 秒恢复。台表、音量、亮度这些设置都会保留。',
    ok: '重启',
    onOk: () => runFix('reboot')
  });

  $('#btnClearHist').onclick = () => confirmSheet({
    title: '清空收听记录？',
    desc: '时长统计和最近听过的台都会清掉，收音机本身不受影响。',
    ok: '清空', danger: true,
    onOk: async () => {
      try{ await post('/api/fix', { action: 'clear_history' }); S.hist = null; toast('已清空'); loadHistory(); }
      catch (e){ toast('没清成', true); }
    }
  });

  $('#btnRecheck').onclick = () => { toast('正在检查…'); loadDiag().then(() => toast('检查完了')); };
  $('#btnFixAll').onclick = fixAll;
  $$('.srow[data-fix]').forEach(b => b.onclick = () => runFix(b.dataset.fix));

  // ---- 1.41.0 收音机那一路 ----
  const rgSq = $('#rgSq');
  const sendSq = debounce((v) => {
    postOk('/api/squelch', { v }).then(() => {
      const now = $('#sqNow');
      if (now) now.textContent = v > 0 ? ('门限 ' + v) : '关';
      try{ localStorage.setItem('mr.sq', v); }catch (e){}
    }).catch(() => toast('静噪没设上', true));
  }, 350);
  if (rgSq) rgSq.oninput = () => {
    setRange(rgSq, rgSq.value);
    const now = $('#sqNow');
    if (now) now.textContent = +rgSq.value > 0 ? ('门限 ' + rgSq.value) : '关';
    sendSq(+rgSq.value);
  };

  const bHalf = $('#btnHalfStep');
  if (bHalf) bHalf.onclick = async () => {
    const sw = $('#swHalf');
    const on = !sw.classList.contains('on');
    try{
      await post('/api/halfstep', { on: on ? 1 : 0 });
      sw.classList.toggle('on', on);
      try{ localStorage.setItem('mr.hs', on ? '1' : '0'); }catch (e){}
      toast(on ? '半步进已开（0.05MHz 一格）' : '半步进已关');
    }catch (e){ toast('没设上，重试一下', true); }
  };

  const bStations = $('#btnFmStations');
  if (bStations) bStations.onclick = openFmStationsSheet;

  const bScan = $('#btnFmScan');
  if (bScan) bScan.onclick = () => confirmSheet({
    title: '重新搜一遍台？',
    desc: '收音机会把调频/中波/长波/短波都扫一遍，十几秒。搜的时候机器上转旋钮会卡一点，正在播的网络台不受影响。',
    ok: '开始搜',
    onOk: async () => {
      try{
        const j = await post('/api/fmscan');
        if (j && j.err === 'notfm'){
          toast('机器现在在听网络台，先在机器上切到收音模式', true);
          return;
        }
        if (j && j.err === 'busy'){ toast('已经在搜了，等它搜完'); waitFmScan(); return; }
        if (j && j.err === 'chip'){
          toast('收音芯片还没准备好，过几秒再点一次', true);
          return;
        }
        toast('正在搜台…');
        waitFmScan();
      }catch (e){ toast('没发出去', true); }
    }
  });

  // 1.46.51：两个入口合成一个 —— 点哪一行都开同一页（4 个格子）
  $('#btnWifi').onclick = openWifiSheet;
  const bSaved = $('#btnSavedWifi');
  if (bSaved) bSaved.onclick = openWifiSheet;      // 老页面还有这一行就用同一个
  $('#btnReconn').onclick = () => confirmSheet({
    title: '重新连接网络？',
    desc: '会断开当前的 WiFi 再连一次，大概 10 秒。正在播的台会中断一下。',
    ok: '重连',
    onOk: doReconnect
  });

  // ---- 1.8.0 远程控制（跨网）----
  // 1.42.0：**这里绝不能再发 on:0** —— 固件虽然会拒绝，但 UI 层面就不该给这条路。
  //   点这一行只是重新读一次状态（人站在这行前，多半是想知道「现在到底连没连上」）。
  $('#btnCloud').onclick = async () => {
    try{
      const c = await get('/api/cloud');
      S.cloud = c;
      $('#svRHost').textContent = (c.host || '—') + (c.port ? ':' + c.port : '');
      $('#svRSt').textContent = c.conn ? '已连上' : '连接中…';
      toast(c.conn ? '中转服务器已连上' : '正在连中转服务器…');
      if (!c.conn) setTimeout(loadSettings, 3000); // 给它 3 秒再确认一眼
    }catch (e){ toast('没读到状态', true); }
  };

  // ---- 1.46.7 连接码 ----
  $('#btnPinEdit').onclick = openPinEdit;

  // ---- 1.19.0 我的机器 ----
  initDevPin();          // 1.46.7：跨网输 4 位码那一行
  $('#btnDevRefresh').onclick = () => {
    cloudStart();
    renderDevs();
    toast('正在找机器…');
    setTimeout(renderDevs, 1500);
  };

  $('#btnResetStations').onclick = () => confirmSheet({
    title: '恢复出厂台表？',
    desc: '你自己加过、删过、改过顺序的台都会被清掉，换成出厂带的 341 个台。机器会重启。',
    ok: '恢复', danger: true,
    onOk: () => runFix('reset_stations')
  });

  $('#btnForgetWifi').onclick = () => confirmSheet({
    title: '忘掉 WiFi？',
    desc: '之后机器只会发自己的热点「我的收音机」（没有密码），连上它重新配网。家里的 WiFi 密码不会丢在别处，随时能再输一次。',
    ok: '忘掉', danger: true,
    onOk: () => runFix('forget_wifi')
  });

  $('#btnFactory').onclick = () => confirmSheet({
    title: '恢复出厂设置？',
    desc: '台表、收听记录、配对全部清空，等于刚买回来。这一步做完找不回来。',
    ok: '全部清掉', danger: true,
    onOk: () => runFix('factory')
  });
}

INIT.push(bindPages);
