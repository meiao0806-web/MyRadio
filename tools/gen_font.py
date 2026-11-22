#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_font.py —— 生成 1bit 点阵中文字库头文件（给 TFT_eSPI 用）

为什么需要：TFT_eSPI 自带字库没有中文，屏上要显示台名就必须自己塞字模。
输出 1bpp 位图，直接 XBM 式写屏，比 smooth font(.vlw) 省事、省 RAM。

两套字号：
  font32 —— 台名大字（28px 时坐远了看不清，放到 32px）。收 GB2312 一级字库(3755 字) + ASCII，
            这样 App 里随便加中文台名都不会出现方块
  font16 —— 状态小字。只收 src/ 里实际出现的字 + ASCII，省 flash

用法：
  python tools/gen_font.py
产物：
  src/font_cn.h
"""
import os
import re
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow：pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(SRC, "font_cn.h")

FONT_PATH = "C:/Windows/Fonts/msyh.ttc"     # 微软雅黑 常规
FONT_PATH_R = "C:/Windows/Fonts/msyh.ttc"   # 同上
BOLD_PATH = "C:/Windows/Fonts/msyhbd.ttc"   # 微软雅黑 Bold
#  1.40.0：收音频率要「加大加粗」。中文字库不能整体换粗（16px 会糊成一团），
#  但频率只有 ASCII（FM 98.4），单独出**一档 48px 粗体 ASCII** 最省事：
#  只要 100 来个字形、约 30KB，比给整套汉字加粗省 200KB。
# 1.38.3：**不再用 Bold**。
#   原厂 UI 的大字是 Orbitron_Light_24（细体），我们一直用 msyhbd（粗体），
#   笔画粗一倍，屏幕上看着比原厂"糊"一圈。中文没有 Orbitron，
#   但「细笔画 + 大字号」这个观感和原厂是一致的，所以统一走常规字重。
FONT_IDX = 0


def font_path_for(px):
    """1.38.3：所有字号都用常规字重（原厂是细体，粗体会糊成一团）。"""
    return FONT_PATH_R


def gb2312_level1():
    """3755 个常用汉字（按拼音序）"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.append(ch)
    return out


def ascii_set():
    return [chr(c) for c in range(0x20, 0x7F)]


PUNCT = "，。、：；！？（）《》【】“”‘’…—·％℃°"

EXTRA = "正在连接已暂停停止信号不好重连中这个台没有声音请检查网络静音音量遥控器学习完成下一台上按" \
        "播放暂定无线有线秒分钟小时星期一二三四五六日年月日号点分号第共首"


def chars_in_ui():
    """只扫「界面文案」所在的源文件（不含 preset_stations.h 的台名）。

    1.38.3：20px 这一档只用在顶栏和三个框，内容是固定文案
    （网络广播 / 广播 / 播放 / 音量 / 断网 / 调频 / 中波…）。
    收整套 3755 汉字要 162KB，其实 3KB 就够 —— flash 已经 79% 了，这一刀省得值。
    """
    s = set()
    for fn in ("ui.cpp", "api.cpp", "net.cpp", "input.cpp", "main.cpp",
               "audio.cpp", "fm.cpp", "power.cpp", "battery.cpp"):
        p = os.path.join(SRC, fn)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8", errors="ignore") as f:
            s |= set(re.findall(r"[^\x00-\x7f]", f.read()))
    return s


def chars_in_src():
    """扫描源码里出现的所有非 ASCII 字符，保证状态文案和台名不缺字

    收全部非 ASCII（而不是只收汉字区）是必要的：
    preset_stations.h 里有韩文台名（디라오），还有地名用字 •，
    只扫 \\u2e80-\\u9fff 会漏，屏上就变成空白。
    """
    s = set()
    for fn in os.listdir(SRC):
        if not fn.endswith((".cpp", ".h")):
            continue
        if fn == "font_cn.h":
            continue
        with open(os.path.join(SRC, fn), encoding="utf-8", errors="ignore") as f:
            s |= set(re.findall(r"[^\x00-\x7f]", f.read()))
    return s


