#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
抓蜻蜓 FM 全部地区电台 -> qtfm_all.json

用法：
    python tools/fetch_qtfm.py            # 抓全量，写 tools/qtfm_all.json
    python tools/fetch_qtfm.py --check    # 只统计不写文件

接口（公开，无需鉴权）：
    GET https://rapi.qingting.fm/regions                        -> [{id,title}]
    GET https://rapi.qingting.fm/categories/<id>/channels?page=N&pagesize=100
注意：pagesize 上限 100，必须翻页；返回空数组即到底。
"""
import json
import os
import sys
import time
import urllib.request

BASE = "https://rapi.qingting.fm"
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "qtfm_all.json")


def get(url, tries=3):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except Exception as e:                      # 网络抖动重试
            last = e
            time.sleep(0.8 * (i + 1))
    raise last


def fetch_region(cid):
    """翻页拿一个地区的全部台"""
    out, page = [], 1
    while page <= 30:
        d = get(f"{BASE}/categories/{cid}/channels?page={page}&pagesize=100")
        rows = d.get("Data") or []
        if not rows:
            break
        for it in rows:
            if it.get("content_type") != "channel":
                continue
            out.append({"id": it.get("content_id"), "name": (it.get("title") or "").strip()})
        page += 1
        time.sleep(0.25)
    return out


def main():
    regions = get(f"{BASE}/regions")["Data"]
    print(f"地区 {len(regions)} 个")

    data, total = [], 0
    for i, rg in enumerate(regions, 1):
        try:
            chs = fetch_region(rg["id"])
        except Exception as e:
            print(f"  [跳过] {rg['title']} ({rg['id']}): {e}")
            continue
        # 同一地区内按 id 去重，保留首次出现顺序
        seen, uniq = set(), []
        for c in chs:
            if c["id"] in seen or not c["name"]:
                continue
            seen.add(c["id"])
            uniq.append(c)
        data.append({"region": rg["title"], "rid": rg["id"], "channels": uniq})
        total += len(uniq)
        print(f"  {i:2d}/{len(regions)} {rg['title']:<6} {len(uniq):>3} 台")

    print(f"\n合计 {total} 台 / {len(data)} 个地区")
    if "--check" in sys.argv:
        return
    json.dump(data, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"写出 {OUT}  ({os.path.getsize(OUT)/1024:.0f} KB)")


if __name__ == "__main__":
    main()
