#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gen_apk_icon.py —— 手写 PNG 生成 MyRadio 的桌面图标（零第三方依赖）

画什么：一块深色圆角底 + 一块浅色屏幕（带两道青绿刻度）+ 下面一个旋钮。
  ic_launcher.png   满幅，给 API 26 以下当普通图标
  ic_fg.png         图形缩到中央 66%，四周透明，给 adaptive-icon 当前景
                     （108dp 画布里只有中央 72dp 是安全区，超出的会被裁）

用法： python tools/gen_apk_icon.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "android", "app", "src", "main", "res")

BG = (0x2E, 0x3A, 0x40)
BODY = (0x39, 0x49, 0x51)
SCREEN = (0xE6, 0xE9, 0xEF)
BRAND = (0x3F, 0x71, 0x80)
KNOB = (0x8F, 0xBE, 0xC8)
SS = 4          # 超采样倍数（抗锯齿）


def png(w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def in_rr(x, y, x0, y0, x1, y1, r):
    """点是否在圆角矩形内。"""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= r * r


def in_circle(x, y, cx, cy, r):
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= r * r


def shade(n):
    """归一化坐标 -> 颜色（按层叠加，后画的盖前面）。返回 (r,g,b,a) 或 None。"""
    # 底：圆角方
    if in_rr(n[0], n[1], 0.0, 0.0, 1.0, 1.0, 0.225):
        col = BG
    else:
        return None

    x, y = n
    # 机身上半（略微亮一点，做出厚度）
    if in_rr(x, y, 0.10, 0.10, 0.90, 0.90, 0.17):
        col = BODY
    # 屏幕
    if in_rr(x, y, 0.19, 0.245, 0.81, 0.485, 0.035):
        col = SCREEN
    # 屏幕上两道刻度
    if in_rr(x, y, 0.255, 0.315, 0.545, 0.345, 0.011):
        col = BRAND
    if in_rr(x, y, 0.255, 0.395, 0.435, 0.425, 0.011):
        col = BRAND
    # 旋钮：外圈
    if in_circle(x, y, 0.5, 0.675, 0.155):
        col = KNOB
    if in_circle(x, y, 0.5, 0.675, 0.105):
        col = BODY
    # 旋钮指针（从中心指向右上）
    dx, dy = x - 0.5, y - 0.675
    if (dx * dx + dy * dy) <= 0.105 * 0.105:
        # 判断是否落在 -55 度方向、宽度 0.045 的带子里
        ang = math.atan2(dy, dx)
        if abs(ang - (-0.96)) < 0.22 and (dx * dx + dy * dy) > 0.018 ** 2:
            col = KNOB
    return col


def render(size, scale=1.0):
    """scale<1 时图形整体缩小（居中留透明边），给 adaptive 前景用。"""
    w = h = size
    buf = bytearray(w * h * 4)
    off = (1.0 - scale) / 2.0
    for py in range(h):
        for px in range(w):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    # 像素内采样点 -> 画布归一化（0..1）
                    fx = (px + (sx + 0.5) / SS) / w
                    fy = (py + (sy + 0.5) / SS) / h
                    nx = (fx - off) / scale
                    ny = (fy - off) / scale
                    if nx < 0 or nx > 1 or ny < 0 or ny > 1:
                        continue
                    c = shade((nx, ny))
                    if c:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 255
            n = SS * SS
            i = (py * w + px) * 4
            if a:
                buf[i] = r // (a // 255)
                buf[i + 1] = g // (a // 255)
                buf[i + 2] = b // (a // 255)
                buf[i + 3] = a // n
            else:
                buf[i + 3] = 0
    return bytes(buf)


def write(path, size, scale=1.0):
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    data = render(size, scale)
    with open(path, "wb") as f:
        f.write(png(size, size, data))
    print("  %-58s %dpx  %d B" % (os.path.relpath(path, ROOT), size, os.path.getsize(path)))


def main():
    dens = [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]
    print("普通图标：")
    for name, px in dens:
        p = os.path.join(RES, "mipmap-" + name, "ic_launcher.png")
        write(p, px)
        write(os.path.join(RES, "mipmap-" + name, "ic_launcher_round.png"), px)
    print("adaptive 前景（中央 72/108 安全区）：")
    for name, px in dens:
        fg = int(round(px * 108.0 / 48.0))
        write(os.path.join(RES, "mipmap-" + name, "ic_fg.png"), fg, 72.0 / 108.0)


if __name__ == "__main__":
    main()
