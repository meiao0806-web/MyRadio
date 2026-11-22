#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
src/preset_stations.h -> 台表-全国.md + 台表-全国.csv（人看的完整列表）

用法：python tools/gen_stationlist.py

为什么从 preset_stations.h 读、而不是从 qtfm_all.json 读：
  固件实际用的就是 preset_stations.h（人工精选），
  文档跟着它走才不会出现「说明书里 31 个地区、机器上却是另一套台表」这种错位。
  想拿回全量抓取结果：python tools/fetch_qtfm.py && python tools/gen_preset.py

1.15.0 起固件不再分组：**表的先后就是播放顺序**（旋钮从第 1 台转到最后一台），
「标签」只是给手机 App 筛选用的标记，同标签的行可以散在表里任何位置
（有声书均匀穿插在电台之间）。
"""
import csv
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src", "preset_stations.h")
OUT = os.path.join(ROOT, "台表-全国.md")
OUT_CSV = os.path.join(ROOT, "台表-全国.csv")

QT_URL = "http://lhttp.qtfm.cn/live/%s/64k.mp3"


def _arr(s, decl):
    m = re.search(decl + r"\[\] = \{(.*?)\n\};", s, re.S)
    if not m:
        raise SystemExit("preset_stations.h 里找不到 %s[]" % decl)
    return m.group(1)


def load():
    s = open(SRC, encoding="utf-8").read()
    st = _arr(s, "static const PresetSt kPreset")
    lst = [{"i": u, "n": n} for u, n in
           re.findall(r'\{\s*"([^"]*)"\s*,\s*"((?:[^\\"]|\\.)*)"\s*\}', st)]

    tags = [(n, int(c)) for n, c in
            re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}',
                       _arr(s, "static const PresetTag kPresetTag"))]
    tag_of = [int(x) for x in re.findall(r"\b(\d+)\b",
                                         _arr(s, "static const uint8_t kPresetTagOf"))]

    if len(tag_of) != len(lst):
        raise SystemExit("kPresetTagOf %d 项 ≠ kPreset %d 台" % (len(tag_of), len(lst)))

    for k, c in enumerate(lst):
        c["t"] = tags[tag_of[k]][0] if tag_of[k] < len(tags) else ""

    ver = re.search(r"#define\s+PRESET_VER\s+(\d+)", s)
    return lst, tags, (ver.group(1) if ver else "?")


def unesc(x):
    return x.replace('\\"', '"').replace("\\\\", "\\")


def kind(u):
    """台号那一列怎么印：纯数字 = 蜻蜓台号；.m3u8 = HLS 直播流（九个中央台）；
    其余 http 直链（有声书的 m4a）就写「直链」。
    以前这里一律写「有声书」，结果中央台那九行也被标成有声书 —— 明显的错。"""
    if re.match(r"^\d+$", u):
        return u
    if ".m3u8" in u:
        return "HLS"
    return "直链"


def main():
    lst, tags, ver = load()
    total = len(lst)

    L = []
    L.append("# 台表 · %d 台 / %d 个标签\n" % (total, len(tags)))
    L.append("> 这份列表就是**固件里出厂台表**本身（`src/preset_stations.h`，台表版本 %s）。  " % ver)
    L.append("> 生成：`python tools/gen_stationlist.py`（源数据 `tools/台表-精选.md` + "
             "`tools/有声书源*.json`：喜马 / 听书网 / 云听 / 蜻蜓）  ")
    L.append("> 台号 = 蜻蜓台号，播放地址 `http://lhttp.qtfm.cn/live/<台号>/64k.mp3`；"
             "有声书那几行存的是完整直链（各源头格式不同：m4a / mp3）\n")
    L.append("**这台表的先后就是播放顺序**：转一下旋钮换一台，从第 1 台一路走到第 %d 台，"
             "到头绕回第 1 台。**没有「分组」这一层了**，转旋钮不会跳组。  " % total)
    nbook = sum(1 for it in lst if kind(it["i"]) == "直链")
    gap = int(round(float(total - nbook) / nbook)) if nbook else 0
    L.append("「标签」只是给手机 App 筛选 / 整理用的标记，同标签的台可以散在表里任何位置"
             "（%d 部有声书就是均匀穿插在电台之间的，大约每 %d 台一本）。\n" % (nbook, gap))

    L.append("## 标签一览\n")
    L.append("| 标签 | 台数 |")
    L.append("|---|---:|")
    for n, c in tags:
        L.append("| %s | %d |" % (n, c))
    L.append("| **合计** | **%d** |\n" % sum(c for _, c in tags))
    L.append("---\n")

    L.append("## 全部台（按播放顺序）\n")
    L.append("台号那列：纯数字 = 蜻蜓台号；`HLS` = 直播流地址（九个中央台）；"
             "`直链` = 完整音频地址（有声书）\n")
    L.append("| # | 台名 | 标签 | 台号 |")
    L.append("|---:|---|---|---|")
    for k, c in enumerate(lst, 1):
        sid = kind(c["i"])
        L.append("| %d | %s | %s | %s |"
                 % (k, unesc(c["n"]).replace("|", "\\|"), c["t"], sid))
    L.append("")

    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(L))

    # Excel 直接打开的 CSV：带 BOM，中文不乱码
    with open(OUT_CSV, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f)
        w.writerow(["播放序号", "台名", "标签", "台号 / 地址", "播放地址"])
        for k, c in enumerate(lst, 1):
            url = QT_URL % c["i"] if re.match(r"^\d+$", c["i"]) else c["i"]
            w.writerow([k, unesc(c["n"]), c["t"], c["i"], url])

    print("写出 %s  (%.0f KB, %d 台 / %d 个标签)"
          % (OUT, os.path.getsize(OUT) / 1024, total, len(tags)))
    print("写出 %s  (%.0f KB, 台表版本 %s)"
          % (OUT_CSV, os.path.getsize(OUT_CSV) / 1024, ver))


if __name__ == "__main__":
    main()
