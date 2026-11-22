/* ============================================================
   core.js —— 工具、接口、屏幕镜像、主页控制、导航
   说明：三个 js 文件会被拼进同一个 <script>，所以这里定义的
   const/S 是全局的，不要在别的文件里重名。
   ============================================================ */
'use strict';

const $  = (s, root) => (root || document).querySelector(s);
const $$ = (s, root) => Array.prototype.slice.call((root || document).querySelectorAll(s));
const INIT = [];

/* ---------------- 格式化 ---------------- */
function esc(s){
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => (
    {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]
  ));
}
function fmtDur(sec){
  sec = Math.max(0, Math.round(sec || 0));
  if (sec < 60) return sec + ' 秒';
  const m = Math.floor(sec / 60), h = Math.floor(m / 60);
  if (!h) return m + ' 分钟';
  return h + ' 小时' + (m % 60 ? ' ' + (m % 60) + ' 分' : '');
}
function fmtShort(sec){          // 统计卡用：13时55分 / 41分钟
  sec = Math.max(0, Math.round(sec || 0));
  const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60;
  if (h) return h + '<small>时</small>' + m + '<small>分</small>';
  if (m) return m + '<small>分钟</small>';
  return sec + '<small>秒</small>';
}
function fmtRel(unix){
  if (!unix) return '时间未知';
  const d = new Date(unix * 1000), n = Date.now();
  const diff = (n - d.getTime()) / 1000;
  if (diff < 90) return '刚刚';
  if (diff < 3600) return Math.round(diff / 60) + ' 分钟前';
  const today = new Date(); today.setHours(0,0,0,0);
  if (d >= today) return '今天 ' + String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
  const y = new Date(today); y.setDate(y.getDate() - 1);
  if (d >= y) return '昨天 ' + String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
  return (d.getMonth() + 1) + '月' + d.getDate() + '日';
}
function fmtUp(sec){
  sec = Math.round(sec || 0);
  const d = Math.floor(sec / 86400), h = Math.floor(sec / 3600) % 24, m = Math.floor(sec / 60) % 60;
  if (d) return d + ' 天 ' + h + ' 小时';
  if (h) return h + ' 小时 ' + m + ' 分';
  return m + ' 分钟';
}
function ic(name, cls){ return '<svg class="' + (cls || 'ic') + '"><use href="#i-' + name + '"></use></svg>'; }

/* ---------------- 轻提示 ---------------- */
let _toastT = 0;
function toast(msg, bad){
  const t = $('#toast');
  $('#toastTx').textContent = msg;
  t.classList.toggle('bad', !!bad);
  t.querySelector('use').setAttribute('href', bad ? '#i-circle-alert' : '#i-circle-check');
  t.classList.add('show');
  clearTimeout(_toastT);
  _toastT = setTimeout(() => t.classList.remove('show'), 2200);
}

/* ---------------- 底部弹层 ---------------- */
let _sheetEl = null;
function openSheet(html, onOpen){
  closeSheet(true);
  const host = $('#sheetHost');
  host.innerHTML = '<div class="mask" id="mask"><div class="sheet"><div class="grab"></div>' + html + '</div></div>';
  const mask = $('#mask');
  _sheetEl = mask;
  mask.addEventListener('click', e => { if (e.target === mask) closeSheet(); });
  requestAnimationFrame(() => requestAnimationFrame(() => mask.classList.add('in')));
  if (onOpen) onOpen($('.sheet', host));
  return mask;
}
function closeSheet(instant){
  const m = _sheetEl;
  if (!m) return;
  _sheetEl = null;
  if (instant) { $('#sheetHost').innerHTML = ''; return; }
  m.classList.remove('in');
  setTimeout(() => { if (!_sheetEl) $('#sheetHost').innerHTML = ''; }, 260);
}
function confirmSheet(o){
  openSheet(
    '<h3>' + esc(o.title) + '</h3>' +
    (o.desc ? '<p class="desc">' + o.desc + '</p>' : '') +
    '<div class="acts">' +
      '<button class="btn ghost" data-x="no">' + esc(o.cancel || '取消') + '</button>' +
      '<button class="btn ' + (o.danger ? 'danger' : 'pri') + '" data-x="yes">' + esc(o.ok || '确定') + '</button>' +
    '</div>',
    sheet => {
      $('[data-x="no"]', sheet).onclick = closeSheet;
      $('[data-x="yes"]', sheet).onclick = () => { closeSheet(); o.onOk && o.onOk(); };
    }
  );
}

/* ---------------- 接口 ---------------- */
const NET = { tk: '', fails: 0, lat: 0, busy: false, base: '', qTok: false };

/* ============================================================
   1.38.0：自动找机器（用户要的「打开 app 就是配好的」）
   ------------------------------------------------------------
   base 是「这台机器」的地址前缀：'' = 同源（App 就是从收音机加载的），
   或者 'http://192.168.1.23' 这种（收音机在路由器后面，App 从别处打开）。

   探测顺序有讲究：
     同源优先 —— 如果页面本身就是收音机给的，那一定是对的，零延迟；
     然后 myradio.local（mDNS，iOS/桌面可用，Android 上常常不通，所以不指望它）；
     再是上次成功过的 IP（换了 WiFi 之后 IP 会变，但常常不变）；
     最后 192.168.4.1（收音机自己的热点，用户刚配网时就在这个网里）。
   谁先应答就用谁，之后所有请求都打到那个主机上。
   ============================================================ */
function hostOf(path){
  // 已经确定过 base，直接用
  if (NET.base) return NET.base + path;
  return path;                       // base 还没定/同源
}
function rememberHost(){
  try{
    if (NET.base) localStorage.setItem('mr_host', NET.base);
    else localStorage.removeItem('mr_host');
  }catch (e){}
}
/* 试一个候选是不是真的收音机；是就返回它的 /api/info */
async function probeHost(base){
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), 1800);
  try{
    const r = await fetch(base + '/api/info', { cache: 'no-store', signal: ctl.signal });
    clearTimeout(t);
    if (!r.ok) return null;
    const j = await r.json();
    return j && j.name ? j : null;
  }catch (e){ clearTimeout(t); return null; }
}
/* 依次探测；找到就设 NET.base 并记住。返回设备信息或 null */
async function findRadio(){
  const cands = [''];
  const saved = (function(){ try{ return localStorage.getItem('mr_host') || ''; }catch(e){ return ''; } })();
  if (saved) cands.push(saved);
  cands.push('http://myradio.local', 'http://192.168.4.1');
  const seen = new Set();
  for (const c of cands){
    if (seen.has(c)) continue;
    seen.add(c);
    const j = await probeHost(c);
    if (!j) continue;
    // 只认这一台：第一次连上就记下设备码；以后连到别的机器，不悄悄换过去
    let want = '';
    try{ want = localStorage.getItem('mr_dev') || ''; }catch (e){}
    const dev = j.devid || '';
    if (!want && dev){
      try{ localStorage.setItem('mr_dev', dev); }catch (e){}
    }
    if (want && dev && dev !== want){
      console.warn('[MyRadio] 找到的是另一台机器 ' + dev + '，这台是 ' + want);
      continue;                     // 不是我家那台，跳过
    }
    NET.base = c;
    rememberHost();
    return j;
  }
  return null;
}

async function authOnce(){
  const tk = localStorage.getItem('mr_tk') || '';
  const r = await fetch(hostOf('/api/auth'), { cache: 'no-store' });
  const j = await r.json();
  if (j.locked) { const e = new Error('locked'); e.locked = true; throw e; }
  localStorage.setItem('mr_tk', j.token);
  NET.tk = j.token;
  return j.token;
}
async function authCode(code){
  const r = await fetch('/api/auth?code=' + encodeURIComponent(code), { cache: 'no-store' });
  const j = await r.json();
  if (j.locked || !j.token) { const e = new Error('code'); e.locked = true; throw e; }
  localStorage.setItem('mr_tk', j.token);
  NET.tk = j.token;
  return j.token;
}

