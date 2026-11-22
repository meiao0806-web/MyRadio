# -*- coding: utf-8 -*-
"""生成 README 用的全部图片 → docs/img/*.png

原则：截的都是**真实那一份**界面 ——
  · 手机 App（五个页签）：从 src/webapp.h 抠 HTML，注入 mock 后端（复用 verify_appctl 的 MOCK），
    只把数据换成「像样的一台机器」，代码一行不改；
  · 配网页：android/app/src/main/assets/home.html（APK 里的那份）；
  · 机器屏：preview/index.html 里的 320×170 画布，按 canvas 的 getBoundingClientRect 精确裁出来；
  · 首页大图：上面几张图 + CSS 拼版，再截一次。

用法： python tools/gen_readme_shots.py [home stations history diag settings setup screen hero]
"""
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "img")

spec = importlib.util.spec_from_file_location("v", os.path.join(HERE, "verify_appctl.py"))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)

CH = v.CHROME
NO_ANIM = "<style>*{transition:none!important;animation:none!important}</style>"

# ---- 把 mock 数据换成「一台正在放中国之声的机器」 ------------------------
FIXTURE = r"""
<script>
(function(){
  var T = Math.floor(Date.now() / 1000);
DATA['/api/history'] = { today: 5460, total: 138600, plays: 12,
  days: [ {d:'2026-11-22',s:5460,p:12}, {d:'2026-11-21',s:7320,p:9},
          {d:'2026-11-20',s:4980,p:7}, {d:'2026-11-19',s:8640,p:11},
          {d:'2026-11-18',s:3240,p:5}, {d:'2026-11-17',s:6180,p:8},
          {d:'2026-11-16',s:2100,p:4} ],
  list: [ {n:'中国之声',g:'新闻',last:T-120,s:5460,p:12},
          {n:'单田芳·三侠五义',g:'评书',last:T-900,s:4320,p:9},
          {n:'音乐之声',g:'音乐',last:T-7200,s:1980,p:6},
          {n:'经典 947',g:'音乐',last:T-86400,s:1260,p:4} ] };
DATA['/api/health'] = { s: '11111111', f: '00000000', c: { bad: 0, dim: 0 },
  cur: { i: 0, lv: 58, st: 1 } };
DATA['/api/diag'] = { fw:'1.46.66', up: 93784, heap: 121000, fsTotal: 2097152, fsUsed: 62915,
  wifi: { ok:1, ssid:'ChinaNet-3F2A', ip:'192.168.1.23', rssi:-55, ch:6 },
  time: { ok: 1 }, reset: '上电',
  audio: { ps:2, kbps: 64 }, table: { total: PRESET_N, count: PRESET_N, tags: TAG_N } };
DATA['/api/state'] = Object.assign({}, DATA['/api/state'], {
  batt: 85, bmv: 4033, chg: 0, fw:'1.46.66', count:PRESET_N, gcount:TAG_N,
  name:'中国之声', vol:18, mode:'net', ip:'192.168.1.23' });
DATA['/api/screen'] = Object.assign({}, DATA['/api/screen'], {
  batt: 85, bmv: 4033, name:'中国之声', vol:18, mode:'net', fw:'1.46.66',
  count:PRESET_N, gcount:TAG_N, ip:'192.168.1.23', clock:'12:24', rssi:-55, idx:0 });
})();
</script>
"""


def stations_js():
    """台表直接从 src/preset_stations.h 生成 —— 截出来的台名 / 台数就是固件里那份。"""
    s = io.open(os.path.join(ROOT, "src", "preset_stations.h"), encoding="utf-8").read()
    pairs = re.findall(r'\{\s*"([^"]*)",\s*"([^"]*)"\s*\}', s)
    tags = re.findall(r'\{\s*"([^"]+)",\s*(\d+)\s*\}', s[s.find("kPresetTag[]"):])
    gidx = [int(x) for x in re.findall(r"\d+", s[s.find("kPresetTagOf[]"):].split("};")[0])]
    st = [{"n": n, "u": u, "g": gidx[k] if k < len(gidx) else 0} for k, (u, n) in enumerate(pairs)]
    tg = [{"i": k, "n": n, "c": int(c)} for k, (n, c) in enumerate(tags)]
    js = "var PRESET_N=%d, TAG_N=%d; DATA['/api/stations']={cur:0, tags:%s, list:%s};" % (
        len(st), len(tg), json.dumps(tg, ensure_ascii=False), json.dumps(st, ensure_ascii=False))
    return js, len(st), len(tg)