def render(ch, px, bold=False):
    """把一个字渲染成 px 高的 1bit 行数据，返回 (w, stride, data)

    中文占满 px×px；ASCII 用 0.76 倍字号 + 实际字宽 + 1px 间隙，
    这样 M / G 不会被压成 N / C（半宽固定格会丢笔画）。
    """
    import math
    SS = 4                                   # 超采样倍数，边缘更干净
    is_ascii = ord(ch) < 0x2E80
    fpx = max(8, int(px * 0.76)) if is_ascii else px

    path = BOLD_PATH if bold else font_path_for(px)
    if not os.path.exists(path):
        path = font_path_for(px)          # 没有粗体就退回常规，不至于跑不出来
    font = ImageFont.truetype(path, fpx * SS, index=FONT_IDX)
    asc, _ = font.getmetrics()
    adv = font.getlength(ch)

    if is_ascii:
        w = max(2, int(math.ceil(adv / SS)) + 1)
    else:
        w = px

    canvas = Image.new("L", (w * SS, px * SS), 0)
    d = ImageDraw.Draw(canvas)

    baseline = int((px * SS) * 0.80)         # 统一基线，中英混排不歪
    x = 0 if is_ascii else int(((w * SS) - adv) / 2)
    d.text((x, baseline - asc), ch, font=font, fill=255)

    small = canvas.resize((w, px), Image.LANCZOS)

    stride = (w + 7) // 8
    data = bytearray(stride * px)
    px_data = small.load()
    for y in range(px):
        for x2 in range(w):
            if px_data[x2, y] > 128:          # 128 比 96 收得干净些，16px 小字不容易糊
                data[y * stride + (x2 >> 3)] |= (0x80 >> (x2 & 7))
    return w, stride, bytes(data)


def build(name, px, charset, bold=False):
    seen = {}
    for ch in charset:
        cp = ord(ch)
        if cp > 0xFFFF:
            continue
        if cp in seen:
            continue
        seen[cp] = render(ch, px, bold)
    cps = sorted(seen)
    blob = bytearray()
    idx = []
    for cp in cps:
        w, stride, data = seen[cp]
        idx.append((cp, len(blob), w, stride))
        blob += data

    print(f"  {name}: {len(cps)} 字形, {len(blob)/1024:.1f} KB")

    lines = []
    lines.append(f"static const uint8_t {name}_blob[{len(blob)}] = {{")
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in blob[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"static const CnGlyph {name}_idx[{len(idx)}] = {{")
    for cp, off, w, stride in idx:
        lines.append(f"  {{0x{cp:04X},{off},{w},{stride}}},")
    lines.append("};")
    lines.append(f"#define {name.upper()}_N {len(idx)}")
    lines.append(f"#define {name.upper()}_H {px}")
    lines.append("")
    return "\n".join(lines)


def main():
    if not os.path.exists(FONT_PATH):
        sys.exit(f"找不到字体 {FONT_PATH}，改脚本里的 FONT_PATH")

    print("生成中…")
    big_set = ascii_set() + gb2312_level1() + list(PUNCT) + list(EXTRA) + list(chars_in_src())
    small_set = ascii_set() + list(PUNCT) + list(EXTRA) + list(chars_in_src())
    # 20px 档：界面文案专用，不含台名（台名用 36px 那档）
    ui_set = ascii_set() + list(PUNCT) + list(EXTRA) + list(chars_in_ui())

    body = ["// 本文件由 tools/gen_font.py 生成，不要手工编辑", "#pragma once",
            "#include <stdint.h>", "",
            "struct CnGlyph { uint16_t cp; uint32_t off; uint8_t w; uint8_t stride; };", ""]
    # 1.38.3：三档字号
    #   font36 —— 台名/大字（原 32 → 36，用户要求「台名称放大」）
    #   font20 —— 顶栏、三个框（原 16 → 20，用户要求「波段名字放大」）
    #   font16 —— 刻度数字、次要提示（刻度区只有 38px 高，16 刚好）
    # 1.40.0：48px 粗体，**只收 ASCII**（收音频率「FM 98.4」用）
    freq_set = ascii_set() + list(" .:-")
    body.append(build("font48", 48, freq_set, bold=True))
    body.append(build("font36", 36, big_set))
    body.append(build("font20", 20, ui_set))
    body.append(build("font16", 16, small_set))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(body))
    print(f"写出 {OUT}  ({os.path.getsize(OUT)/1024:.0f} KB)")


if __name__ == "__main__":
    main()
