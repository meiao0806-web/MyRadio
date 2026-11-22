#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_webapp.py —— 把 tools/app/ 拆开的源文件拼成 src/webapp.h

为什么要拆：
  手机 App 现在有 5 个页签、一块实时屏幕镜像、拖拽排序、体检修复，
  合起来两千多行。全塞进一个 .h 里没人维护得动。

产物：
  src/webapp.h   单文件 HTML（设备端 send_P 直接发，零外链、零 CDN）

用法：
  python tools/gen_webapp.py
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "tools", "app")
ICONS = os.path.join(ROOT, "tools", "icons", "lucide.json")
OUT = os.path.join(ROOT, "src", "webapp.h")

JS_ORDER = ["cloud.js", "core.js", "stations.js", "pages.js", "health.js"]


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def sprite():
    """把抓下来的 Lucide 图标做成 <symbol>，页面里用 <use href="#i-play"> 引用。
    图标来自 lucide-static（MIT），下载后内联，设备离线也能显示。"""
    icons = json.load(open(ICONS, encoding="utf-8"))
    parts = []
    for name in sorted(icons):
        inner = icons[name].strip()
        parts.append(
            '<symbol id="i-%s" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
            'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">%s</symbol>'
            % (name, inner)
        )
    return ('<svg xmlns="http://www.w3.org/2000/svg" style="display:none" aria-hidden="true">'
            + "".join(parts) + "</svg>")


def squeeze(html):
    """不删换行（JS 的 // 注释依赖它），只去行首尾空白和空行，
    再把 HTML 注释去掉。约省 15%。"""
    html = re.sub(r"<!--(?!\[if).*?-->", "", html, flags=re.S)
    lines = [ln.strip() for ln in html.splitlines()]
    return "\n".join(ln for ln in lines if ln)


def main():
    tpl = read(os.path.join(APP, "base.html"))
    css = read(os.path.join(APP, "style.css"))
    body = read(os.path.join(APP, "body.html"))

    js_parts = []
    for f in JS_ORDER:
        p = os.path.join(APP, "js", f)
        js_parts.append("/* ==== %s ==== */\n%s" % (f, read(p)))

    html = (tpl
            .replace("/*__CSS__*/", css)
            .replace("<!--__BODY__-->", body.replace("<!--__SPRITE__-->", sprite()))
            .replace("/*__JS__*/", "\n;\n".join(js_parts)))

    # C++ raw string 里不能出现结束标记
    assert ')HTMLX"' not in html, "HTML 里出现了 R\"HTMLX 的结束标记"

    html = squeeze(html)

    hdr = (
        "// ============================================================\n"
        "//  webapp.h —— 手机 App（单文件网页，由设备自己发出）\n"
        "//\n"
        "//  !! 这个文件是生成的，不要手改 !!\n"
        "//  改 tools/app/ 下的 base.html / style.css / body.html / js/*.js\n"
        "//  然后跑： python tools/gen_webapp.py\n"
        "//\n"
        "//  五个页签：收音机（实时屏幕镜像 + 控制）/ 电台 / 记录 / 体检 / 设置\n"
        "//  图标来自 lucide-static（MIT），构建时内联成 <symbol>，不依赖任何 CDN。\n"
        "// ============================================================\n"
        "#pragma once\n\n"
    )
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(hdr)
        f.write('static const char WEBAPP_HTML[] = R"HTMLX(')
        f.write(html)
        f.write(')HTMLX";\n')

    kb = os.path.getsize(OUT) / 1024
    print("写出 %s  (%.1f KB)" % (OUT, kb))
    print("  css %d B / body %d B / js %d B / 图标 %d 个"
          % (len(css), len(body), sum(len(x) for x in js_parts), 58))


if __name__ == "__main__":
    main()