/* 1.8.0：两个通道
     useCloud() 为真  → 走云端（MQTT），跨网也能用
     否则             → 走局域网 HTTP（同网时最快）
   云端那条路只有在「远程版」页面（window.MR_REMOTE）里才是首选；
   局域网页面里只有当 HTTP 彻底不通、而云端又恰好连着时才兜底。 */
function useCloud(){
  return !!window.MR_REMOTE || !!(window.NET && NET.forceCloud);
}

/* 1.46.19：固件那边如果没在 server.collectHeaders() 里登记 X-Token，
   这个头会被整个丢掉（server.header() 永远是空串）—— 页面就会一路 401，
   用户看到的就是「进得去首页、但顶上写没连上收音机」。
   所以头这条路走不通时改走 ?t=<token>（固件 authed() 一直认这个参数）；
   一旦发现 ?t= 能用，本次会话就都用它，别每次都白挨一轮 401。 */
function tokQ(url){
  return url + (url.indexOf('?') < 0 ? '?' : '&') + 't=' + encodeURIComponent(NET.tk);
}
async function getLan(path){
  if (!NET.tk) await authOnce();
  const url = hostOf(path);
  if (NET.qTok){
    const rq = await fetch(tokQ(url), { cache: 'no-store' });
    if (rq.ok) return rq.json();
    if (rq.status !== 401) throw new Error('http ' + rq.status);
    NET.qTok = false;                       // 令牌失效了，下面重走一遍认证
  }
  let r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' });
  if (r.status === 401) { await authOnce(); r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' }); }
  if (r.status === 401) {
    r = await fetch(tokQ(url), { cache: 'no-store' });
    if (r.ok) NET.qTok = true;
  }
  if (!r.ok) throw await httpErr(r);
  return r.json();
}
// 1.46.36：非 2xx 时把 body 里的 {"err":"..."} 也读出来。
//   固件对「参数不对 / 没有这一组 / 写盘失败」都是 errJson(4xx, "xxx")，
//   只抛 "http 404" 的话，按 e.code 分流的分支全都进不去，用户只会看到一句「没成功」。
async function httpErr(r){
  let code = '';
  try{
    const b = await r.json();
    if (b && b.err) code = String(b.err);
  }catch (e){}
  const err = new Error(code || ('http ' + r.status));
  if (code) err.code = code;
  err.status = r.status;
  return err;
}

async function postLan(path, body){
  if (!NET.tk) await authOnce();
  const url = hostOf(path);
  const send = (u, tok) => fetch(u, {
    method: 'POST',
    headers: tok
      ? { 'X-Token': NET.tk, 'Content-Type': 'application/x-www-form-urlencoded' }
      : { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(body || {})
  });
  if (NET.qTok){
    const rq = await send(tokQ(url), false);
    if (rq.ok) return rq.json();
    if (rq.status !== 401) throw new Error('http ' + rq.status);
    NET.qTok = false;
  }
  let r = await send(url, true);
  if (r.status === 401) { await authOnce(); r = await send(url, true); }
  if (r.status === 401) {
    const rq = await send(tokQ(url), false);
    if (rq.ok) NET.qTok = true;
    r = rq;
  }
  if (!r.ok) throw await httpErr(r);
  return r.json();
}

async function get(path){
  if (useCloud() && CLOUD.active()) return CLOUD.request(path, null, 'GET');
  if (useCloud()) throw new Error('cloud down');
  try{
    return await getLan(path);
  }catch (e){
    if (CLOUD.active()) return CLOUD.request(path, null, 'GET');   // 家里也连不上时兜底
    throw e;
  }
}
// 1.46.33：动作类接口出错时固件回的是 200 + {"err":"..."}，fetch 根本不算失败。
//   以前一律 .then(() => toast('已删除')) —— 机器明明没删，App 说"已删除"，
//   用户再拉一次列表发现台还在，就成了「删除失败 / 没设上」。
//   凡是有"成功/失败"语义的调用都改走这个。
async function postOk(path, body){
  const j = await post(path, body);
  if (j && j.err){ const e = new Error(String(j.err)); e.code = j.err; throw e; }
  if (j && (j.ok === 0 || j.ok === false)){ const e = new Error('nok'); e.code = 'nok'; throw e; }
  return j;
}
async function post(path, body){
  if (useCloud() && CLOUD.active()) return CLOUD.request(path, body, 'POST');
  if (useCloud()) throw new Error('cloud down');
  try{
    return await postLan(path, body);
  }catch (e){
    if (CLOUD.active()) return CLOUD.request(path, body, 'POST');
    throw e;
  }
}

/* ---------------- 配对码门 ---------------- */
function pairGate(){
  const b = $('#boot');
  b.style.display = '';
  b.classList.remove('gone');
  b.innerHTML =
    '<div class="logo">' + ic('key-round') + '</div>' +
    '<div style="text-align:center;max-width:300px;padding:0 24px">' +
      '<div style="font-size:17px;font-weight:800;margin-bottom:6px">这台收音机开了配对码保护</div>' +
      '<div style="font-size:13.5px;color:var(--ink2);line-height:1.7;margin-bottom:16px">' +
        '看一下收音机的屏幕，上面有 6 个数字，输进来就能控制它。' +
        '屏幕没显示的话，按一下旋钮。</div>' +
      '<input id="pcIn" inputmode="numeric" autocomplete="one-time-code" maxlength="6" ' +
        'style="width:100%;height:54px;text-align:center;letter-spacing:10px;font-size:26px;' +
        'font-weight:800;border-radius:16px;border:1px solid var(--line);background:var(--card);' +
        'outline:none;padding-left:10px" placeholder="······">' +
      '<button class="btn pri" id="pcGo" style="margin-top:12px;width:100%">连接</button>' +
      '<button class="btn ghost" id="pcAgain" style="margin-top:8px;width:100%;height:42px;font-size:14px">' +
        '再让屏幕显示一次</button>' +
    '</div>';
  $('#pcGo').onclick = async () => {
    const v = ($('#pcIn').value || '').trim();
    if (v.length !== 6) { toast('要 6 个数字', true); return; }
    try { await authCode(v); location.reload(); }
    catch (e) { toast('配对码不对，再看看屏幕', true); }
  };
  $('#pcAgain').onclick = async () => {
    try { await fetch('/api/auth', { cache: 'no-store' }); } catch (e) {}
    toast('看收音机的屏幕');
  };
}

/* ---------------- 全局状态 ---------------- */
const S = {
  scr: null,
  tags: [],          // 1.15.0：标签清单（机器不再分组，标签只用于筛选）
  tag: -1,           // 当前筛的标签下标，-1 = 全部（只在本机生效）
  list: [],          // 整张台表（台数随台表变，别写死）
  page: 'home',
  diag: null,
  hist: null,
  pair: null,
  q: '',
  offline: false,     // 连不上收音机时置位：不拦路，让用户进来配网
  hl: null,           // 1.17.0：/api/health 的最新一份（每台的健康档案）
  hlOff: false        // 老固件没有这个接口时置位，界面就不显示那些标
};

/* ============================================================
   屏幕镜像：320×170，坐标与 src/ui.cpp 一一对应
   ============================================================ */
const SCR_W = 320, SCR_H = 170;
const cv = document.getElementById('scr');
const ctx = cv.getContext('2d');

const C = {
  bg:'#000000', name:'#e9e9e9', accent:'#b06b03', cool:'#01839d',
  sub:'#cfcece', barBg:'#202020', namBg:'#004e61', namFg:'#ffff00',
  bandBg:'#b06b03', bandFg:'#000000', modeBg:'#004e61', modeFg:'#e9e9e9',
  time:'#cfcece', err:'#b20a0a', dim:'#8c8e8c',
  // 调谐盘配色，与固件 config.h 的 C_DIAL_* 一致
  dialBg:'#084d5a', dialFg:'#eef4f4', dialMk:'#c8e8e8', dialPtr:'#b20a0a'
};
const F16 = 'bold 15px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';
const F16R = '600 15px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';
const F32 = 'bold 31px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';

function fitCanvas(){
  const w = cv.clientWidth || 320;
  const dpr = Math.min(window.devicePixelRatio || 1, 3);
  cv.width  = Math.round(w * dpr);
  cv.height = Math.round(w * SCR_H / SCR_W * dpr);
  ctx.setTransform(cv.width / SCR_W, 0, 0, cv.height / SCR_H, 0, 0);
}

function rect(x, y, w, h, c){
  ctx.fillStyle = c;
  ctx.fillRect(x, y, w, h);
}
function textW(s, font){
  ctx.font = font;
  return ctx.measureText(s).width;
}
function text(s, x, y, c, font){
  ctx.fillStyle = c;
  ctx.font = font;
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  ctx.fillText(s, x, y);
}
/* 在色块里「垂直居中」写字。
   canvas 的 textBaseline='top' 对齐的是 em 框顶，中文字形实际在 y-1..y+11 之间（15px 字号实测），
   measureText 的 actualBoundingBox 各浏览器符号不一致、这里不依赖它，直接用实测字形高。 */
function textInBox(s, x, boxY, boxH, c, font, align){
  ctx.font = font;
  ctx.fillStyle = c;
  ctx.textAlign = align || 'left';          // 'left' 时 x 是左缘，'right' 时是右缘
  ctx.textBaseline = 'top';
  const gh = 13;                            // 15px 字形的实际高度
  ctx.fillText(s, x, Math.round(boxY + (boxH - gh) / 2));
  ctx.textAlign = 'left';
}
function textR(s, xr, y, c, font){
  ctx.fillStyle = c;
  ctx.font = font;
  ctx.textAlign = 'right';
  ctx.textBaseline = 'top';
  ctx.fillText(s, xr, y);
  ctx.textAlign = 'left';
}
function textC(s, y, c, font){
  ctx.fillStyle = c;
  ctx.font = font;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  ctx.fillText(s, SCR_W / 2, y);
  ctx.textAlign = 'left';
}
function ellipsis(s, maxW, font){
  ctx.font = font;
  if (ctx.measureText(s).width <= maxW) return s;
  let out = s;
  while (out.length > 1 && ctx.measureText(out + '…').width > maxW) out = out.slice(0, -1);
  return out + '…';
}

/* 台名里写着真实频率的，盘上就按它摆指针：FM89.1 / 电台914 / 1008 / FM100
   规则与 src/ui.cpp 的 nameFreq10 一一对应，改一边必须改另一边，否则镜像会对不上。 */
function nameFreq10(s){
  s = s || '';
  for (let i = 0; i < s.length; ){
    const ch = s[i];
    if (ch < '0' || ch > '9'){ i++; continue; }
    let j = i, v = 0, nd = 0;
    while (j < s.length && s[j] >= '0' && s[j] <= '9'){
      if (nd < 5){ v = v * 10 + (+s[j]); nd++; }
      j++;
    }
    if (nd > 4){ i = j; continue; }             // 5 位以上：年份 / 流水号
    let f = -1;
    if (s[j] === '.' && s[j + 1] >= '0' && s[j + 1] <= '9'){
      f = v * 10 + (+s[j + 1]);
      j += 2;
      while (j < s.length && s[j] >= '0' && s[j] <= '9') j++;
    }
    if (f < 0){
      const tenths = v, whole = v * 10;
      if (tenths >= 640 && tenths <= 1080) f = tenths;
      else if (whole >= 640 && whole <= 1080) f = whole;
    }
    if (f >= 640 && f <= 1080) return f;
    i = j > i ? j : i + 1;
  }
  return -1;
}

// 信号柱（4 格）
function sigBars(x, y, ok, rssi){
  let bars = 0;
  if (ok) bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  for (let i = 0; i < 4; i++){
    const h = 4 + i * 3;
    rect(x + i * 6, y + 13 - h, 4, h, i < bars ? C.cool : C.barBg);
  }
}
// 1.44.0：音量两路量程不同（原厂 volume 0..63 / netvolume 0..21）。
//   显示、进度条一律按「当前这一路」的 max 算 —— 写死 100 会让 FM 的音量 40
//   只画出 40% 长，和屏幕上（ui.cpp）那条对不上。
function volMaxOf(s){
  const fm = !!s && s.mode === 'fm';
  const mx = fm ? s.fvolmax : s.volmax;
  return (mx != null && +mx > 0) ? +mx : (fm ? 63 : 21);
}

// 音量胶囊（1.4.5：与色块同高 20，数字居中，不再顶出色块）
function volPill(x, y, vol, muted, vmax){
  const w = 52, h = 20;
  rect(x, y, w, h, C.barBg);
  ctx.strokeStyle = muted ? C.accent : C.cool; ctx.lineWidth = 1;
  ctx.strokeRect(x + .5, y + .5, w - 1, h - 1);
  const fw = muted ? 0 : Math.round(vol * (w - 22) / (vmax || 100));
  if (fw > 0) rect(x + 2, y + 2, fw, h - 4, C.cool);
  const label = String(muted ? 0 : vol);
  textInBox(label, x + w - 3, y, h, muted ? C.accent : C.sub, F16R, 'right');
}
function badge(x, y, label, fg, bg){
  const w = textW(label, F16R) + 8;
  rect(x, y, w, 20, bg);
  textInBox(label, x + 4, y, 20, fg, F16R);   // 垂直居中，不贴上下边
  return x + w + 4;
}
function tri(x1, y1, x2, y2, x3, y3, c){
  ctx.fillStyle = c;
  ctx.beginPath();
  ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3);
  ctx.closePath(); ctx.fill();
}
function psColor(s){
  if (s.mode === 'fm') return s.mute ? C.sub : C.cool;   // 收音不靠网络，红点会让人以为坏了
  if (!s.wifi) return C.err;
  switch (s.ps){
    case 2: return C.cool;
    case 1: return C.accent;
    case 3: return C.accent;
    case 4: return C.err;
    default: return C.sub;
  }
}
// 台数到三位（100/140）时序号占掉半个框宽，状态词要退成 2 字
function psShort(lbl){
  return ({ '播放中':'播放', '连接中':'连接', '缓冲中':'缓冲', '出错了':'出错',
            '已暂停':'暂停', '已静音':'静音',
            '调频收音':'调频', '中波收音':'中波', '长波收音':'长波', '短波收音':'短波' })[lbl] || lbl;
}
function psLabel(s){
  if (s.mode === 'fm') return s.mute ? '已静音' : radioBandWord(radioKhz(s)) + '收音';
  switch (s.ps){
    case 2: return '播放中';
    case 1: return '连接中';
    case 3: return '缓冲中';
    case 4: return '出错了';
    default: return s.mute ? '已静音' : '已暂停';
  }
}

/* ---------------- 收音波段环（1.10.0）：一个旋钮走完 长波→中波→调频→短波 ----------------
   与 src/fm.cpp 的 kBands / radioWalk() 一一对应，频率单位全部是 kHz。
   长波 153~279、中波 531~1602、调频 64000~108000、短波 2300~26100。
   跨过波段边界直接跳到下一波段的第一格 —— 空白频段不占格子，所以「不用切波段」。 */
const RBANDS = [
  { k: 'LW', lo: 153,   hi: 279,    step: 9   },
  { k: 'MW', lo: 531,   hi: 1602,   step: 9   },
  { k: 'FM', lo: 64000, hi: 108000, step: 100 },
  { k: 'SW', lo: 2300,  hi: 26100,  step: 5   },
];
function radioClosest(khz){
  let best = RBANDS[0].lo, bd = Infinity;
  for (const b of RBANDS){
    for (const e of [b.lo, b.hi]){
      const d = Math.abs(khz - e);
      if (d < bd){ bd = d; best = e; }
    }
  }
  return best;
}
function radioBandOf(khz){
  for (let i = 0; i < RBANDS.length; i++)
    if (khz >= RBANDS[i].lo && khz <= RBANDS[i].hi) return i;
  const near = radioClosest(khz);
  for (let i = 0; i < RBANDS.length; i++)
    if (near >= RBANDS[i].lo && near <= RBANDS[i].hi) return i;
  return 0;
}
function radioSnap(khz){
  for (const b of RBANDS) if (khz >= b.lo && khz <= b.hi) return khz;
  return radioClosest(khz);
}
function radioWalk(khz, clicks, gear){
  let k = radioSnap(khz), bi = radioBandOf(k);
  if (!gear || gear < 1) gear = 1;
  let n = Math.abs(clicks); if (n > 400) n = 400;
  for (let i = 0; i < n; i++){
    const st = RBANDS[bi].step * gear;
    if (clicks > 0){
      let v = k + st;
      if (v > RBANDS[bi].hi){ bi = (bi + 1) % RBANDS.length; v = RBANDS[bi].lo; }
      k = v;
    } else {
      let v = k - st;
      if (v < RBANDS[bi].lo){ bi = (bi + RBANDS.length - 1) % RBANDS.length; v = RBANDS[bi].hi; }
      k = v;
    }
  }
  return k;
}
// 转速 → 加速倍数：慢转 1（精调）、正常 5、快转 10、甩起来 30
function radioGear(msPerClick){
  if (msPerClick <= 60)  return 30;
  if (msPerClick <= 130) return 10;
  if (msPerClick <= 400) return 5;
  return 1;
}
function radioBandWord(khz){
  const k = RBANDS[radioBandOf(khz)].k;
  return ({ LW: '长波', MW: '中波', SW: '短波' })[k] || '调频';
}
function radioFreqText(khz){
  if (RBANDS[radioBandOf(khz)].k === 'FM') return 'FM ' + (khz / 1000).toFixed(1);
  return radioBandWord(khz) + ' ' + khz;
}

// 老固件（1.9.0 及以前）只给 fmfreq（100kHz），新固件给 khz + band
function radioKhz(s){
  if (s && typeof s.khz === 'number' && s.khz > 0) return s.khz;
  return (s && s.fmfreq) ? s.fmfreq * 100 : 98000;
}

/* ---------------- 有声书（1.9.0）：一个台 = 一部书 ----------------
   固件在 /api/screen 里给 epn（共几集）/ epi（第几集，0 基）/ ept（这一集讲什么），
   普通台 epn=0，这里返回 null，界面就整块藏起来。 */
function epInfo(s){
  const n = s && s.epn ? s.epn : 0;
  if (n <= 1) return null;
  return { n: n, i: Math.max(0, Math.min(n - 1, s.epi | 0)), t: s.ept || '', prog: s.prog || '' };
}

function drawScreen(s){
  rect(0, 0, SCR_W, SCR_H, C.bg);

  // 熄屏
  if (!s.on){
    textC('屏幕已熄灭', 62, '#4a4a4a', F16R);
    textC('转一下旋钮就亮', 86, '#3a3a3a', F16R);
    return;
  }

  // 配对码页（占满全屏）
  if (s.pc){
    rect(0, 0, SCR_W, 3, C.accent);
    textC('配对码', 26, C.sub, F16R);
    textC('······', 48, C.name, F32);
    textC('在手机上输入这 6 个数字', 108, C.sub, F16R);
    textC('输完就能控制收音机', 132, C.dim, F16R);
    return;
  }

  /* ---- 顶栏 0..22：时钟 + 星期 + 组名 | WiFi + 信号 + 音量 ---- */
  let x = 5;
  const clk = s.clock || '--:--';
  text(clk, x, 3, C.time, F16R);
  x += textW(clk, F16R) + 6;
  if (s.wd >= 0){
    const wd = ['周日','周一','周二','周三','周四','周五','周六'][s.wd % 7];
    text(wd, x, 3, C.sub, F16R);
    x += textW(wd, F16R) + 8;
  }
  const wf = s.wifi ? 'WiFi' : '断网';
  const wbg = s.wifi ? C.modeBg : C.err;
  const ww = textW(wf, F16R) + 8;
  const rightX = 232 - 4 - ww;
  // 1.15.0：这块显示的是「当前这台所属的标签」，没有标签就不画
  const gname = s.gname ? ellipsis(s.gname, 66, F16R) : '';
  let gbw = gname ? textW(gname, F16R) + 8 : 0;
  if (x + gbw > rightX - 8) gbw = 0;   // 标签色块不许压到右侧
  if (gbw > 0){
    rect(x, 1, gbw, 20, C.bandBg);
    textInBox(gname, x + 4, 1, 20, C.name, F16R);
  }
  rect(rightX, 1, ww, 20, wbg);
  textInBox(wf, rightX + 4, 1, 20, C.modeFg, F16R);
  sigBars(232, 2, !!s.wifi, s.rssi);
  volPill(SCR_W - 5 - 52, 1, s.vol, s.mute, volMaxOf(s));

  /* ---- 台名 22..98（76 高）：FM 收音时这里是频率 ---- */
  const n = s.cnt || 0;
  const isFm = s.mode === 'fm';
  const bigName = isFm ? radioFreqText(radioKhz(s))
                       : ellipsis(s.name || '没有电台', SCR_W - 20, F32);
  const ep = isFm ? null : epInfo(s);
  if (ep){
    // 有声书：书名上移 6px，下面补一行「第 N 集 + 这一集讲什么」（与 ui.cpp 同坐标）
    textC(bigName, 22 + 6, s.pv >= 0 ? C.sub : C.name, F32);
    const sub2 = '第' + (ep.i + 1) + '集' + (ep.t ? ' ' + ep.t : '');
    textC(ellipsis(sub2, SCR_W - 40, F16R), 22 + 76 - 22, C.sub, F16R);
  } else {
    textC(bigName, 22 + Math.round((76 - 31) / 2), s.pv >= 0 ? C.sub : C.name, F32);
  }

  /* ---- 三个小框 102..126 ---- */
  const by = 102, bh = 24;
  rect(5, by, 118, bh, C.barBg);
  ctx.fillStyle = psColor(s);
  ctx.beginPath(); ctx.arc(15, by + bh / 2, 4, 0, Math.PI * 2); ctx.fill();
  // 有声书这一格显示「第几集 / 共几集」，普通台还是「第几台 / 共几台」
  const ord = isFm ? ''                      // 收音模式：右边「第几台」没意义
                 : (ep ? ((ep.i + 1) + '/' + ep.n)
                       : ((n > 0 ? (s.idx + 1) : 0) + '/' + n));
  const ordW = textW(ord, F16R);
  let lbl = s.wifi ? psLabel(s) : '断网';
  if (textW(lbl, F16R) + ordW > 123 - 23 - 6 - 8) lbl = psShort(lbl);
  textInBox(lbl, 23, by, bh, C.name, F16R);
  textInBox(ord, 5 + 118 - 6, by, bh, C.sub, F16R, 'right');

  rect(128, by, 86, bh, C.barBg);
  let bars = 0;
  if (s.wifi) bars = s.rssi > -55 ? 4 : s.rssi > -67 ? 3 : s.rssi > -78 ? 2 : 1;
  for (let i = 0; i < 4; i++){
    const hh = 4 + i * 3;
    rect(128 + 8 + i * 7, by + 19 - hh, 5, hh, i < bars ? C.cool : C.bg);
  }
  textInBox(String(s.rssi), 128 + 86 - 7, by, bh, C.sub, F16R, 'right');

  rect(219, by, 96, bh, C.barBg);
  textInBox('音量', 225, by, bh, s.mute ? C.accent : C.sub, F16R);
  textInBox(String(s.mute ? 0 : s.vol), 219 + 96 - 6, by, bh, C.name, F16R, 'right');
  rect(225, by + 21, 84, 2, C.bg);   // 条压到框底，上面整块留给文字
  const vfw = s.mute ? 0 : Math.round(s.vol * 84 / volMaxOf(s));
  if (vfw > 0) rect(225, by + 21, vfw, 2, C.cool);

  /* ---- 调谐盘 134..170：指针钉在正中，刻度随台滚动（与 ui.cpp 一致） ---- */
  const TUN_Y = 134, TUN_H = SCR_H - TUN_Y;
  const TRI_H = 8, TRI_TOP = TUN_Y - TRI_H;  // 蓝条上方那条带子，放倒三角
  const DIAL_PX = 5, DIAL_CX = SCR_W >> 1;   // 每 0.1MHz 占 5px，指针永不动
  rect(0, TUN_Y, SCR_W, TUN_H, C.dialBg);
  rect(0, TRI_TOP, SCR_W, TRI_H, C.bg);      // 空带擦干净，倒三角才不拖影
  const base = TUN_Y + TUN_H - 1;

  // 收音模式：按当前波段的频道栅格铺刻度（与 ui.cpp 的 drawTuner 同规则）
  if (isFm){
    const khz = radioKhz(s);
    const band = RBANDS[radioBandOf(khz)];
    const curT = (khz - band.lo) / band.step;
    const half = Math.floor(SCR_W / (2 * DIAL_PX));
    const lab  = (band.k === 'MW' || band.k === 'LW') ? 10 : 20;
    for (let t = curT - half; t <= curT + half; t++){
      const k = band.lo + t * band.step;
      if (k < band.lo || k > band.hi) continue;
      const x = DIAL_CX + (t - curT) * DIAL_PX;
      if (x < 0 || x >= SCR_W) continue;
      const isLab = (t % lab === 0) || (k === band.hi);
      const isMid = (t % 5 === 0);
      const hh = isLab ? 16 : (isMid ? 6 : 3);
      rect(x, base - hh + 1, 1, hh, isLab ? C.dialFg : C.dialMk);
    }
    for (let t = curT - half; t <= curT + half; t++){
      const k = band.lo + t * band.step;
      if (k < band.lo || k > band.hi) continue;
      if ((t % lab !== 0) && (k !== band.hi)) continue;
      const lb = band.k === 'FM' ? (k / 1000).toFixed(1) : String(k);
      const tw = textW(lb, F16R);
      const tx = DIAL_CX + (t - curT) * DIAL_PX - Math.round(tw / 2);
      if (tx < 1 || tx + tw > SCR_W - 1) continue;
      text(lb, tx, TUN_Y + 2, C.dialFg, F16R);
    }
    rect(DIAL_CX - 1, TUN_Y, 2, TUN_H, C.dialPtr);
    tri(DIAL_CX, TUN_Y + 2, DIAL_CX - 5, TRI_TOP, DIAL_CX + 5, TRI_TOP, C.dialPtr);
    return;
  }

  const idx2 = Math.min(Math.max(0, n - 1), Math.max(0, s.idx | 0));
  // 台名里写了频率就认它（FM89.1 → 891），没写才按第几台摊（与 ui.cpp 同规则）
  let fcur = 975;
  {
    const nf = nameFreq10(s.name);
    if (nf > 0) fcur = nf;
    else if (n > 0) fcur = n <= 1 ? 975 : 875 + Math.floor(205 * idx2 / (n - 1));
  }
  const left = fcur - Math.round(DIAL_CX / DIAL_PX);
  const right = left + Math.floor(SCR_W / DIAL_PX);

  // 刻度：每 0.5 MHz 短（6px）、每 1 MHz 中（10px）、每 2 MHz 长（16px，带数字）
  // 不夹在 87.5~108 之间——整条屏都铺刻度，第 1 台指针左边也有刻度可看
  let f0 = Math.floor(left / 5) * 5;   // 向下取整，x=0 处就有刻度
  let fEnd = right;
  // 网络台的频率是按台序编出来的，整条屏都铺刻度
  for (let f10 = f0; f10 <= fEnd; f10++){
    const isHalf = (f10 % 5) === 0;
    const isMhz = (f10 % 10) === 0;
    const isBig = isMhz && (((f10 / 10) % 2) === 0);   // 88 / 90 / 92 … / 108
    const hh = isBig ? 16 : (isMhz ? 10 : (isHalf ? 6 : 3));
    const x = (f10 - left) * DIAL_PX;
    rect(x, base - hh + 1, 1, hh, isMhz ? C.dialFg : C.dialMk);
  }

  // 频率数字：对准 2MHz 长刻度，站不下就整个不画
  for (let f10 = f0; f10 <= fEnd; f10++){
    if ((f10 % 10) !== 0 || ((f10 / 10) % 2) !== 0) continue;
    if (f10 < 880 || f10 > 1080) continue;   // 波段外只画刻度不写数字
    const lb = (f10 / 10) + '.0';
    const tw = textW(lb, F16R);
    const tx = (f10 - left) * DIAL_PX - Math.round(tw / 2);
    if (tx < 1 || tx + tw > SCR_W - 1) continue;
    text(lb, tx, TUN_Y + 2, C.dialFg, F16R);
  }

  // 指针最后画，压在刻度与数字「上面」。
  // canvas 的 rect(x, w) 覆盖 [x, x+w)，所以 2px 从 DIAL_CX-1 起 → 中心正好 DIAL_CX，与三角尖同线
  if (n > 0){
    const pc = s.pv >= 0 ? C.sub : C.dialPtr;
    rect(DIAL_CX - 1, TUN_Y, 2, TUN_H, pc);  // 只走蓝条高度
    // 尖扎进蓝条 2px：三角形最后几行宽度近 0，抗锯齿后会被抹掉，看着像跟竖线断开了
    tri(DIAL_CX, TUN_Y + 2, DIAL_CX - 5, TRI_TOP, DIAL_CX + 5, TRI_TOP, pc);
  }
}

/* ---------------- 主页 ---------------- */
// 1.15.0：主页这排标签只是「跳到电台页并筛好」的快捷方式，不再切机器的组
function renderHomeChips(){
  const box = $('#homeChips');
  if (!S.tags || !S.tags.length){ box.innerHTML = ''; return; }
  box.innerHTML = S.tags.map(t =>
    '<button class="chip" data-t="' + t.i + '">' +
      esc(t.n) + '<b>' + t.c + '</b></button>'
  ).join('') +
  '<button class="chip ghost" data-go="stations">' + ic('list-music','ic sm') + '全部电台</button>';

  $$('#homeChips .chip').forEach(b => {
    b.onclick = () => {
      if (b.dataset.go){ S.tag = -1; go('stations'); return; }
      S.tag = +b.dataset.t;              // 只设筛选，机器上什么都不变
      go('stations');
    };
  });
}

function updateHome(s){
  // 顶栏副标题
  const on = !!s.wifi;
  // 1.15.0：gname 现在是「当前这台所属的标签」，不是分组名
  $('#hdrSub').textContent = s.mode === 'fm'
    ? '正在收音' + (on ? '' : ' · 没网也能听') + (useCloud() ? ' · 外地控制' : '')
    : (on
        ? (s.gname ? '标签 · ' + s.gname : '已连接') + (useCloud() ? ' · 外地控制' : '')
        : '机器没连上 WiFi');
  $('#hdrSub').style.color = on ? '' : 'var(--bad)';

  // 状态行（有声书把「第几集」拼在台名后面，不用再点进去看）
  const ep = epInfo(s);
  // 1.41.0：收音模式下「正在放什么」不是网络台名，而是波段 + 频率。
  //   原来这里一直写 s.name（网络台），机器在收音时 App 顶部还写着「中国之声」，
  //   看起来像 App 没同步。
  const fmOn = s.mode === 'fm';
  const bandWord = { fm: '调频', am: '中波', lw: '长波', sw: '短波' }[s.band] || '';
  // ⚠️ 单位：固件 /api/screen 的 fmfreq 是 **kHz/10**（98.4MHz → 9840），
  //   所以换成 MHz 要 /100，不是 /10（原来写 /10，屏上显示成「984.0」）。
  const fmTx = fmOn
    ? (bandWord + ' ' + (s.fmfreq ? (s.fmfreq / 100).toFixed(1) : (s.khz || '')))
    : '';
  const nm = fmOn ? fmTx : ((s.name || '') + (ep ? ' · 第 ' + (ep.i + 1) + ' 集' : ''));
  const dot = $('#dot');
  dot.className = 'dot ' + (!s.wifi ? 'bad' : s.ps === 2 ? 'ok' : (s.ps === 1 || s.ps === 3) ? 'warn' : s.ps === 4 ? 'bad' : '');
  $('#tipTx').textContent = !s.wifi ? '机器断网了，去设置里看看'
    : s.ps === 2 ? nm : psLabel(s) + (nm ? ' · ' + nm : '');
  renderEp(s);
  $('#lat').textContent = NET.lat ? NET.lat + 'ms' : '';

  // 播放键。
  //  1.41.0 时收音模式下它是「静音」开关；1.43.0 静音功能删了，芯片又没有「暂停」
  //  ⇒ 这个键在收音模式下没有任何语义，直接**藏起来**。
  //    留一个点了没反应的键，比没有这个键更糟：老人会以为机器坏了。
  const playing = s.ps === 2 || s.ps === 3;
  const pi = $('#playIc').querySelector('use');
  pi.setAttribute('href', playing ? '#i-pause' : '#i-play');
  $('#btnPlay').classList.toggle('paused', !playing);
  $('#btnPlay').classList.toggle('hide', fmOn);
  // ⚠️ 这里是 querySelector，必须带 # —— 写成 $('btnPrev') 会拿到 null，
  //   然后 && 短路，按钮文案永远不改（断言一跑就露）。
  const lblPrev = $('#btnPrev') && $('#btnPrev').querySelector('span');
  const lblNext = $('#btnNext') && $('#btnNext').querySelector('span');
  if (lblPrev) lblPrev.textContent = fmOn ? '上一个台' : '上一台';
  if (lblNext) lblNext.textContent = fmOn ? '下一个台' : '下一台';
  $('#btnPlay').setAttribute('aria-label', '播放或暂停');

  // 音量
  // 1.41.0：量程跟固件走。1.39.0 起音量是 3..10 档，滑条原来写死 max=100，
  //   拖到中间就被固件夹回 10，数字会突然跳回 —— 看着像「调不动」。
  const v = s.vol;
  const vs = $('#vol');
  setVolRange(vs, s.volmin, s.volmax);
  if (document.activeElement !== vs) vs.value = v;
  // 填充比例要按量程算，不能直接拿档位当百分比：
  // 3..10 档时「5」只占 29%，写 5% 会让滑条看着几乎没有
  vs.style.setProperty('--p', pctOf(vs, v) + '%');
  $('#volNum').textContent = s.mute ? '静音' : s.vol;

  // 1.41.0：首页那个配网按钮。没网时闪琥珀 + 红点 —— 老人也能一眼看出「机器没连上」。
  const wb = $('#btnWifiHome');
  if (wb) wb.classList.toggle('bad', !s.wifi);

  // 1.41.0：控制面板收起时，按钮副标题就是唯一的状态显示
  const cs = $('#ctlSub');
  if (cs){
    const parts = [];
    // 台名太长会顶掉后面，先量一下长度再决定要不要带
    // 收音模式下 s.name 还是网络台那个名字，写上去会让人以为在听网络台
    const nmShort = !fmOn && (s.name || '').length <= 10 ? s.name : '';
    if (nmShort) parts.push(nmShort);
    parts.push('音量 ' + s.vol);      // 1.43.0：静音功能已删，不再有「静音」这一档
    parts.push(fmOn ? fmTx : (playing ? '正在放' : '已暂停'));
    cs.textContent = parts.join(' · ');
  }

  // 1.15.0：没有分组了，不再需要「组变了就重拉」
}

/* ---------------- 有声书集控件 ---------------- */
function renderEp(s){
  const bar = $('#epBar');
  if (!bar) return;
  const ep = epInfo(s);
  bar.classList.toggle('hide', !ep);
  if (!ep) return;
  $('#epNow').textContent = '第 ' + (ep.i + 1) + ' 集';
  $('#epOf').textContent = '共 ' + ep.n + ' 集';
  const tt = $('#epTitle');
  tt.textContent = ep.t || '';
  tt.style.display = ep.t ? '' : 'none';
}

// 选集：一本书可能上千集（官场现形记 1167 集），整份 JSON 三十多 KB，
// 跨网那条 MQTT 通道一次送不完 —— 所以机器按**字节预算**切页（EPIS_PAGE_BYTE），
// 这里一页页翻。别再想着「一次拉全」：那会把云那边的 16KB 缓冲撑爆。
const _EP = { from: 0, n: 0, tot: 0, cur: -1, prog: '', sid: -1 };

function epRow(o, cur){
  const t = o.t || ('第 ' + (o.i + 1) + ' 集');
  return '<div class="row' + (o.i === cur ? ' cur' : '') + '" data-e="' + o.i + '">' +
    '<span class="ix' + (o.i === cur ? ' on' : '') + '">' + (o.i + 1) + '</span>' +
    '<div class="nm"><div class="t">' + esc(t) + '</div>' +
      '<div class="m"><span>第 ' + (o.i + 1) + ' 集</span>' +
      (o.i === cur ? '<span>上次听到这儿</span>' : '') + '</div></div>' +
  '</div>';
}

// 取一页：成功返回这一页的 list（同时写进 _EP），不是书返回 null
// 1.46.33：带上台号 —— 弹层可以给「不是正在播」的那一台打开，
//   不用先让机器切过去（那样一打开选集，正在听的就断了）。
async function epPage(from, sid){
  const body = { from: from | 0 };
  if (sid != null && sid >= 0) body.id = sid;
  const d = await post('/api/epis', body);
  const list = (d && d.list) || [];
  if (!d || !d.n || !list.length) return null;
  _EP.tot = d.n | 0;
  _EP.cur = (typeof d.cur === 'number') ? d.cur : -1;
  _EP.prog = d.prog || '';
  _EP.from = (typeof d.from === 'number') ? d.from : (from | 0);
  _EP.n = list.length;
  _EP.sid = (sid != null && sid >= 0) ? sid : -1;
  return list;
}

function epRangeText(){
  return '第 ' + (_EP.from + 1) + '–' + (_EP.from + _EP.n) + ' 集 · 共 ' + _EP.tot + ' 集';
}

async function openEpisodes(stIdx){
  const sid = (typeof stIdx === 'number' && stIdx >= 0) ? stIdx : -1;
  let list = null;
  try { list = await epPage(0, sid); } catch (e) {}
  if (!list){ toast('这一台不是有声书', true); return; }
  // 上次听到的那集不在第一页里 → 直接翻到它那一页，不然打开弹层看不见「上次听到这儿」
  if (_EP.cur >= _EP.n){
    try { const l2 = await epPage(Math.max(0, _EP.cur - 2), sid); if (l2) list = l2; } catch (e) {}
  }

  openSheet(
    '<h3>' + esc(_EP.prog || '选集') + '</h3>' +
    '<p class="desc">共 ' + _EP.tot + ' 集。点某一集就换到那一集，这个弹层不会关；播完会自动接下一集。</p>' +
    '<div class="eppage">' +
      '<button class="btn ghost sm" id="epgPrev">上一段</button>' +
      '<span id="epRange"></span>' +
      '<button class="btn ghost sm" id="epgNext">下一段</button>' +
    '</div>' +
    '<div class="list" id="epList"></div>',
    sheet => {
      const box = $('#epList', sheet);
      const prev = $('#epgPrev', sheet), next = $('#epgNext', sheet);

      const paint = () => {
        box.innerHTML = list.map(o => epRow(o, _EP.cur)).join('');
        $('#epRange', sheet).textContent = epRangeText();
        prev.disabled = _EP.from <= 0;
        next.disabled = _EP.from + _EP.n >= _EP.tot;
        const c = $('.row.cur', box);
        if (c) setTimeout(() => c.scrollIntoView({ block: 'center' }), 110);
      };

      const go = async from => {
        if (from < 0) from = 0;
        try {
          const l = await epPage(from, sid);
          if (l){ list = l; paint(); }
          else toast('翻到头了', true);
        } catch (e) { toast('没取到，再点一次', true); }
      };

      paint();
      prev.onclick = () => { if (_EP.from > 0) go(Math.max(0, _EP.from - _EP.n)); };
      next.onclick = () => go(_EP.from + _EP.n);

      // 行是随时换掉的（翻页会整体重画），所以点事件挂在容器上，不在每行上
      // 1.46.33：点集数**不关弹层**（用户要「留在选集页面」），
      //   也不做任何事直到真的点了某一集 —— 打开弹层/翻页都不碰播放。
      //   点的就是当前这一集时直接忽略：没意义，还白搭一次断流重连。
      box.onclick = async e => {
        const r = e.target.closest ? e.target.closest('.row[data-e]') : null;
        if (!r) return;
        const en = +r.dataset.e;
        if (en === _EP.cur){ toast('正在播这一集'); return; }
        const body = { e: en };
        if (sid >= 0) body.id = sid;
        try {
          await postOk('/api/epis', body);
          _EP.cur = en;
          paint();                       // 高亮跟着走，页面留着
          toast('从第 ' + (en + 1) + ' 集开始');
          pollScreen();
        } catch (err) { toast('没切成', true); }
      };
    }
  );
}

/* ---------------- 今日收听小卡 ---------------- */
async function refreshToday(){
  try{
    const h = await get('/api/history');
    S.hist = h;
    $('#todayNum').innerHTML = fmtShort(h.today || 0);
    $('#todayPlays').textContent = h.plays ? '开过 ' + h.plays + ' 次' : '';
    const last = (h.list || [])[0];
    $('#lastOne').textContent = last
      ? ('上次听：' + last.n + ' · ' + (last.g || '未打标签') + ' · ' + fmtRel(last.last))
      : '还没有记录，听一会儿就有了';
  }catch (e){}
}

/* ---------------- 操作 ---------------- */
let _actLock = 0;
async function act(path, body, msg){
  const now = Date.now();
  if (now - _actLock < 220 && !body) return;   // 防连点
  _actLock = now;
  try{
    await post(path, body);
    if (msg) toast(msg);
    await pollScreen();
  }catch (e){
    toast('没成功，检查一下连接', true);
  }
}

async function doVol(v){
  const vs = $('#vol');
  // 1.44.0：夹在滑条自己的量程里。原来写死 0..100 —— 新刻度下按 +5 两下就能
  //   把值顶到 30，固件那边只会夹回来，App 与机器上的数字就对不上了。
  const mn = +vs.min, mx = +vs.max;
  v = Math.max(mn, Math.min(mx, Math.round(v)));
  vs.value = v; vs.style.setProperty('--p', pctOf(vs, v) + '%');
  $('#volNum').textContent = v;
  try { await post('/api/vol', { v }); } catch (e) {}
}

let _volT = 0;
function volSoon(v){
  clearTimeout(_volT);
  _volT = setTimeout(() => doVol(v), 140);
}

/* ---------------- 离线态 ---------------- */
// 连不上收音机时不拦路：顶上挂条子，告诉用户点哪儿能修
function setOffline(on){
  S.offline = !!on;
  const bar = $('#offBar');
  if (bar) bar.hidden = !S.offline;
  document.body.classList.toggle('off', S.offline);
  if (S.offline){
    const tx = $('#offTx');
    // 1.46.33：把"连不上"拆成两句人话 ——
    //   收音机自己没网（它连不上 WiFi）跟"手机够不到它"是两件事，
    //   以前一律写「没连上收音机」，用户不知道是机器的事还是手机的事。
    const noWifi = !!(S.scr && S.scr.wifi === 0);
    if (tx) tx.textContent = window.MR_REMOTE
      ? (CLOUD.S.conn && !CLOUD.S.dev ? '先选一台机器（设置 → 我的机器）'
                                      : '没连上收音机（远程）')
      : (noWifi ? '收音机没连上 WiFi' : '手机连不上收音机');
    const sub = $('#hdrSub');
    if (sub && S.page === 'home') sub.textContent = '点这条 → 换 WiFi / 重新连接';
  }
}
function focusWifiCard(){
  const card = $('#wifiCard');
  if (!card) return;
  card.classList.add('hot');
  setTimeout(() => card.classList.remove('hot'), 1600);
  card.scrollIntoView({ block: 'center', behavior: 'smooth' });
}

/* ---------------- 导航 ---------------- */
function go(page){
  if (page === S.page) return;
  closeSheet(true);
  S.page = page;
  $$('.page').forEach(p => p.classList.toggle('on', p.dataset.page === page));
  $$('.tab').forEach(t => t.classList.toggle('on', t.dataset.tab === page));
  window.scrollTo({ top: 0, behavior: 'auto' });
  if (page === 'stations') loadStations();
  if (page === 'history') loadHistory();
  if (page === 'diag') loadDiag();
  if (page === 'settings') loadSettings();
}

/* ---------------- 轮询 ---------------- */
let _pollT = 0, _polling = false;

async function pollScreen(){
  if (_polling) return;
  _polling = true;
  const t0 = performance.now();
  try{
    const s = await get('/api/screen');
    NET.lat = Math.round(performance.now() - t0);
    NET.fails = 0;
    if (S.offline) setOffline(false);      // 恢复了就把条子摘掉
    S.scr = s;
    drawScreen(s);
    $('#liveTx').classList.remove('lost');
    $('#liveTx').textContent = '实时';
    if (S.page === 'home') updateHome(s);
  }catch (e){
    NET.fails++;
    if (NET.fails >= 2){
      $('#liveTx').classList.add('lost');
      $('#liveTx').textContent = '断开';
    }
    if (NET.fails >= 3 && !S.offline) setOffline(true);   // 连续失败 3 次才判离线，避免闪
  }finally{
    _polling = false;
  }
}

// 1.46.51：电量单独轮询 —— 用户报「App 的电量和收音机不同步」。
//   原因：电量只写在设置页，而设置页只在**进来那一刻**读一次 /api/state，
//   之后一直挂着不动（机器从 96% 掉到 90%，App 还写着 96%）。
//   这里单独 20 秒问一次 /api/batt（比 /api/state 轻），只在设置页可见时问。
let _battT = 0;
function startBattPoll(){
  if (_battT) clearInterval(_battT);
  _battT = setInterval(async () => {
    if (document.hidden) return;
    if (S.page !== 'settings') return;
    const row = $('#battRow');
    if (!row || row.style.display === 'none') return;   // 没接电量检测就别去问
    try{ renderBatt(await get('/api/batt')); }catch (e){}
  }, 20000);
}
function stopBattPoll(){ if (_battT) clearInterval(_battT); _battT = 0; }

function startPoll(){
  stopPoll();
  _pollT = setInterval(() => {
    if (document.hidden) return;
    pollScreen();
  }, useCloud() ? 1600 : 900);   // 跨网走中转，1.6 秒一次够用也省流量
  startBattPoll();
}
function stopPoll(){ if (_pollT) clearInterval(_pollT); _pollT = 0; }

/* ---------------- 启动 ---------------- */
async function bootApp(){
  fitCanvas();
  window.addEventListener('resize', () => { fitCanvas(); if (S.scr) drawScreen(S.scr); });

  CLOUD.load();                       // 1.8.0：跨网用的遥控码与服务器

  let online = false;
  if (window.MR_REMOTE){
    // 远程版：页面根本不在收音机那个网里，没有局域网可试，直接连云端。
    // 1.19.0：没配对也要连 —— 先连上才能听到设备广播、才知道家里有哪几台机器。
    CLOUD.enable(true);
    online = await new Promise(res => {
      if (CLOUD.active()) return res(true);
      const t0 = Date.now();
      const iv = setInterval(() => {
        if (CLOUD.active()){ clearInterval(iv); res(true); }
        else if (Date.now() - t0 > 7000){ clearInterval(iv); res(false); }
      }, 200);
    });
    if (!online) S.offline = true;
  } else {
    try{
      // 1.38.0：先把「这台机器在哪」确定下来，再认证。
      //   原来是直接 fetch('/api/auth') 赌同源 —— 收音机换到路由器后面就必然失败，
      //   用户只能自己去设置页手输 IP，也就是他说的「额外链接」。
      if (!NET.base){
        const j = await findRadio();
        if (j) console.log('[MyRadio] 已找到机器 ' + (j.devid || '') + ' @ ' + (NET.base || '同源'));
      }
      await authOnce();
      online = true;
    }catch (e){
      if (e.locked){ pairGate(); return; }
      // 连不上也放行：先进界面，再去设置页配网。不再用遮罩把人挡在门外
      S.offline = true;
    }
  }

  if (online){
    try{
      await pollScreen();
      await refreshStations();
    }catch (e){ S.offline = true; }
  }

  $('#boot').classList.add('gone');
  // 过渡结束后彻底摘掉，免得某些浏览器/截图工具停在半透明状态压着界面
  setTimeout(() => { const b = $('#boot'); if (b && b.classList.contains('gone')) b.style.display = 'none'; }, 420);
  INIT.forEach(f => { try { f(); } catch (e) {} });
  startPoll();
  refreshToday();
  setInterval(refreshToday, 300000);   // 5 分钟刷一次"今天听了多久"

  if (S.offline){
    setOffline(true);                  // 顶上挂条子
    go('settings');                    // 直接落到设置页，那儿能配网
    setTimeout(focusWifiCard, 120);
  } else {
    // 方便直接打开某一页：?page=stations / history / diag / settings
    const want = new URLSearchParams(location.search).get('page');
    if (want && $('.page[data-page="' + want + '"]')) setTimeout(() => go(want), 60);
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden){ pollScreen(); if (S.page === 'diag') loadDiag(); }
  });
}

