#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_remote.py —— 导出「远程版」单文件网页

为什么需要它：
  平时用的那个 App 是**收音机自己发出来的**，所以只有在同一个 WiFi 里才打得开。
  人在外地时根本连页面都没有。这个脚本把同一套界面导出成一个独立 HTML：
  存手机里（微信收藏 / 文件）、或者挂到任意静态托管上，随时打开都能用。

它和局域网版只差三件事：
  1. window.MR_REMOTE = true     → 所有请求走云端（MQTT），不走 fetch
  2. 第一次打开时弹一扇门：**输收音机那 4 位连接码（出厂 1234）就连**；
     没带码的同码多台会列出来让你挑；实在找不到才退回手输 12 位遥控码（1.46.7 起）
  3. 标题加「远程」

为什么第 2 条要改（1.19.1）：
  1.19.0 的机器已经会往 myradio/dev/+ 广播「我在线」了，可那扇门还只认手抄的
  12 位码 —— 换一台手机就得重新抄一遍，和「换手机也能用」自相矛盾。
  现在门里先连服务器、听广播、把机器列出来，点一下就选好了（免配对的机器
  连码都不用输，直接由设备码算出来）。

用法：
  python tools/gen_remote.py
产物：
  MyRadio-远程版.html     （工程根目录，直接发给用户）
  preview/remote.html     （同一份，方便本地预览）
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBAPP = os.path.join(ROOT, "src", "webapp.h")
OUT1 = os.path.join(ROOT, "MyRadio-远程版.html")
OUT2 = os.path.join(ROOT, "preview", "remote.html")

HEAD_FLAG = '<script>window.MR_REMOTE = true;</script>\n'

GATE_CSS = """<style>
/* 新拟物：和 App 同一套底色、同一对双向阴影 */
#rmGate{position:fixed;inset:0;z-index:9999;background:#e6e9ef;display:flex;
  align-items:center;justify-content:center;padding:20px;box-sizing:border-box;
  overflow:auto}
#rmGate[hidden]{display:none}
.rmbox{width:100%;max-width:344px;background:#e6e9ef;border-radius:30px;
  padding:26px 22px;box-sizing:border-box;
  box-shadow:8px 8px 18px #c3c8d3, -8px -8px 18px #ffffff}
.rmt{font-size:18px;font-weight:800;color:#3c414b;margin-bottom:6px;
  text-align:center;letter-spacing:-.2px}
.rmd{font-size:13px;line-height:1.7;color:#5c636f;margin-bottom:14px;text-align:center}
/* 1.46.7：4 格码 —— 一个 input 放大字距，模拟四格，比四个 input 好处理粘贴/退格 */
  /* ⚠️ 不要用 text-indent 去补偿 letter-spacing：Chrome 的 ::placeholder
     不认自带的 text-indent:0（继承外层的值），文字会被顶偏 14px。
     letter-spacing 最多让 4 位数字整体偏左 7px —— 62px 高的框里看不出来。 */
  #rmPin{width:100%;height:62px;border:0;border-radius:20px;background:#e6e9ef;
  text-align:center;font-size:30px;font-weight:800;letter-spacing:14px;
  outline:none;box-sizing:border-box;color:#3c414b;font-variant-numeric:tabular-nums;
  box-shadow:inset 4px 4px 9px #c3c8d3, inset -4px -4px 9px #ffffff}
#rmPin::placeholder{color:#b9c0cc;font-size:19px;letter-spacing:0;font-weight:600}
#rmList{max-height:34vh;overflow:auto;margin:12px 0 0;-webkit-overflow-scrolling:touch}
.rmdev{display:block;width:100%;text-align:left;border:0;border-radius:18px;
  background:#e6e9ef;padding:12px 15px;margin-bottom:10px;cursor:pointer;
  box-shadow:4px 4px 9px #c3c8d3, -4px -4px 9px #ffffff}
.rmdev:last-child{margin-bottom:0}
.rmdev:active,.rmdev.on{box-shadow:inset 3px 3px 7px #c3c8d3, inset -3px -3px 7px #ffffff}
.rmn{display:block;font-size:15.5px;font-weight:800;color:#3c414b}
.rms{display:block;font-size:12px;color:#8d95a3;margin-top:3px;
  font-variant-numeric:tabular-nums}
.rmwait{font-size:13px;color:#5c636f;line-height:1.75;padding:14px 2px;text-align:center}
.rmtip{margin-top:12px;font-size:12.5px;color:#8a5a30;text-align:center;
  min-height:18px;line-height:1.5}
/* 遥控码手输：收进小折叠里，只有「机器关了免配对」或「固件太老」时才需要 */
#rmMore{margin-top:14px;font-size:12px;color:#8d95a3}
#rmMore summary{cursor:pointer;text-align:center;outline:none;list-style:none}
#rmMore summary::-webkit-details-marker{display:none}
#rmIn{width:100%;height:50px;margin-top:10px;border:0;border-radius:16px;
  background:#e6e9ef;text-align:center;font-size:16px;font-weight:700;
  letter-spacing:1.5px;outline:none;text-transform:uppercase;box-sizing:border-box;
  color:#3c414b;box-shadow:inset 4px 4px 9px #c3c8d3, inset -4px -4px 9px #ffffff}
#rmGo{width:100%;height:46px;margin-top:10px;border:0;border-radius:999px;
  background:#3f7180;color:#fff;font-size:15px;font-weight:800;
  box-shadow:5px 5px 11px #c3c8d3, -5px -5px 11px #ffffff}
#rmGo:active{background:#335d69;box-shadow:inset 3px 3px 8px rgba(0,0,0,.22)}
</style>
"""

