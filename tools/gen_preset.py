#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
qtfm_all.json -> src/preset_stations.h（编译进固件的出厂台表）

用法：python tools/gen_preset.py

产物结构：
    kPreset[]     每台：台号 + 台名（按地区顺序连续排列）
    kPresetGrp[]  每组：组名 + 在 kPreset 里的起始下标 + 台数
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "qtfm_all.json")
OUT = os.path.join(ROOT, "src", "preset_stations.h")

MAX_NAME = 39          # 与 Station::name[40] 对齐，留 1 字节 0


def fit(s, limit=MAX_NAME):
    """按 UTF-8 边界截断，不留半个汉字"""
    b = s.encode("utf-8")
    if len(b) <= limit:
        return s
    cut = limit
    while cut > 0 and (b[cut] & 0xC0) == 0x80:      # 回退到字符首字节
        cut -= 1
    return b[:cut].decode("utf-8", "ignore")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    data = json.load(open(SRC, encoding="utf-8"))

    st_lines, grp_lines = [], []
    idx = 0
    for g in data:
        chs = [c for c in g["channels"] if c.get("name") and c.get("id")]
        if not chs:
            continue
        grp_lines.append(f'  {{ {cstr(g["region"])}, {idx}, {len(chs)} }},')
        for c in chs:
            st_lines.append(f'  {{ "{c["id"]}", {cstr(fit(c["name"]))} }},')
        idx += len(chs)

    hdr = f'''// 自动生成，请勿手改。
// 来源：蜻蜓 FM 公开接口 https://rapi.qingting.fm/regions
// 重新抓取：python tools/fetch_qtfm.py && python tools/gen_preset.py
#pragma once
#include <stdint.h>

struct PresetSt  {{ const char* id; const char* name; }};
struct PresetGrp {{ const char* name; uint16_t first; uint16_t count; }};

#define PRESET_ST_N   {idx}
#define PRESET_GRP_N  {len(grp_lines)}

static const PresetSt kPreset[] = {{
'''
    body = "\n".join(st_lines) + "\n};\n\n"
    body += "static const PresetGrp kPresetGrp[] = {\n" + "\n".join(grp_lines) + "\n};\n"

    open(OUT, "w", encoding="utf-8", newline="\n").write(hdr + body)
    kb = os.path.getsize(OUT) / 1024
    print(f"写出 {OUT}  ({kb:.0f} KB)")
    print(f"  {idx} 台 / {len(grp_lines)} 组")
    for g in data[:5]:
        print(f"  {g['region']}: {len(g['channels'])} 台")


if __name__ == "__main__":
    main()
