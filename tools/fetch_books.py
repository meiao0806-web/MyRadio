#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
抓喜马拉雅专辑 RSS -> tools/有声书源.json

用法：
    python tools/fetch_books.py              # 按 tools/有声书.json 里的清单抓（全量）
    python tools/fetch_books.py --only=三国演义
    python tools/fetch_books.py --check      # 只验能不能播，不写文件
    python tools/fetch_books.py --full       # 连「本来只取前 N 集」的书也抓全量

为什么要 RSS：喜马拉雅专辑页要登录、要会员，但**开了 RSS 输出的专辑**，
`https://www.ximalaya.com/album/<id>.xml` 里每条 <enclosure> 就是一条 m4a 直链，
裸请求（无 Referer / 无 Cookie）返回 206 audio/mp4 —— ESP32 能直接播。
实测 2022 年发布的链接到 2026 年还有效，比抓专辑页的临时地址稳。

1.18.0 起：
    · `count: 0`（或不写）= **整本抓完**，不再默认只取 15 集。
      之前 `count: 15` 是手工写的上限，导致三国演义 / 官场现形记 / 上下五千年 /
      每晚一个历史故事 在 App 里都只显示 15 集，翻不到后面。
    · `rev: true` = 把 RSS 顺序反过来。喜马有些专辑的 RSS 是**倒序**（最新一集在最前），
      正着取的话「第 1 集」其实是中间某一集，得反转才是真正的第 1 集。
      判据很简单：看最后一条是不是「引子 / 短序 / 第 1 集」。
    · 每本书抓完都打印首末两条，方便一眼看出顺序对不对。
"""
import json
import os
import re
import sys
import ssl
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "有声书.json")
OUT = os.path.join(HERE, "有声书源.json")
UA = {"User-Agent": "Mozilla/5.0"}

CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE
PH = {k: v for k, v in (("http", os.environ.get("http_proxy", "")),
      ("https", os.environ.get("https_proxy", ""))) if v}
OP = urllib.request.build_opener(urllib.request.ProxyHandler(PH),
                                 urllib.request.HTTPSHandler(context=CTX))


def get(url, timeout=90):
    req = urllib.request.Request(url, headers=UA)
    with OP.open(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def probe(url):
    """裸请求探一下；返回 True 表示 ESP32 那边也能播"""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0",
                                                  "Range": "bytes=0-1023"})
        with OP.open(req, timeout=20) as r:
            return r.status in (200, 206) and "audio" in (r.headers.get("Content-Type") or "")
    except Exception:
        return False


def arg(k):
    for a in sys.argv[1:]:
        if a == "--" + k:
            return True
        if a.startswith("--" + k + "="):
            return a.split("=", 1)[1]
    return None


def main():
    check_only = bool(arg("check"))
    force_full = bool(arg("full"))
    only = arg("only")

    albums = json.load(open(LIST, encoding="utf-8"))
    out = []

    for a in albums:
        name = a["name"]
        if only and only != name:
            continue
        try:
            xml = get(a["feed"])
        except Exception as e:
            print("  [跳过] %-12s RSS 打不开：%s" % (name, e))
            continue

        items = re.findall(r"<item>(.*?)</item>", xml, re.S)
        picked = []
        for it in items:
            t = re.search(r"<title><!\[CDATA\[(.*?)\]\]></title>", it, re.S)
            enc = re.search(r'<enclosure[^>]*?url="([^"]+)"', it, re.S)
            if not (t and enc):
                continue
            picked.append({"title": t.group(1).strip(),
                           "url": enc.group(1).replace("&amp;", "&")})

        skip = a.get("skip", 0)
        picked = picked[skip:]
        if a.get("rev"):
            picked = picked[::-1]

        # count 缺省 / 0 → 全量；--full 强制全量
        n = int(a.get("count") or 0)
        if n > 0 and not force_full:
            picked = picked[:n]
        if not picked:
            print("  [跳过] %-12s RSS 里没解析到条目（共 %d 条）" % (name, len(items)))
            continue

        longest = max(len(p["url"]) for p in picked)
        if check_only:
            head = picked[:2] + picked[-1:]
            ok = sum(1 for p in head if probe(p["url"]))
            print("  %-12s %4d 集，抽 3 集可播 %d/3，最长地址 %d 字节"
                  % (name, len(picked), ok, longest))
            continue

        out.append({"group": a["group"], "name": name, "items": picked})
        print("  %-12s %4d 集（RSS 共 %d 条，skip %d%s）最长地址 %d 字节"
              % (name, len(picked), len(items), skip,
                 "，已反转" if a.get("rev") else "", longest))
        print("        首：%s" % picked[0]["title"][:46])
        print("        末：%s" % picked[-1]["title"][:46])
        time.sleep(0.5)

    if check_only:
        return

    # --only 时只重抓那一本，别的书要保留（否则文件里只剩一本）
    if only:
        try:
            keep = [b for b in json.load(open(OUT, encoding="utf-8"))
                    if b["name"] != only]
        except Exception:
            keep = []
        out = keep + out

    json.dump(out, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    total = sum(len(x["items"]) for x in out)
    print("写出 %s：%d 部书 / %d 集" % (OUT, len(out), total))


if __name__ == "__main__":
    main()