GATE_HTML = """<div id="rmGate" hidden>
  <div class="rmbox">
    <div class="rmt">在外地控制家里的收音机</div>
    <div class="rmd">输入收音机那 <b>4 位连接码</b>（出厂 <b>1234</b>），
      连上就能用整套界面。</div>
    <input id="rmPin" inputmode="numeric" maxlength="4" autocomplete="off"
           spellcheck="false" placeholder="4 位连接码">
    <div id="rmList"></div>
    <div class="rmtip" id="rmTip"></div>
    <details id="rmMore">
      <summary>找不到？用 12 位遥控码直接连</summary>
      <input id="rmIn" placeholder="A1B2C3-482913" autocapitalize="characters"
             autocomplete="off" spellcheck="false">
      <button id="rmGo">连上收音机</button>
    </details>
  </div>
</div>
"""

GATE_JS = """<script>
(function(){
  CLOUD.load();
  var g = document.getElementById('rmGate');
  if (!g) return;
  var pinI = document.getElementById('rmPin');
  var inp  = document.getElementById('rmIn');
  var tip  = document.getElementById('rmTip');
  var list = document.getElementById('rmList');

  // 已经配过（本机存着设备码+遥控码）：这扇门不该挡路
  if (CLOUD.paired()) { g.hidden = true; if (CLOUD.S.want) CLOUD.connect(); return; }

  g.hidden = false;
  // 1.46.7：一进来就连服务器 —— 机器每 30 秒报一次到，早连一秒早看见它
  CLOUD.enable(true);

  var t0 = Date.now();
  var picked = '';          // 已经选定了某台，正在连（此时冻结列表，别重绘）
  var busy = false;

  function esc(s){
    return String(s == null ? '' : s).replace(/[&<>"]/g, function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];
    });
  }
  function alive(){
    return (CLOUD.S.devs || []).filter(function(d){ return CLOUD.devAlive(d); });
  }
  function say(msg, kind){
    tip.style.color = kind === 'ok' ? '#4a8a63' : (kind === 'plain' ? '#5c636f' : '#8a5a30');
    tip.innerHTML = msg;
  }
  function devBtn(d, note){
    var s = ' · ' + ((d.batt >= 0) ? (d.batt + '%') : '电量未知');
    if (note) s += ' · ' + note;
    return '<button class="rmdev" data-dev="' + esc(d.dev) + '">' +
           '<span class="rmn">' + esc(d.n) + '</span>' +
           '<span class="rms">' + esc(s) + '</span></button>';
  }

  // 什么都没输的时候：把在线的机器列出来（多数就是它，点一下也成）
  function idleRender(){
    if (picked || busy) return;
    var ds = alive();
    if (!ds.length){
      var waiting = (Date.now() - t0) < 12000;
      list.innerHTML = waiting ? '' : '';
      say(waiting
        ? '正在等家里的机器报到…<br><span style="color:#8d95a3">机器每 30 秒报一次，等几秒</span>'
        : '还没收到任何机器 —— 确认它开着、连着网。<br>也可以展开下面用 12 位遥控码直连。',
        waiting ? 'plain' : 'warn');
      return;
    }
    say('选一台，或者在上面输它的 4 位码', 'plain');
    var h = ds.map(function(d){
      return devBtn(d, d.pin ? ('码 ' + d.pin) : '固件太老，不报码');
    }).join('');
    if (list.innerHTML !== h) list.innerHTML = h;
  }

  function waitUp(dev){
    var t = Date.now();
    var iv = setInterval(function(){
      if (CLOUD.S.dev === dev && CLOUD.active()){
        clearInterval(iv);
        location.reload();            // 连上了，重进就是能用整套界面的远程版
      } else if (Date.now() - t > 12000){
        clearInterval(iv);
        picked = ''; busy = false; idleRender();
        say('连不上服务器 —— 看看手机有没有网', 'warn');
      }
    }, 300);
  }

  function connectDev(dev, name){
    var auto = CLOUD.pickDev(dev);     // 免配对的机器顺手把算出来的码填好
    if (!auto){
      picked = ''; busy = false;
      say('这台关了「免配对」，没法自动连。<br>展开下面，输入它的 12 位遥控码。', 'warn');
      var d = document.getElementById('rmMore'); if (d) d.open = true;
      if (inp) { inp.value = dev; if (inp.focus) inp.focus(); }
      return;
    }
    picked = dev;
    say('正在连 ' + esc(name || dev) + '…', 'ok');
    CLOUD.enable(true);
    waitUp(dev);
  }

  /* 1.46.7：输满 4 位就查。
     机器那 4 位码跟着在线广播一起发过来，所以「查」实际是本地筛：
       1 台 → 直接连（这就是用户要的手感）
       多台 → 列出来让人挑（都叫 1234 时不能替人决定）
       0 台 → 说清是「没上线」还是「固件太老不报码」 */
  function tryPin(){
    var v = String(pinI.value || '').replace(/\\D/g, '').slice(0, 4);
    if (v.length !== 4) return;
    busy = true;
    var r = CLOUD.pinLogin(v);
    if (r.n === 1){
      list.innerHTML = '';
      connectDev(r.dev, r.name);
      return;
    }
    busy = false;
    if (r.n > 1){
      say('有 <b>' + r.n + '</b> 台机器都用 ' + v + ' —— 点一台', 'plain');
      list.innerHTML = r.list.map(function(d){ return devBtn(d, ''); }).join('');
      return;
    }
    // 0 台：分清两种情况，别让用户白等
    if (!CLOUD.anyPin()){
      say('没找到用 ' + v + ' 的机器。<br><span style="color:#8d95a3">这些机器是 1.46.7 之前的老固件，' +
          '不会报连接码 —— 下面挑一台，或展开输 12 位遥控码。</span>', 'warn');
    } else {
      say('没找到用 ' + v + ' 的机器。<br><span style="color:#8d95a3">确认码没打错、机器开着。' +
          '下面是现在在线的机器。</span>', 'warn');
    }
    var ds = alive();
    list.innerHTML = ds.map(function(d){ return devBtn(d, d.pin ? ('码 ' + d.pin) : ''); }).join('');
  }

  // 自己轮询：等广播陆续到齐（第一台可能 1 秒后才到）
  var iv = setInterval(function(){ if (!picked && !pinI.value) idleRender(); }, 500);
  idleRender();

  pinI.addEventListener('input', function(){
    var v = this.value.replace(/\\D/g, '').slice(0, 4);
    if (v !== this.value) this.value = v;
    if (v.length === 4) setTimeout(tryPin, 120);   // 让第 4 个字先画出来
  });
  pinI.addEventListener('keydown', function(e){ if (e.key === 'Enter') tryPin(); });

  list.addEventListener('click', function(e){
    var b = (e.target && e.target.closest) ? e.target.closest('.rmdev') : null;
    if (!b || picked) return;
    clearInterval(iv);
    b.className = 'rmdev on';
    connectDev(b.getAttribute('data-dev'), b.querySelector('.rmn').textContent);
  });

  document.getElementById('rmGo').onclick = function(){
    if (!CLOUD.setPair(inp.value)){
      say('要 12 位：前 6 位机器码 + 后 6 位密码', 'warn');
      return;
    }
    clearInterval(iv);
    picked = CLOUD.S.dev;
    say('正在连…', 'ok');
    CLOUD.save({ want: true });
    CLOUD.enable(true);
    waitUp(CLOUD.S.dev);
  };

  inp.addEventListener('keydown', function(e){
    if (e.key === 'Enter') document.getElementById('rmGo').click();
  });
})();
</script>
"""


