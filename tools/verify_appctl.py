# -*- coding: utf-8 -*-
"""验证 App 页面（src/webapp.h 里那一份）的「控制收音机」折叠 + 新增设置项。

做法：从 webapp.h 抠出 HTML → 注入 mock fetch（按路径返回假数据）→
      注入串行断言驱动器 → headless Chrome --dump-dom → 抠 <pre id="out">。

为什么不用截图：这些都是「点了按钮到底调了哪个接口 / DOM 有没有真的变」，
断言比看图可靠。版式另有 preview/index.html 看。

用法： python tools/verify_appctl.py
"""
import io
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = os.path.join(ROOT, "src", "webapp.h")
CHROME = r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
if not os.path.exists(CHROME):
    CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


def extract():
    s = io.open(H, encoding="utf-8").read()
    m = re.search(r'R"HTMLX\((.*)\)HTMLX";', s, re.S)
    if not m:
        sys.exit("!! webapp.h 里没找到 R\"HTMLX(...)HTMLX\"")
    return m.group(1)


MOCK = r"""
<script>
/* ===== 假后端：把 /api/* 全拦下来，返回固定数据 ===== */
window.__calls = [];
const SCR = {
  cur: 2, name: '中国之声', count: 307, group: 0, gcount: 9, gname: '广播',
  ps: 2, vol: 18, mute: 0, wifi: 1, rssi: -55, mode: 'net',
  fmfreq: 0, fmrssi: 0, khz: 98000, band: 'fm',
  fmscan: 0, fmprog: 0, fmstn: 12, on: 1, bmax: 80, offmin: 3,
  sq: 0, hs: 0, fmvol: 35, volmin: 6, volmax: 21, fvolmin: 6, fvolmax: 63,
  ip: '192.168.1.23', clock: '20:31', fw: '1.41.0',
  bok: 1, batt: 76, bmv: 3900, chg: 0, blow: 0, bpin: 4
};
const STATE = Object.assign({}, SCR);
const DATA = {
  '/api/auth':        { token: 'tkn123', locked: 0 },
  '/api/info':        { fw: 'MyRadio 1.41.0', name: 'MyRadio', devid: 'A1B2C3', needSetup: 0 },
  '/api/screen':      SCR,
  '/api/state':       STATE,
  '/api/stations':    { list: [{ n: '中国之声', u: 'http://x/1.mp3' }, { n: '经济之声', u: 'http://x/2.mp3' }], tags: [] },
  '/api/diag':        { fw: '1.41.0', up: 3600, wifi: { ok: 1, ssid: 'ChinaNet-3F2A', ip: '192.168.1.23', rssi: -55, ch: 6 },
                        audio: { ps: 2 }, heap: 100000, fsTotal: 1000, fsUsed: 100, table: { tags: 9, count: 307 } },
  '/api/pair':        { locked: 0 },
  // 1.42.0：固件恒开 —— mock 也要照真实固件回 on:1。原来 on:0/conn:0 会让
  //   点状态行后的 setTimeout(loadSettings,3000) 在虚拟时间里真的跑掉，
  //   把后面半步进断言的 swHalf 状态洗回 st.hs（0）⇒ 假 FAIL。
  '/api/cloud':       { on: 1, conn: 1, host: 'broker-cn.emqx.io', port: 8084 },
  '/api/wifi/list':   { n: 2, cur: 1, list: [{ i: 0, ssid: 'ChinaNet-3F2A', on: 1 }, { i: 1, ssid: 'TP-LINK_5G', on: 0 }] },
  '/api/wifi/status': { state: 2, ok: 1, ssid: 'ChinaNet-3F2A', ip: '192.168.1.23' },
  '/api/scan':        [{ s: 'TP-LINK_5G', r: -48, e: 4 }, { s: 'ChinaNet-3F2A', r: -61, e: 4 }],
  '/api/history':     { list: [], total: 0 },
  '/api/health':      { list: [] },
  '/api/batt':        { bok: 1, batt: 76, bmv: 3900, chg: 0, blow: 0, bpin: 4 },
  '/api/fmstations':  { n: 3, busy: 0, list: [
      { khz: 98000, rssi: 78, band: 'FM' },
      { khz: 101700, rssi: 55, band: 'FM' },
      { khz: 999, rssi: 31, band: 'AM' }] }
};

function body(path, opt){
  // --dump-dom 里靠这个判断「点了按钮有没有真的打接口」
  window.__calls.push((opt && opt.method === 'POST' ? 'P ' : 'G ') + path +
                      (opt && opt.body ? ' ' + opt.body.toString() : ''));
  const k = path.split('?')[0];
  return DATA[k] !== undefined ? DATA[k] : { ok: 1 };
}

window.fetch = function(url, opt){
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  const j = body(path, opt);
  return Promise.resolve(new Response(JSON.stringify(j), {
    status: 200, headers: { 'Content-Type': 'application/json' }
  }));
};
/* 别真去连 MQTT */
window.WebSocket = function(){ this.close = function(){}; this.send = function(){}; };
/* findRadio 会挨个试 host，直接告诉它「同源就行」 */
window.__noFindRadio = true;
</script>
"""

