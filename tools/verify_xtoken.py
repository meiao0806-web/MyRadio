# -*- coding: utf-8 -*-
"""
verify_xtoken.py —— 「输 1234 连上了、进去却显示没连上」这条 bug 的回归测试

现象（用户报）：
    App 输入 1234 → 显示「正在连接」→ 进了首页 → 首页顶上写「没连上收音机」，
    设置页也连不上；按系统返回键又跳回输码那一页。

真凶：
    ESP32 的 WebServer **只解析 collectHeaders() 里登记过的请求头**。
    网页（core.js 的 getLan/postLan）一直用 X-Token 头带配对令牌，可固件从来没登记过，
    于是 server.header("X-Token") 恒为空串 → authed() 只剩 ?t= → 所有 /api/* 全 401。
    页面连续 3 次失败就挂「没连上收音机」，但人还留在首页。

本测试不烧板子：起一个本地代理，把页面发给浏览器、把 /api/* 转给真实收音机，
并且可以「故意把头丢掉」（= 老固件的 collectHeaders 缺失）。四种组合：

    A. 旧页面 + 头被丢   → 必须复现「断开」（证明诊断没错）
    B. 新页面 + 头被丢   → 必须正常（?t= 兜底生效）
    C. 新页面 + 头正常   → 必须正常（正常路径）
    D. 直连设备发 X-Token → 顺便报告当前固件是哪一种

用法：
    python tools/verify_xtoken.py
真实收音机不在线时 A/B/C 会跳过（打印 SKIP），不会假装通过。
"""
import io
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV = "10.240.152.129"
CHROME = r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"

PAGE = ""          # 当前要发的页面（切换 old/new）
DROP_HDR = False   # True = 假装固件没登记 X-Token（原样复现老固件）

_ok = 0
_bad = []
_skip = []


def check(cond, msg):
    global _ok
    if cond:
        _ok += 1
        print("  OK   " + msg)
    else:
        _bad.append(msg)
        print("  FAIL " + msg)


def skip(msg):
    _skip.append(msg)
    print("  SKIP " + msg)


# ---------------------------------------------------------------- 页面来源
def page_from_webapp():
    s = io.open(os.path.join(ROOT, "src", "webapp.h"), encoding="utf-8").read()
    m = re.search(r'R"HTMLX\((.*)\)HTMLX";', s, re.S)
    assert m, "webapp.h 里找不到 R\"HTMLX(...)\"HTMLX"
    return m.group(1)


OLD_GET = """async function getLan(path){
  if (!NET.tk) await authOnce();
  const url = hostOf(path);
  let r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' });
  if (r.status === 401) { await authOnce(); r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' }); }
  if (!r.ok) throw new Error('http ' + r.status);
  return r.json();
}"""


def make_old_page(new_page):
    """把新页面的 getLan 换回 1.46.18 的写法（只会用 X-Token 头）"""
    i = new_page.find("function tokQ(url){")
    assert i > 0, "新页面里找不到 tokQ（gen_webapp.py 没跑？）"
    j = new_page.find("async function postLan(path, body){", i)
    assert j > 0
    k = new_page.find("\nasync function get(path){", j)
    assert k > 0
    return new_page[:i] + OLD_GET + "\n" + new_page[k + 1:]


# ---------------------------------------------------------------- 代理
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _proxy(self, method):
        url = "http://%s%s" % (DEV, self.path)
        data = None
        if method == "POST":
            n = int(self.headers.get("Content-Length") or 0)
            data = self.rfile.read(n) if n else b""
        req = urllib.request.Request(url, data=data, method=method)
        ct = self.headers.get("Content-Type")
        if ct:
            req.add_header("Content-Type", ct)
        if not DROP_HDR:
            tk = self.headers.get("X-Token")
            if tk:
                req.add_header("X-Token", tk)
        try:
            op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            r = op.open(req, timeout=8)
            self._send(r.status, r.read())
        except urllib.error.HTTPError as e:
            self._send(e.code, e.read())
        except Exception as e:
            self._send(502, '{"err":"%s"}' % e)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif self.path.startswith("/api/"):
            self._proxy("GET")
        else:
            self._send(404, "{}")

    def do_POST(self):
        if self.path.startswith("/api/"):
            self._proxy("POST")
        else:
            self._send(404, "{}")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "X-Token,Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()