/* 事件绑定（不依赖接口的部分） */
function bindCore(){
  // 1.38.2：电量行/校准按钮
  const bRow = $('#battRow');
  if (bRow) bRow.onclick = () => openBattCal();
  const bCal = $('#btnBattCal');
  if (bCal) bCal.onclick = () => openBattCal();
  $$('.tab').forEach(t => t.onclick = () => go(t.dataset.tab));

  // 离线条上的「去配网」：切到设置页并把配网卡顶到眼前
  const offGo = $('#offGo');
  if (offGo) offGo.onclick = () => { go('settings'); setTimeout(focusWifiCard, 80); };

  $('#btnPrev').onclick = () => act('/api/step', { dir: -1 });
  $('#btnNext').onclick = () => act('/api/step', { dir: 1 });
  // 有声书：上一集 / 下一集 / 选集（循环，两端都能点）
  const ePrev = $('#epPrev'), eNext = $('#epNext'), eOpen = $('#epOpen');
  if (ePrev) ePrev.onclick = () => act('/api/epis', { dir: -1 });
  if (eNext) eNext.onclick = () => act('/api/epis', { dir: 1 });
  if (eOpen) eOpen.onclick = openEpisodes;
  $('#btnPlay').onclick = () => act('/api/pause');
  // 1.44.0：兜底值改成网络那一路的默认（原厂 netvolume 18），不再是 45
  $('#btnVolDown').onclick = () => doVol((S.scr ? S.scr.vol : 18) - 5);
  $('#btnVolUp').onclick = () => doVol((S.scr ? S.scr.vol : 18) + 5);
  $('#btnHist').onclick = () => go('history');
  // 1.41.0：首页右上角的 WiFi 按钮 —— 直接弹配网面板，不用先切到设置页
  const wbh = $('#btnWifiHome');
  if (wbh) wbh.onclick = () => openWifiSheet();

  // 1.41.0：调台/暂停/音量收在「控制收音机」按钮里，点一下才展开。
  //   展开状态存 localStorage —— 子女用顺手了下次进来还是开着的；
  //   老人机上从来不点，就一直是收起的干净首屏。
  bindCtl();

  const vs = $('#vol');
  vs.addEventListener('input', () => {
    const v = +vs.value;
    vs.style.setProperty('--p', pctOf(vs, v) + '%');
    $('#volNum').textContent = v;
    volSoon(v);
  });
}