PAGES = [("app-home", "home"), ("app-stations", "stations"), ("app-history", "history"),
         ("app-diag", "diag"), ("app-settings", "settings")]


def _open(src_path, js="", budget=7000, tail_ms=700):
    """读 html，关掉过渡动画、追加一段 load 后执行的 js，写到临时文件，返回 file:// URL。"""
    s = io.open(src_path, encoding="utf-8").read()
    # 注意：有的页面内嵌了第二份文档（远程版/预览页），必须注入到**最后一个** </head>
    a, b = s.rsplit("</head>", 1)
    s = a + NO_ANIM + "</head>" + b
    if js:
        a, b = s.rsplit("</body>", 1)
        s = a + ("<script>window.addEventListener('load',function(){setTimeout(function(){%s},%d);});"
                 "</script></body>" % (js, tail_ms)) + b
    p = os.path.join(tempfile.gettempdir(), "mrs_" + os.path.basename(src_path) + ".shot.html")
    io.open(p, "w", encoding="utf-8").write(s)
    return "file:///" + p.replace("\\", "/")


def _run(url, w, h, budget, scale, extra):
    return [CH, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
            "--user-data-dir=" + os.path.join(tempfile.gettempdir(), "mrs_prof"),
            "--window-size=%d,%d" % (w, h), "--allow-file-access-from-files",
            "--virtual-time-budget=%d" % budget, "--hide-scrollbars",
            "--force-device-scale-factor=%d" % scale] + extra + [url]