DRIVER = r"""
<pre id="out" style="pointer-events:none;position:fixed;left:0;bottom:0;
  font:11px monospace;white-space:pre-wrap;max-height:46vh;overflow:auto"></pre>
<script>
(function(){
  var L = [], o = document.getElementById('out');
  function log(s){ L.push(s); o.textContent = L.join('\n'); }
  function ok(n, c, x){ log((c ? 'OK   ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); }
  function $(id){ return document.getElementById(id); }
  function txt(id){ var e = $(id); return e ? e.textContent : '<无此元素>'; }
  function calls(){ return window.__calls || []; }
  function cnt(p){ return calls().filter(function(c){ return c.indexOf(p) >= 0; }).length; }
  function lastPost(p){
    var f = calls().filter(function(c){ return c.indexOf('P ' + p) === 0; });
    return f.length ? f[f.length - 1] : '';
  }
  function vis(id){
    var e = $(id); if (!e) return false;
    var cs = getComputedStyle(e);
    return cs.display !== 'none' && cs.visibility !== 'hidden';
  }

  // 切页用页面自己的 go()
  window.__mrGo = function(p){ try{ go(p); }catch (e){} };

  var steps = [];
  function step(f, d){ steps.push({f: f, d: d || 60}); }
  function run(i){
    if (i >= steps.length){ log('DONE'); return; }
    var s = steps[i];
    try { s.f(); } catch(e){ log('FAIL 步骤 ' + i + ' 抛错: ' + e); }
    setTimeout(function(){ run(i+1); }, s.d);
  }

  // ---------------- 首页：控制收在按钮里 ----------------
  step(function(){
    window.__mrGo && window.__mrGo('home');
    ok('首页有「控制收音机」按钮', !!$('btnCtl'));
    ok('按钮文案就是"控制收音机"', txt('btnCtl').indexOf('控制收音机') >= 0, txt('btnCtl'));
    ok('初始是收起的', $('btnCtl').getAttribute('aria-expanded') === 'false',
       $('btnCtl').getAttribute('aria-expanded'));
    ok('面板真的看不见（不是只加了 class）', !vis('ctlPanel'), getComputedStyle($('ctlPanel')).display);
    ok('上一台/暂停/下一台都在面板里', !!$('btnPrev') && !!$('btnPlay') && !!$('btnNext')
       && $('ctlPanel').contains($('btnPrev')) && $('ctlPanel').contains($('btnPlay')));
    ok('音量滑条也在面板里', !!$('vol') && $('ctlPanel').contains($('vol')));
    ok('音频电平条留在面板外（收起也看得见）', !!$('sndRow') && !$('ctlPanel').contains($('sndRow')));
  });

  step(function(){ $('btnCtl').click(); });
  step(function(){
    ok('点一下展开', $('btnCtl').getAttribute('aria-expanded') === 'true');
    ok('展开后面板可见', vis('ctlPanel'));
    ok('展开状态记进 localStorage', localStorage.getItem('mr.ctl') === '1');
  });

  step(function(){ $('btnCtl').click(); });
  step(function(){
    ok('再点一下收起', $('btnCtl').getAttribute('aria-expanded') === 'false');
    ok('收起后面板不可见', !vis('ctlPanel'));
    ok('收起状态也记下来', localStorage.getItem('mr.ctl') === '0');
  });

  // ---------------- 首页的配网按钮 ----------------
  step(function(){
    ok('首页有独立配网按钮（不是只在设置页）', !!$('btnWifiHome'));
    ok('配网按钮在 header 右侧按钮组里', $('btnWifiHome').parentNode.classList.contains('hdrbtns'));
    ok('静音按钮已删除（1.43.0）', !$('btnMute'));
    ok('有网时不闪红', !$('btnWifiHome').classList.contains('bad'),
       $('btnWifiHome').className);
  });

  step(function(){ $('btnWifiHome').click(); }, 400);
  step(function(){
    var h3 = document.querySelectorAll('#sheetHost h3');
    var t = h3.length ? h3[0].textContent : '';
    ok('点首页配网按钮直接弹出配网面板', /WiFi|配网/.test(t), t || '没弹出来');
    // 关掉弹层，别影响后面的步骤
    try{ closeSheet(); }catch (e){}
  }, 260);

  // ---------------- 远程控制恒开（1.42.0） ----------------
  step(function(){ window.__mrGo && window.__mrGo('settings'); }, 320);
  step(function(){
    ok('设置页有「远程控制」状态行', !!$('btnCloud'));
    ok('那一行不再是开关（没有 .sw 滑块）',
       !$('btnCloud').querySelector('.sw'), $('btnCloud').innerHTML.slice(0,80));
    ok('标题写明「始终开启」', $('btnCloud').textContent.indexOf('始终开启') >= 0,
       $('btnCloud').textContent.trim().slice(0,30));
  });

  step(function(){ $('btnCloud').click(); }, 300);
  step(function(){
    ok('点远程控制行 = 只读状态，绝不 POST /api/cloud（关=锁死机器）',
       cnt('P /api/cloud') === 0, calls().filter(function(c){ return c.indexOf('cloud') >= 0; }).join(' | ') || '没有 cloud 请求');
    ok('状态来自 GET /api/cloud', cnt('G /api/cloud') >= 1);
  });

  // ---------------- 摘要行 ----------------
  step(function(){
    var t = txt('ctlSub');
    ok('摘要里有音量数字', /音量\s*18/.test(t), t);
    ok('摘要里有播放状态', /正在放|已暂停/.test(t), t);
  });

  // ---------------- 设置页：新增项存在 ----------------
  step(function(){ window.__mrGo && window.__mrGo('settings'); }, 320);
  step(function(){
    ok('设置页有「网络电台音量」滑条', !!$('rgVol'));
    ok('设置页有「收音机音量」滑条（原厂 volume）', !!$('rgFmVol'));
    ok('设置页有「静噪门限」滑条（原厂 currentSquelch）', !!$('rgSq'));
    ok('设置页有「半步进」开关（原厂 halfstep）', !!$('btnHalfStep'));
    ok('设置页有「重新搜台」', !!$('btnFmScan'));
    ok('设置页有「已存的 WiFi」', !!$('btnSavedWifi'));
    ok('收音机卡显示组数', txt('savedN').indexOf('2') >= 0, txt('savedN'));
    // 1.44.0：两路量程**不同**（原厂口径）—— 网络 netvolume 6..21、收音 volume 6..63。
    //   共用一对 min/max 是老版本的写法，现在共用会把 FM 那条卡在 21。
    ok('网络音量滑条量程 = 固件的 6..21',
       $('rgVol').min === '6' && $('rgVol').max === '21',
       $('rgVol').min + '..' + $('rgVol').max);
    ok('收音机音量滑条量程 = 固件的 6..63',
       $('rgFmVol').min === '6' && $('rgFmVol').max === '63',
       $('rgFmVol').min + '..' + $('rgFmVol').max);
  });

  // 首页那条音量滑条也走同一套量程
  step(function(){ window.__mrGo && window.__mrGo('home'); }, 320);
  step(function(){
    ok('首页音量滑条量程跟网络那一路一致（6..21）',
       $('vol').min === '6' && $('vol').max === '21',
       $('vol').min + '..' + $('vol').max);
    window.__mrGo && window.__mrGo('settings');
  }, 320);

  // ---------------- 真能改 ----------------
  step(function(){
    // 1.44.0：故意拖到 40 —— 超过网络那一路的上限 21，能发出去才说明
    //   「两条滑条各用各的量程」是真的分开了（共用 max 的话这里会被夹成 21）
    var e = $('rgFmVol'); e.value = 40;
    e.dispatchEvent(new Event('input', { bubbles: true }));
  }, 400);
  step(function(){
    var c = lastPost('/api/vol');
    ok('拖「收音机音量」到 40 → POST /api/vol 带 ch=fm&v=40（不被 21 夹住）',
       c.indexOf('ch=fm') >= 0 && c.indexOf('v=40') >= 0, c);
  });

  step(function(){
    var e = $('rgVol'); e.value = 15;
    e.dispatchEvent(new Event('input', { bubbles: true }));
  }, 400);
  step(function(){
    var c = lastPost('/api/vol');
    ok('拖「网络音量」→ POST /api/vol 带 ch=net&v=15', c.indexOf('ch=net') >= 0 && c.indexOf('v=15') >= 0, c);
  });

  step(function(){
    var e = $('rgSq'); e.value = 40;
    e.dispatchEvent(new Event('input', { bubbles: true }));
  }, 500);
  step(function(){
    var c = lastPost('/api/squelch');
    ok('拖静噪 → POST /api/squelch v=40', c.indexOf('v=40') >= 0, c);
    ok('静噪的当前值就地显示', txt('sqNow').indexOf('40') >= 0, txt('sqNow'));
  });

  step(function(){ $('btnHalfStep').click(); }, 300);
  step(function(){
    var c = lastPost('/api/halfstep');
    ok('点半步进 → POST /api/halfstep on=1', c.indexOf('on=1') >= 0, c);
    ok('开关变成开', $('swHalf').classList.contains('on'));
  });
  step(function(){ $('btnHalfStep').click(); }, 300);
  step(function(){
    var c = lastPost('/api/halfstep');
    ok('再点一次 → on=0', c.indexOf('on=0') >= 0, c);
    ok('开关变回关', !$('swHalf').classList.contains('on'));
  });

  // ---------------- 搜台 ----------------
  step(function(){ $('btnFmScan').click(); }, 260);
  step(function(){
    // 弹确认层，点「开始搜」
    var btns = document.querySelectorAll('.sheet button, #sheetHost button');
    var hit = null;
    for (var i = 0; i < btns.length; i++){
      if (/开始搜/.test(btns[i].textContent)) { hit = btns[i]; break; }
    }
    ok('点重新搜台会先弹确认', !!hit, btns.length + ' 个按钮');
    if (hit) hit.click();
  }, 400);
  step(function(){
    ok('确认后 → POST /api/fmscan', cnt('P /api/fmscan') === 1, calls().join(' | '));
  });

  // ---------------- 已存 WiFi ----------------
  step(function(){ $('btnSavedWifi').click(); }, 400);
  step(function(){
    var box = document.getElementById('swBody');
    ok('弹出「已存的 WiFi」列表', !!box);
    var t = box ? box.textContent : '';
    ok('列出已存的 ChinaNet-3F2A', t.indexOf('ChinaNet-3F2A') >= 0, t.slice(0, 80));
    ok('标出哪一组正在用', t.indexOf('正在用') >= 0, t.slice(0, 80));
    ok('每组都有「忘掉」', box && box.querySelectorAll('[data-ssid]').length === 2,
       box ? box.querySelectorAll('[data-ssid]').length + ' 个' : '无');
  });
  step(function(){
    var b = document.querySelector('[data-ssid]');
    if (b) b.click();
  }, 300);
  step(function(){
    var btns = document.querySelectorAll('#sheetHost button');
    var hit = null;
    for (var i = 0; i < btns.length; i++){
      if (/忘掉/.test(btns[i].textContent) && !btns[i].dataset.ssid) { hit = btns[i]; break; }
    }
    ok('忘掉前要确认', !!hit);
    if (hit) hit.click();
  }, 500);
  step(function(){
    var c = lastPost('/api/wifi/forget');
    ok('确认后 → POST /api/wifi/forget 带 ssid', c.indexOf('ssid=') >= 0, c);
  });

  // ---------------- 收音模式（1.41.0） ----------------
  step(function(){
    // 把假数据切成「机器在收音」，再推一次屏幕
    SCR.mode = 'fm'; SCR.fmfreq = 9840; SCR.khz = 98400; SCR.band = 'fm';
    SCR.name = '中国之声'; SCR.mute = 0; SCR.ps = 2;
    window.__mrGo && window.__mrGo('home');
  }, 400);
  step(function(){
    pollScreen();
  }, 500);
  step(function(){
    ok('收音模式：顶部写波段+频率，不是网络台名',
       txt('tipTx').indexOf('调频 98.4') >= 0, txt('tipTx'));
    ok('收音模式：副标题说「正在收音」', txt('hdrSub').indexOf('正在收音') >= 0, txt('hdrSub'));
    ok('收音模式：控制按钮的摘要也是频率', txt('ctlSub').indexOf('调频 98.4') >= 0, txt('ctlSub'));
    var pv = $('btnPrev').querySelector('span');
    var nx = $('btnNext').querySelector('span');
    ok('收音模式：换台按钮改叫「上一个台/下一个台」',
       pv.textContent === '上一个台' && nx.textContent === '下一个台',
       pv.textContent + '/' + nx.textContent);
    // 1.43.0：静音删了、芯片也没有暂停 ⇒ 这个键在收音模式下必须**藏起来**
    ok('收音模式：中间键被隐藏（没有暂停也没有静音可言）',
       getComputedStyle($('btnPlay')).display === 'none',
       getComputedStyle($('btnPlay')).display);
  });

  step(function(){ $('btnNext').click(); }, 300);
  step(function(){
    ok('收音模式下点「下一个台」也走 /api/step（固件里会转成 fmScanStep）',
       cnt('P /api/step') >= 1, calls().join(' | '));
  });

  step(function(){ $('btnPlay').click(); }, 300);
  step(function(){
    ok('静音功能已删：任何路径都不该再发 /api/mute',
       cnt('P /api/mute') === 0, calls().join(' | '));
  });

  // ---------------- 搜到的台清单 ----------------
  step(function(){ window.__mrGo && window.__mrGo('settings'); }, 320);
  step(function(){ $('btnFmStations').click(); }, 400);
  step(function(){
    var box = document.getElementById('fsBody');
    ok('「搜到的台」能弹出清单', !!box);
    var t = box ? box.textContent : '';
    ok('清单里有调频 98.0 MHz', t.indexOf('98.0 MHz') >= 0, t.slice(0, 60));
    ok('清单里有中波 999 kHz', t.indexOf('999 kHz') >= 0, t.slice(0, 60));
    var rows = box ? box.querySelectorAll('[data-khz]') : [];
    ok('每行都能点（带频率）', rows.length === 3, rows.length + ' 行');
    if (rows.length) rows[1].click();
  }, 400);
  step(function(){
    var c = lastPost('/api/fmtune');
    ok('点一行 → POST /api/fmtune 带 khz', c.indexOf('khz=101700') >= 0, c);
  });

  // 回到网络模式，别影响后面的断言
  step(function(){
    SCR.mode = 'net'; SCR.fmfreq = 0; SCR.name = '中国之声';
    try{ closeSheet(); }catch (e){}
    pollScreen();
  }, 400);

  // ---------------- 老接口没被改坏 ----------------
  step(function(){
    ok('播放/换台接口仍在', !!$('btnPlay') && !!$('btnPrev') && !!$('btnNext'));
    $('btnPlay').click();
  }, 300);
  step(function(){
    ok('点暂停 → POST /api/pause', cnt('P /api/pause') >= 1, calls().join(' | '));
  });

  setTimeout(function(){ run(0); }, 260);
})();
</script>
"""


def main():
    html = extract()
    # mock 必须在页面自己的脚本之前
    i = html.find("<script>")
    if i < 0:
        sys.exit("!! 页面里没有 <script>")
    html = html[:i] + MOCK + html[i:]
    html = html.replace("</body>", DRIVER + "\n</body>", 1)

    tmp = os.path.join(tempfile.gettempdir(), "mr_appctl.html")
    io.open(tmp, "w", encoding="utf-8").write(html)

    prof = os.path.join(tempfile.gettempdir(), "mr_chrome_appctl")
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
           "--user-data-dir=" + prof, "--window-size=420,1000",
           "--allow-file-access-from-files", "--virtual-time-budget=26000",
           "--dump-dom", "file:///" + tmp.replace("\\", "/")]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    dom = r.stdout or ""

    m = re.search(r'<pre id="out"[^>]*>(.*?)</pre>', dom, re.S)
    if not m:
        print("!! 没抓到 #out。dump 前 2500 字：\n", dom[:2500])
        return 1
    body = (m.group(1).replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
                      .replace("&quot;", '"').replace("&#39;", "'"))
    print(body)
    fails = body.count("FAIL ")
    oks = body.count("OK   ")
    print("\n通过 %d / 失败 %d" % (oks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