/* ---------------- 1.41.0 音量量程 ---------------- */
// 一个滑块里某个值占多少百分比（给 CSS 的 --p / --fill 用）
function pctOf(el, v){
  const a = +el.min, b = +el.max;
  if (!(b > a)) return 0;
  return Math.max(0, Math.min(100, Math.round((v - a) / (b - a) * 100)));
}

// 固件 /api/state 会带 volmin / volmax（网络那一路）/ fvolmin / fvolmax（收音那一路）。
// 1.44.0 起两路量程不同（6..21 / 6..63），调用方必须传对应的那一对。
// 老固件没有这些字段时兜底成网络那一路的 6..21。
// 只在值真的变了才写 DOM —— 每 0.9 秒一次的轮询里改 attr 会让滑条抖。
function setVolRange(el, mn, mx){
  if (!el) return;
  const a = mn != null ? +mn : 6, b = mx != null ? +mx : 21;
  if (+el.min !== a) el.min = a;
  if (+el.max !== b) el.max = b;
  return { mn: a, mx: b };
}

/* ---------------- 1.41.0 控制面板 ---------------- */
function ctlOpen(on){
  const p = $('#ctlPanel'), b = $('#btnCtl');
  if (!p || !b) return;
  p.classList.toggle('hide', !on);
  b.setAttribute('aria-expanded', on ? 'true' : 'false');
  try{ localStorage.setItem('mr.ctl', on ? '1' : '0'); }catch (e){}
}

function bindCtl(){
  const b = $('#btnCtl');
  if (!b) return;
  b.onclick = () => ctlOpen($('#ctlPanel').classList.contains('hide'));
  let on = false;
  try{ on = localStorage.getItem('mr.ctl') === '1'; }catch (e){}
  ctlOpen(on);
}

document.addEventListener('DOMContentLoaded', () => {
  bindCore();
  bootApp();
});
