# -*- coding: utf-8 -*-
"""生成「App 配网」可点预览页（不需要安卓手机，浏览器里点一遍就能看流程）。

做法：拿 android/assets/home.html 原样，前面注入一份**假的 MRApp 桥**，
     后面不动。于是所有按钮都真能点，只是「连热点/发密码」被换成了
     定时器 + 假回调 —— 版式和文案 100% 就是 APK 里那份。

产物：
  preview/app-setup.html            项目内预览
  MyRadio-App配网预览.html           根目录，直接发人
"""
import io, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "android", "app", "src", "main", "assets", "home.html")

MOCK = r"""
<script>
/* ===== 预览用假桥：让浏览器里也能把「一键配网」整条流程点完 ===== */
var NETS = [
  { s: 'TP-LINK_5G',      r: -48, sec: 1 },
  { s: 'ChinaNet-3F2A',   r: -61, sec: 1 },
  { s: 'Xiaomi_AX3000',   r: -72, sec: 1 },
  { s: 'CMCC-8kQz',       r: -86, sec: 0 },
  { s: 'HUAWEI-9A2C',     r: -78, sec: 1 }
];
window.__previewNote = '';
function previewToast(t){
  window.__previewNote = t;
  var d = document.getElementById('pvtoast');
  if (!d) { d = document.createElement('div'); d.id = 'pvtoast';
    d.style.cssText = 'position:fixed;left:12px;right:12px;top:12px;z-index:200;' +
      'background:#3c414b;color:#fff;font:12.5px/1.5 sans-serif;padding:10px 13px;' +
      'border-radius:12px;text-align:center;opacity:.95';
    document.body.appendChild(d); }
  d.textContent = t;
  clearTimeout(window.__pvT);
  window.__pvT = setTimeout(function(){ if (d.parentNode) d.parentNode.removeChild(d); }, 4200);
}
window.MRApp = {
  ver: function () { return 'preview'; },
  scan: function () { setTimeout(function () {
      window.__mrDev && window.__mrDev('192.168.1.23', 'MyRadio-A1B2C3', 'MyRadio 1.40.2', 'mdns');
      window.__mrDone && window.__mrDone(1);
    }, 900); },
  recent: function () { return '[]'; },
  open: function (ip) { previewToast('真机上这里会打开 http://' + ip + '/ 的遥控界面'); },
  openRemote: function () { previewToast('真机上这里会打开离线跨网遥控页'); },
  forget: function () {},
  apInfo: function () {
    // 1.46.8：热点改名「我的收音机」、去掉密码（开放热点）
    return JSON.stringify({ ssid: '我的收音机', pass: '', ip: '192.168.4.1', sdk: 34 });
  },
  provJoin: function () {
    window.__mrProv && window.__mrProv({ stage: 'joining' });
    setTimeout(function () {
      window.__mrProv && window.__mrProv({ stage: 'ready', name: 'MyRadio',
        fw: 'MyRadio 1.40.2', homeSsid: 'ChinaNet-3F2A', apip: '192.168.4.1', nets: NETS });
    }, 1800);
  },
  provRescan: function () {
    setTimeout(function () { window.__mrProv && window.__mrProv({ stage: 'nets', nets: NETS }); }, 1100);
  },
  provSend: function (s) {
    setTimeout(function () { window.__mrProv && window.__mrProv({ stage: 'sending', ssid: s }); }, 250);
    setTimeout(function () {
      if (/5G/i.test(s)) {
        window.__mrProv && window.__mrProv({ stage: 'fail',
          msg: '连不上 ' + s + '，密码或信号有问题（收音机只认 2.4G）' });
      } else {
        window.__mrProv && window.__mrProv({ stage: 'ok', ssid: s, ip: '192.168.1.47' });
      }
    }, 3200);
  },
  provCancel: function () {},
  provDirect: function () {
    previewToast('真机上这里会直接打开 http://192.168.4.1 的遥控界面 —— 收音机的热点一直开着');
  },
  provFinish: function (ip) {
    previewToast('真机上：App 放掉热点 → 等手机切回家里 WiFi → 自动打开 http://' + ip + '/');
  }
};
</script>
"""

TIPS = r"""
<div id="tipbar" style="position:fixed;left:0;right:0;bottom:0;z-index:99;
  background:#335d69;color:#fff;font:12px/1.5 -apple-system,'PingFang SC',sans-serif;
  padding:8px 14px calc(8px + env(safe-area-inset-bottom));text-align:center;opacity:.94">
  这是 <b>App 首页的可点预览</b>（不是真机）。点「一键配网」走一遍：<br>
  连收音机热点 → 填/选 WiFi 名字 → 输密码 → 完成。选带 5G 的名字会演示失败那条路。
</div>
<style>body{padding-bottom:64px !important}</style>
"""

html = io.open(SRC, encoding="utf-8").read()

anchor = "<script>\n(function () {"
if anchor not in html:
    raise SystemExit("!! 注入点没命中")
html = html.replace(anchor, MOCK + anchor, 1)
html = html.replace("</body>", TIPS + "\n</body>", 1)
html = html.replace("<title>MyRadio</title>", "<title>MyRadio · App 配网预览</title>", 1)

outs = [os.path.join(ROOT, "preview", "app-setup.html"),
        os.path.join(ROOT, "MyRadio-App配网预览.html")]
for p in outs:
    os.makedirs(os.path.dirname(p), exist_ok=True)
    io.open(p, "w", encoding="utf-8").write(html)
    print("%-62s %d B" % (p, len(html.encode("utf-8"))))
