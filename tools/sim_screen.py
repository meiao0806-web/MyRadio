#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
sim_screen.py —— 320x170 屏幕离线渲染器

作用：不改固件、不上机，先把 src/ui.cpp 的版式在电脑上画出来，
      用来检查重叠、越界、字宽、对比度。坐标与 ui.cpp 一一对应。

用法：
  python tools/sim_screen.py            # 输出 3 张状态图到 %TEMP%
"""
import os
import re
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "src", "font_cn.h")
W, H, S = 320, 170, 3
OUT = os.environ.get("TEMP", "/tmp")

# ------- 配色（与 config.h 一致）-------
C = dict(
    bg=(0, 0, 0), name=(233, 233, 233), accent=(176, 107, 3), cool=(1, 131, 157),
    sub=(207, 206, 206), bar_bg=(32, 32, 32), nam_bg=(0, 78, 97), nam_fg=(255, 255, 0),
    band_bg=(176, 107, 3), band_fg=(0, 0, 0), mode_bg=(0, 78, 97), mode_fg=(233, 233, 233),
    time=(207, 206, 206), err=(248, 0, 0), dim=(140, 142, 140),
)

# ------- 布局（与 ui.cpp 一致）-------
TOP_H = 22
FREQ_Y, FREQ_H = 22, 46
TUN_Y, TUN_H = 68, 16
NAMB_Y, NAMB_H = 84, 20
META_Y, META_H = 106, 26
BOT_Y = 138
MARGIN = 5


def load_font(tag):
    src = open(HDR, encoding="utf-8").read()
    m = re.search(r"static const uint8_t %s_blob\[\d+\] = \{(.*?)\};" % tag, src, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", m.group(1)))
    mi = re.search(r"static const CnGlyph %s_idx\[\d+\] = \{(.*?)\};" % tag, src, re.S)
    idx = {}
    for cp, off, w, st in re.findall(r"\{0x([0-9A-F]{4}),(\d+),(\d+),(\d+)\}", mi.group(1)):
        idx[int(cp, 16)] = (int(off), int(w), int(st))
    return blob, idx


class Screen:
    def __init__(self):
        self.im = Image.new("RGB", (W, H), C["bg"])
        self.d = ImageDraw.Draw(self.im)
        self.f28 = load_font("font28")
        self.f16 = load_font("font16")

    # --- 基本图元 ---
    def rect(self, x, y, w, h, c):
        if w > 0 and h > 0:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def line_rect(self, x, y, w, h, c):
        self.d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)

    def dot(self, x, y, r, c):
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=c)

    def tw(self, s, big):
        blob, idx = self.f28 if big else self.f16
        return sum(idx.get(ord(ch), (0, 28 if big else 16, 0))[1] for ch in s)

    def text(self, s, x, y, fg, bg, big):
        blob, idx = self.f28 if big else self.f16
        Hh = 28 if big else 16
        for ch in s:
            g = idx.get(ord(ch))
            w = g[1] if g else (28 if big else 16)
            self.rect(x, y, w, Hh, bg)
            if g:
                off, ww, stride = g
                for yy in range(Hh):
                    for xx in range(ww):
                        if blob[off + yy * stride + (xx >> 3)] & (0x80 >> (xx & 7)):
                            self.d.point((x + xx, y + yy), fill=fg)
            x += w

    def center(self, s, y, fg, bg, big):
        self.text(s, (W - self.tw(s, big)) // 2, y, fg, bg, big)

    # --- 组件 ---
    def badge(self, x, y, t, fg, bg):
        w = self.tw(t, False) + 8
        self.rect(x, y, w, 16, bg)
        self.text(t, x + 4, y, fg, bg, False)
        return x + w + 4

    def sigbars(self, x, y, bars):
        for i in range(4):
            h = 4 + i * 3
            self.rect(x + i * 6, y + 13 - h, 4, h, C["cool"] if i < bars else C["bar_bg"])

    def volpill(self, x, y, vol, muted):
        w, h = 52, 15
        self.rect(x, y, w, h, C["bar_bg"])
        self.line_rect(x, y, w, h, C["cool"])
        fw = 0 if muted else vol * (w - 22) // 100
        if fw > 0:
            self.rect(x + 2, y + 2, fw, h - 4, C["cool"])
        b = str(0 if muted else vol)
        self.text(b, x + w - 2 - self.tw(b, False), y, C["sub"], C["bar_bg"], False)

    # --- 分区 ---
    def top(self, wifi, muted, vol, rssi, bars, band="网络"):
        self.rect(0, 0, W, TOP_H, C["bg"])
        x = MARGIN
        x = self.badge(x, 3, band, C["band_fg"], C["band_bg"])
        x = self.badge(x, 3, "WiFi" if wifi else "断网", C["mode_fg"], C["mode_bg"] if wifi else C["err"])
        if muted:
            self.badge(x, 3, "静音", C["band_fg"], C["accent"])
        self.sigbars(232, 2, bars if wifi else 0)
        self.volpill(W - MARGIN - 52, 3, vol, muted)

    def freq(self, name, color=C["name"]):
        self.rect(0, FREQ_Y, W, FREQ_H, C["bg"])
        self.center(name, FREQ_Y + 9, color, C["bg"], True)

    def tuner(self, n, idx, preview=False):
        self.rect(0, TUN_Y, W, TUN_H, C["bar_bg"])
        if n <= 0:
            return
        x0, x1 = 4, W - 5
        pos = lambda i: (x0 + x1) // 2 if n == 1 else x0 + (x1 - x0) * i // (n - 1)
        skip = 2 if n > 60 else 1
        for i in range(0, n, skip):
            h = 7 if i == idx else 5
            self.rect(pos(i), TUN_Y + TUN_H - 2 - h, 1, h, C["cool"])
        cx = pos(max(0, idx))
        self.rect(cx - 1, TUN_Y, 3, TUN_H, C["sub"] if preview else C["accent"])

    def namebar(self, st, sc, idx, n):
        self.rect(0, NAMB_Y, W, NAMB_H, C["nam_bg"])
        self.dot(9, NAMB_Y + NAMB_H // 2, 4, sc)
        self.text(st, 17, NAMB_Y + 2, C["nam_fg"], C["nam_bg"], False)
        b = "%d/%d" % (idx + 1, n)
        self.text(b, W - MARGIN - self.tw(b, False), NAMB_Y + 2, C["nam_fg"], C["nam_bg"], False)

    def meta(self, rssi, vol, muted, n, wifi):
        mx, mw = 4, 150
        self.rect(mx, META_Y, mw, META_H, C["bar_bg"])
        self.text("信号", mx + 4, META_Y + 1, C["sub"], C["bar_bg"], False)
        r = "%ddB" % rssi
        self.text(r, mx + mw - 4 - self.tw(r, False), META_Y + 1, C["sub"], C["bar_bg"], False)
        bx, by, bw, bh = mx + 4, META_Y + 17, mw - 8, 7
        self.rect(bx, by, bw, bh, C["bg"])
        lvl = max(0, min(bw, (rssi + 95) * bw // 55)) if wifi else 0
        if lvl:
            self.rect(bx, by, lvl, bh, C["cool"])

        px, pw = 160, W - 160 - 4
        self.rect(px, META_Y, pw, META_H, C["bar_bg"])
        self.line_rect(px, META_Y, pw, META_H, C["nam_bg"])
        a = "音量 %d" % (0 if muted else vol)
        self.text(a, px + 6, META_Y + 5, C["accent"] if muted else C["sub"], C["bar_bg"], False)
        b = "共 %d 台" % n
        self.text(b, px + pw - 6 - self.tw(b, False), META_Y + 5, C["sub"], C["bar_bg"], False)

    def bottom(self, clk, wd, tip):
        self.rect(0, BOT_Y, W, H - BOT_Y, C["bg"])
        x = MARGIN
        self.text(clk, x, BOT_Y + 7, C["time"], C["bg"], False)
        x += self.tw(clk, False) + 10
        self.text(wd, x, BOT_Y + 7, C["sub"], C["bg"], False)
        self.text(tip, W - MARGIN - self.tw(tip, False), BOT_Y + 7, C["dim"], C["bg"], False)

    def infopage(self, title, lines):
        self.rect(0, 0, W, 3, C["accent"])
        self.center(title, 12, C["name"], C["bg"], True)
        y = 52
        for i, s in enumerate(lines):
            self.dot(14, y + 8, 3, C["cool"])
            self.text(s, 24, y, C["accent"] if i == 3 else C["sub"], C["bg"], False)
            y += 26

    def save(self, path):
        self.im.resize((W * S, H * S), Image.NEAREST).save(path)


def scene_play():
    s = Screen()
    s.top(True, False, 45, -58, 4, "蜻蜓")
    s.freq("北京新闻广播")
    s.tuner(20, 0)
    s.namebar("播放中", C["cool"], 0, 20)
    s.meta(-58, 45, False, 20, True)
    s.bottom("10:24", "周六", "旋钮换台")
    return s


def scene_link():
    s = Screen()
    s.top(True, False, 45, -72, 3, "蜻蜓")
    s.freq("上海动感101")
    s.tuner(20, 8, preview=True)
    s.namebar("连接中", C["accent"], 8, 20)
    s.meta(-72, 45, False, 20, True)
    s.bottom("10:25", "周六", "稍等缓冲")
    return s


def scene_setup():
    s = Screen()
    s.infopage("先连一下网络", ["1.手机连 MyRadio-1A2B", "2.密码 12345678",
                                "3.打开 192.168.4.1", "配对码 5731"])
    return s


def scene_muted():
    """非蜻蜓台 + 静音状态（验证徽标变多时顶栏是否挤）"""
    s = Screen()
    s.top(True, True, 0, -66, 2, "网络")
    s.freq("广东珠江之声")
    s.tuner(20, 14)
    s.namebar("已静音", C["sub"], 14, 20)
    s.meta(-66, 0, True, 20, True)
    s.bottom("10:26", "周六", "旋钮换台")
    return s


if __name__ == "__main__":
    for nm, fn in [("play", scene_play), ("link", scene_link), ("muted", scene_muted)]:
        p = os.path.join(OUT, "scr_%s.png" % nm)
        fn().save(p)
        print(p)