def shoot(src_path, out, w, h, js="", budget=7000, scale=2, tail_ms=700):
    url = _open(src_path, js, budget, tail_ms)
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(_run(url, w, h, budget, scale, ["--screenshot=" + out]), capture_output=True)
    ok = os.path.exists(out)
    print("%-20s %s %s" % (os.path.basename(out), "ok" if ok else "MISSING",
                           ("%dKB" % (os.path.getsize(out) // 1024)) if ok else ""))
    return ok


def app_page(name, page, h=1000):
    """手机 App 的一页：webapp.h 里的真页面 + mock 数据。"""
    html0 = v.extract()
    i = html0.find("<script>")
    sj, _, _ = stations_js()
    base = html0[:i] + v.MOCK + "<script>" + sj + "</script>" + FIXTURE + html0[i:]
    tmp = os.path.join(tempfile.gettempdir(), "mr_app_" + page + ".html")
    io.open(tmp, "w", encoding="utf-8").write(base)
    shoot(tmp, os.path.join(OUT, name + ".png"), 420, h, "go('%s')" % page)


def device_screen():
    """机器屏特写：坐标由页面自己量（--window-size 给的宽度比真实视口大 16px，手量必歪）。"""
    src = os.path.join(ROOT, "preview", "index.html")
    js = ("var c=document.getElementById('screen'),r=c.getBoundingClientRect();"
          "var d=document.createElement('div');d.id='__rect';d.textContent=JSON.stringify("
          "[r.left,r.top,r.width,r.height,document.documentElement.clientWidth]);"
          "document.body.appendChild(d);")
    dom = subprocess.run(_run(_open(src, js, 5000, 600), 1280, 860, 5000, 2, ["--dump-dom"]),
                         capture_output=True).stdout.decode("utf-8", "replace")
    m = re.search(r'id="__rect">\[([\d.,]+)\]<', dom)
    if not m:
        sys.exit("!! 没从预览页读回屏幕坐标")
    left, top, w, h, vw = [float(x) for x in m.group(1).split(",")]
    tmp = os.path.join(tempfile.gettempdir(), "mrs_screen.png")
    if os.path.exists(tmp):
        os.remove(tmp)
    subprocess.run(_run(_open(src, "", 5000), 1280, 860, 5000, 2, ["--screenshot=" + tmp]),
                   capture_output=True)
    im = Image.open(tmp)
    k = im.size[0] / vw                      # 截图 px / CSS px
    box = (int(left * k), int(top * k), int((left + w) * k), int((top + h) * k))
    im.crop(box).save(os.path.join(OUT, "device-screen.png"))
    print("%-20s ok %s" % ("device-screen.png", im.crop(box).size))


HERO = """<!doctype html><html><head><meta charset="utf-8"><style>
*{margin:0;padding:0;box-sizing:border-box}
body{width:1600px;height:700px;overflow:hidden;
  font:400 14px/1.5 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
  background:
    radial-gradient(880px 520px at 6% -10%, #ffffff 0%, rgba(255,255,255,0) 60%),
    radial-gradient(720px 480px at 98% 96%, #dbe3ef 0%, rgba(219,227,239,0) 62%),
    linear-gradient(158deg,#f5f7fb 0%, #e8ecf3 58%, #e1e7f0 100%);
  display:flex;align-items:center;gap:44px;padding:0 60px}
.dev{flex:0 0 auto;width:552px}
.dev .frame{background:linear-gradient(180deg,#161d27,#0a0f15);border-radius:20px;padding:14px;
  box-shadow:0 28px 54px -20px rgba(16,26,42,.5)}
.dev img{display:block;width:100%;border-radius:6px}
.dev .cap{margin-top:12px;text-align:center;color:#5b6675;font-size:13px;letter-spacing:.4px}
.dev .cap b{color:#2f3a48;font-weight:600}
.phones{display:flex;align-items:center;gap:22px}
.ph{border-radius:26px;overflow:hidden;background:#e6e9ef;
  box-shadow:0 24px 40px -18px rgba(16,26,42,.38), 0 0 0 6px #f8fafc, 0 0 0 7px #d2d8e2}
.ph img{display:block;width:100%}
.s{width:242px}
.m{width:264px;margin-top:-16px;margin-bottom:-16px}
</style></head><body>
<div class="dev">
  <div class="frame"><img src="IMG/device-screen.png"></div>
  <div class="cap">机器本体：<b>320 × 170</b> 彩屏 + 一个旋钮，没有菜单树</div>
</div>
<div class="phones">
  <div class="ph s"><img src="IMG/app-stations.png"></div>
  <div class="ph m"><img src="IMG/app-home.png"></div>
  <div class="ph s"><img src="IMG/app-history.png"></div>
</div>
</body></html>
"""


def hero():
    p = os.path.join(tempfile.gettempdir(), "mr_hero.html")
    base = "file:///" + OUT.replace("\\", "/") + "/"
    io.open(p, "w", encoding="utf-8").write(HERO.replace("IMG/", base))
    out = os.path.join(OUT, "hero.png")
    if os.path.exists(out):
        os.remove(out)
    subprocess.run(_run("file:///" + p.replace("\\", "/"), 1600, 760, 4000, 1,
                        ["--screenshot=" + out]), capture_output=True)
    print("%-20s %s" % ("hero.png", "ok" if os.path.exists(out) else "MISSING"))


if __name__ == "__main__":
    want = sys.argv[1:] or [p for _, p in PAGES] + ["setup", "screen", "hero"]
    for name, page in PAGES:
        if page in want:
            app_page(name, page)
    if "setup" in want:
        shoot(os.path.join(ROOT, "android/app/src/main/assets/home.html"),
              os.path.join(OUT, "app-setup.png"), 420, 1000)
    if "screen" in want:
        device_screen()
    if "hero" in want:
        hero()
