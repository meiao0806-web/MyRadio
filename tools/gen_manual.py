#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把真台表注入说明书模板 -> 说明书.html（单文件，离线可用）

用法：python tools/gen_manual.py
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "qtfm_all.json")
TPL = os.path.join(HERE, "manual_template.html")
OUT = os.path.join(ROOT, "说明书.html")


def fw_version():
    """固件版本从 config.h 读，说明书里的版本号和目录名跟着走，不用手工同步"""
    s = open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+FW_VER\s+"([^"]+)"', s)
    if not m:
        raise SystemExit("config.h 里找不到 FW_VER")
    return m.group(1)


def load_preset():
    """从固件台表读，保证说明书 / 固件 / App 三处永远是同一份数据

    1.15.0 起固件不再分组，所以给模板的是两份东西：
      list —— 整张表，**先后就是播放顺序**（说明书里的序号要照这个印）
      tags —— 标签名 + 台数，只用于筛选
    """
    s = open(os.path.join(ROOT, "src", "preset_stations.h"), encoding="utf-8").read()

    def arr(decl):
        m = re.search(decl + r"\[\] = \{(.*?)\n\};", s, re.S)
        if not m:
            raise SystemExit("preset_stations.h 里找不到 %s[]" % decl)
        return m.group(1)

    st = arr("static const PresetSt kPreset")
    lst = [{"n": n, "i": u} for u, n in
           re.findall(r'\{\s*"([^"]*)"\s*,\s*"((?:[^\\"]|\\.)*)"\s*\}', st)]
    for c in lst:
        c["n"] = c["n"].replace('\\"', '"').replace("\\\\", "\\")

    ep = open(os.path.join(ROOT, "src", "preset_episodes.h"), encoding="utf-8").read()
    m = re.search(r"#define\s+EPIS_ITEM_N\s+(\d+)", ep)
    epn = int(m.group(1)) if m else 0
    mb = re.search(r"#define\s+EPIS_BOOK_N\s+(\d+)", ep)
    epb = int(mb.group(1)) if mb else 0

    tags = [(n, int(c)) for n, c in
            re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}',
                       arr("static const PresetTag kPresetTag"))]
    tag_of = [int(x) for x in re.findall(r"\b(\d+)\b",
                                         arr("static const uint8_t kPresetTagOf"))]
    if len(tag_of) != len(lst):
        raise SystemExit("kPresetTagOf %d 项 ≠ kPreset %d 台" % (len(tag_of), len(lst)))
    for k, c in enumerate(lst):
        c["t"] = tag_of[k]

    return {"list": lst, "tags": [{"n": n, "c": c} for n, c in tags],
            "epn": epn, "epb": epb}


def main():
    data = load_preset()
    total = len(data["list"])

    js = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    # 防止台名里混进 </script> 把脚本截断（\u003c 是 JSON 合法转义）
    js = js.replace("</", "<\\/")

    tpl = open(TPL, encoding="utf-8").read()
    if "__STATIONS__" not in tpl:
        raise SystemExit("模板里找不到 __STATIONS__ 占位符")
    html = (tpl.replace("__STATIONS__", js)
               .replace("__FWV__", fw_version())
               .replace("__GCNT__", str(len(data["tags"])))
               .replace("__STCNT__", str(total))
               .replace("__EPCNT__", str(data["epn"]))
               .replace("__EPBOOK__", str(data["epb"])))

    open(OUT, "w", encoding="utf-8", newline="\n").write(html)
    print("写出 %s  (%.0f KB, %d 台 / %d 个标签)"
          % (OUT, os.path.getsize(OUT) / 1024, total, len(data["tags"])))


if __name__ == "__main__":
    main()