def main():
    s = open(WEBAPP, encoding="utf-8").read()
    m = re.search(r'R"HTMLX\((.*)\)HTMLX"', s, re.S)
    if not m:
        raise SystemExit("webapp.h 里找不到 HTML")
    html = m.group(1)

    assert '<title>' in html
    # 1.46.8：App 改名「操控收音机」。这里**别钉死标题原文** —— 之前写死
    #   "<title>我的收音机</title>"，改名后 replace 静默不生效（"标题远程 False"
    #   就是它），远程版标题会变成没有「· 远程」的普通名，两版看起来一模一样。
    #   用正则取现有标题再拼后缀，改名也不会再踩。
    m_t = re.search(r'<title>([^<]*)</title>', html)
    assert m_t, 'webapp 里没有 <title>'
    html = html.replace(m_t.group(0),
                        '<title>%s · 远程</title>' % m_t.group(1), 1)
    assert "MR_REMOTE = true" not in html

    # 1) 标记：必须在 cloud.js/core.js 之前
    html = html.replace("<style>", HEAD_FLAG + "<style>", 1)

    # 2) 开门那扇窗 + 启动逻辑：放最后，脚本里的 CLOUD / devAlive 已经定义好了
    assert html.count("</body>") == 1
    html = html.replace("</body>", GATE_CSS + GATE_HTML + GATE_JS + "</body>")

    with open(OUT1, "w", encoding="utf-8", newline="\n") as f:
        f.write("<!doctype html>\n" + html if not html.lstrip().lower().startswith("<!doctype") else html)
    os.makedirs(os.path.dirname(OUT2), exist_ok=True)
    with open(OUT2, "w", encoding="utf-8", newline="\n") as f:
        f.write(open(OUT1, encoding="utf-8").read())

    kb = os.path.getsize(OUT1) / 1024
    print("写出 %s  (%.1f KB)" % (OUT1, kb))
    print("     %s" % OUT2)
    print("  标记 %s / 小窗 %s / 发现列表 %s / 标题远程 %s"
          % ("MR_REMOTE" in html, "rmGate" in html, "rmList" in html, "· 远程" in html))


if __name__ == "__main__":
    main()