# ---------------------------------------------------------------- 跑浏览器
def render(port):
    prof = os.path.join(tempfile.gettempdir(), "mrxtok%d" % port)
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
           "--no-proxy-server", "--user-data-dir=" + prof,
           "--window-size=420,900",
           "--virtual-time-budget=12000", "--dump-dom",
           "http://127.0.0.1:%d/" % port]
    env = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
        env.pop(k, None)
    p = subprocess.run(cmd, capture_output=True, timeout=120, env=env)
    return p.stdout.decode("utf-8", errors="replace")


def status_of(dom):
    """从渲染后的 DOM 里读「实时/断开」和顶部离线条是否露出"""
    m = re.search(r'<span class="([^"]*)" id="liveTx">([^<]*)</span>', dom)
    lost = m and "lost" in m.group(1)
    text = (m.group(2) if m else "").strip()
    # 离线条：<div class="offbar" id="offBar"> 没 hidden 就是露着的
    bar = re.search(r'<div class="offbar" id="offBar"([^>]*)>', dom)
    barVis = bool(bar) and "hidden" not in bar.group(1)
    return text, bool(lost), barVis


def run_case(name, page, drop, port):
    global PAGE, DROP_HDR
    PAGE, DROP_HDR = page, drop
    dom = render(port)
    tx, lost, bar = status_of(dom)
    online = (tx == "实时") and not lost
    print("[%s] liveTx=%r lost=%s offBar=%s" % (name, tx, lost, bar))
    return online


def main():
    new_page = page_from_webapp()
    old_page = make_old_page(new_page)

    # 先看真实设备现在认不认 X-Token
    dev_online = False
    try:
        op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        info = op.open("http://%s/api/info" % DEV, timeout=4).read().decode()
        dev_online = True
        auth = op.open("http://%s/api/auth" % DEV, timeout=4).read().decode()
        tok = re.search(r'"token":"([^"]+)"', auth).group(1)
        req = urllib.request.Request("http://%s/api/screen" % DEV, headers={"X-Token": tok})
        try:
            op.open(req, timeout=4)
            hdr_ok = True
        except urllib.error.HTTPError as e:
            hdr_ok = (e.code != 401)
        fw = re.search(r'"fw":"([^"]+)"', info)
        print("[设备] %s  X-Token 头 = %s" % (fw.group(1) if fw else "?", "认" if hdr_ok else "401（未登记）"))
    except Exception as e:
        print("[设备] 离线：%s" % e)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    print("\n== A 旧页面 + 头被丢（应该复现「没连上」）==")
    if dev_online:
        a_online = run_case("A", old_page, True, port)
        check(not a_online, "旧页面在未登记 X-Token 的固件上确实连不上（复现用户现象）")
    else:
        skip("A（设备不在线）")

    print("\n== B 新页面 + 头被丢（?t= 兜底应该救回来）==")
    if dev_online:
        b_online = run_case("B", new_page, True, port)
        check(b_online, "新页面在未登记 X-Token 的固件上也能连上（兜底生效）")
    else:
        skip("B（设备不在线）")

    print("\n== C 新页面 + 头正常（正常路径）==")
    if dev_online:
        c_online = run_case("C", new_page, False, port)
        check(c_online, "X-Token 头正常时页面实时刷新")
    else:
        skip("C（设备不在线）")

    print("\n== D 固件源码检查 ==")
    net = io.open(os.path.join(ROOT, "src", "net.cpp"), encoding="utf-8").read()
    check("collectHeaders" in net and '"X-Token"' in net,
          "net.cpp 里登记了 X-Token 头（server.collectHeaders）")
    core = io.open(os.path.join(ROOT, "tools", "app", "js", "core.js"), encoding="utf-8").read()
    check("tokQ(" in core and "NET.qTok" in core,
          "core.js 有 ?t= 兜底（头被丢时不至于整页瞎掉）")
    web = io.open(os.path.join(ROOT, "src", "webapp.h"), encoding="utf-8").read()
    check("NET.qTok" in web, "webapp.h 已经用新 core.js 重新生成过")

    print("\n通过 %d / 失败 %d / 跳过 %d" % (_ok, len(_bad), len(_skip)))
    if _bad:
        for b in _bad:
            print("  ✗ " + b)
        sys.exit(1)
    if _skip:
        print("  注意：%d 条没设备验（需要收音机在线）" % len(_skip))


if __name__ == "__main__":
    main()
