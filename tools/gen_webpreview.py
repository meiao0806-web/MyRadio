#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_webpreview.py —— 把固件的「屏上界面 + 手机 App」导出成一个单文件网页

为什么这么做：屏是 320×170，界面逻辑全在 ui.cpp 里，烧录前没法看效果。
本脚本读真的 font_cn.h（点阵字库）、真的台表（store.cpp）、
真的网页 App（webapp.h），拼成 preview/index.html。

用法：
  python tools/gen_webpreview.py
产物：
  preview/index.html   （双击即开，零依赖）
"""
import base64
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
OUT_DIR = os.path.join(ROOT, "preview")
OUT = os.path.join(OUT_DIR, "index.html")


def parse_font(tag):
    """从 font_cn.h 取出 blob 与索引"""
    s = open(os.path.join(SRC, "font_cn.h"), encoding="utf-8").read()
    m = re.search(r"static const uint8_t %s_blob\[\d+\] = \{(.*?)\};" % tag, s, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", m.group(1)))
    mi = re.search(r"static const CnGlyph %s_idx\[\d+\] = \{(.*?)\};" % tag, s, re.S)
    idx = [[int(cp, 16), int(off), int(w), int(st)]
           for cp, off, w, st in re.findall(r"\{0x([0-9A-F]{4}),(\d+),(\d+),(\d+)\}", mi.group(1))]
    return {"blob": base64.b64encode(blob).decode(), "idx": idx}


def parse_seeds():
    """从 preset_stations.h 取整张台表 + 标签：
       {tags:[{n,c}], tagOf:[每台的标签下标], list:[{n,u}]}

       1.15.0 起固件不再分组：kPreset 的先后就是播放顺序，
       原来的 kPresetGrp（组名/起始/台数）换成了 kPresetTag（标签名/台数）
       + kPresetTagOf（每台一个标签下标）。"""
    s = open(os.path.join(SRC, "preset_stations.h"), encoding="utf-8").read()

    st = re.search(r"kPreset\[\] = \{(.*?)\n\};", s, re.S).group(1)
    lst = [{"n": n, "u": u} for u, n in
           re.findall(r'\{\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', st)]
    # 还原 C 字符串里的转义（台名里有中文引号，没有反斜杠，但保险）
    for it in lst:
        it["n"] = it["n"].replace('\\"', '"').replace("\\\\", "\\")

    tof = re.search(r"kPresetTagOf\[\] = \{(.*?)\n\};", s, re.S).group(1)
    tag_of = [int(x) for x in re.findall(r"\b(\d+)\b", tof)]

    tg = re.search(r"kPresetTag\[\] = \{(.*?)\n\};", s, re.S).group(1)
    tags = [{"n": n, "c": int(c)} for n, c in
            re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}', tg)]

    if len(tag_of) != len(lst):
        raise SystemExit("kPresetTagOf 有 %d 项，kPreset 有 %d 台 —— 两边对不上"
                         % (len(tag_of), len(lst)))
    return {"tags": tags, "tagOf": tag_of, "list": lst}


def parse_episodes():
    """从 preset_episodes.h 取有声书集表：{books:[{n,f,c}], items:[{t}]}

    1.18.0 起**不再带 url**：集数从 1823 涨到 3517（官场现形记一部就 1167 集），
    地址平均 200 字节，全塞进来光地址就 800KB，预览页会变成一个 1.3MB 的 HTML。
    预览页从来不放真音频（点了只改屏幕状态），只用得上标题 —— 所以只留 t。
    """
    path = os.path.join(SRC, "preset_episodes.h")
    if not os.path.exists(path):
        return {"books": [], "items": []}
    s = open(path, encoding="utf-8").read()
    st = re.search(r"kEpis\[\] = \{(.*?)\n\};", s, re.S).group(1)
    items = [{"t": t} for t, _u in
             re.findall(r'\{\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', st)]
    gr = re.search(r"kEpisBook\[\] = \{(.*?)\n\};", s, re.S).group(1)
    books = [{"n": n, "f": int(f), "c": int(c)} for n, f, c in
             re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', gr)]
    return {"books": books, "items": items}


def parse_webapp():
    """从 webapp.h 取真正的手机 App HTML"""
    s = open(os.path.join(SRC, "webapp.h"), encoding="utf-8").read()
    m = re.search(r'R"HTMLX\((.*)\)HTMLX"', s, re.S)
    return m.group(1)


# ---- 1.40.0：配色单一来源 ----
#  为什么：之前预览页的调色板是**手工抄**的 config.h 数值，抄一次就漂一次。
#  用户已经两次说「真机颜色和预览不一样」——每次都得两处对着比，还容易再错。
#  现在每次生成预览页都从 config.h 重新解析一遍：真机改一个值，预览页自动跟着变，
#  两边在物理上就不可能不一致。
#
#  RGB565 → #rrggbb：R 占高 5 位、G 占 6 位、B 占低 5 位。
#  注意 G 是 6 位（除 63），R/B 是 5 位（除 31）—— 这里写错会让绿色整体偏亮，
#  是很常见的坑，别照抄 "都除 31"。
_C_KEY = {
    "BG": "bg", "NAME": "name", "ACCENT": "accent", "COOL": "cool",
    "SUB": "sub", "BAR_BG": "barBg", "NAM_BG": "namBg", "NAM_FG": "namFg",
    "BAND_BG": "bandBg", "BAND_FG": "bandFg", "MODE_BG": "modeBg",
    "MODE_FG": "modeFg", "TIME": "time", "ERR": "err", "WHITE": "white",
    "RED": "red", "TICK": "tick", "SIGLOW": "sigLow",
    "DIAL_BG": "dialBg", "DIAL_FG": "dialFg", "DIAL_MK": "dialMk",
    "DIAL_PTR": "dialPtr",
    "BATT_HI": "battHi", "BATT_MID": "battMid", "BATT_LOW": "battLow",
}


def parse_palette():
    """从 config.h 的 #define C_XXX 0xNNNN 生成预览页用的 JS 调色板"""
    s = open(os.path.join(SRC, "config.h"), encoding="utf-8").read()
    pal = {}
    for name, hexv in re.findall(r"#define\s+C_([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]{4})", s):
        key = _C_KEY.get(name)
        if not key:
            continue
        v = int(hexv, 16)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        pal[key] = "#%02x%02x%02x" % (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    return pal


# ---- 1.45.0：真机屏（ST7789 320x170）面板参数 ----
#  预览页原来只做 RGB565 还原，那是「算出来的颜色」；这里补上「屏上看到的颜色」。
#  默认值不是猜的，是按这类 1.9~2.0 寸 IPS(ST7789) 的常见特性 + 用户实测反馈定的：
#    · black=7   背光常亮，纯黑仍有漏光（屏上看不到真正的 #000000）
#    · white=253 面板白点（背光亮度不到满量程）
#    · sat=0.90  色域窄于 sRGB，高饱和色被拉回
#    · blue=0.93 这块屏的蓝偏弱 —— 用户看过真机说 C_COOL(#01839d)「偏墨绿」，
#                墨绿 = 蓝掉、绿相对涨，所以单削蓝通道
#    · contrast=1.04  驱动 IC 的 γ 比 sRGB 略陡
#  预览页右上角「标定…」里有滑杆，对着真机调到像为止，把得到的数抄回这里即可。
PANEL = {
    "black": 13, "white": 252, "sat": 0.87,
    "blue": 0.90, "contrast": 1.06,
}


def fw_version():
    """从 config.h 读 FW_VER，免得预览页的版本号写死忘了同步"""
    s = open(os.path.join(SRC, "config.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+FW_VER\s+"([^"]+)"', s)
    return m.group(1) if m else "0.0.0"


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    tpl = open(os.path.join(ROOT, "tools", "preview_template.html"), encoding="utf-8").read()

    # 1.38.3：三档字库（font36 台名 / font20 顶栏 / font16 刻度）
    font = {"f48": parse_font("font48"), "f36": parse_font("font36"),
            "f20": parse_font("font20"), "f16": parse_font("font16")}
    seeds = parse_seeds()
    episodes = parse_episodes()
    webapp = parse_webapp()

    # 注意：webapp 自带 <script>...</script>，直接塞进外层脚本文本会把外层脚本提前截断，
    # 导致后面所有 JS 被当成正文渲染。必须把 "</script" 转义成 "<\/script"（JSON 合法转义）。
    def js_str(v):
        return json.dumps(v, ensure_ascii=False).replace("</script", "<\\/script")

    # 1.4.2 起模板改用矢量字，不再内嵌点阵字库（预览页 ~720 KB → ~100 KB）。
    # 这里仍解析一遍字库，只为了在输出里报个数、确认字库本身没问题。
    pal = parse_palette()
    html = (tpl
            .replace("__SEEDS_JSON__", js_str(dict(seeds, epis=episodes,
                                                  fw="MyRadio v" + fw_version())))
            .replace("__WEBAPP_JSON__", js_str(webapp))
            .replace("__PALETTE_JSON__", js_str(pal))
            .replace("__PANEL_JSON__", js_str(PANEL)))

    open(OUT, "w", encoding="utf-8").write(html)
    kb = os.path.getsize(OUT) / 1024
    print(f"写出 {OUT}  ({kb:.0f} KB)")
    print(f"  配色 {len(pal)} 项（来自 config.h）")
    print(f"  字库（真机用，预览页不再内嵌）48px {len(font['f48']['idx'])} / "
          f"36px {len(font['f36']['idx'])} / "
          f"20px {len(font['f20']['idx'])} / 16px {len(font['f16']['idx'])} 字形，"
          f"{len(seeds['list'])} 台 / {len(seeds['tags'])} 个标签，"
          f"有声书 {len(episodes['books'])} 部 / {len(episodes['items'])} 集")


if __name__ == "__main__":
    main()
