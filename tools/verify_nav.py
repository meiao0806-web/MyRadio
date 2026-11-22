#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""换台「乱跳」回归（1.46.22）：两道护栏都得在。

用户原话：
  「有的台会乱跳，没有按顺序，140 多台突然跳到 280 多」

140 → 280 这个量级不可能是一格一格转出来的，只可能来自两处：
  ① 自动换台连着跳（连不上 → 判停播 → 跳 → 又连不上 → 再跳……）
  ② 旋钮判据一次性吐出几十格（编码器坏 / 线松 / 假沿）

所以各加一道刹车：
  ① advance() 里同一个原因连跳 AUTO_HOP_MAX 台就停住、放沙沙、提示；
     用户手动切台（CMD_PLAY）把计数清零，不挡正常使用。
  ② inputLoop 一次取格超过 ±ENC_STEP_CLAMP 就夹住并记数；
     commitPending 再夹一道（遥控 / App 也汇到这里）。

用法：python tools/verify_nav.py
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def read(rel):
    return io.open(os.path.join(ROOT, rel), encoding="utf-8").read()


ok_n = bad_n = 0


def chk(name, got, want):
    global ok_n, bad_n
    if got == want:
        ok_n += 1
        print("  OK   %s" % name)
    else:
        bad_n += 1
        print("  FAIL %s  期望 %r 实际 %r" % (name, want, got))


# ------------------------------------------------------------
# 行为仿真：advance 的刹车（与 src/audio.cpp 里那段逐行对应）
# ------------------------------------------------------------
def sim(config_text):
    mx = int(re.search(r"#define\s+AUTO_HOP_MAX\s+(\d+)", config_text).group(1))
    win = int(re.search(r"#define\s+AUTO_HOP_WINDOW_MS\s+(\d+)", config_text).group(1))
    clamp = int(re.search(r"#define\s+ENC_STEP_CLAMP\s+(\d+)", config_text).group(1))

    class Nav:
        def __init__(self):
            self.n = 0
            self.at = 0

        def advance(self, t):
            if self.at == 0 or (t - self.at) > win:
                self.n = 0
            if self.n >= mx:
                self.at = t            # 只刷新时刻，不清零（清了就等于没刹）
                return "stop"          # 放沙沙 + 「信号不好，停在这一台」
            self.n += 1
            self.at = t
            return "hop"

        def manual(self, t):
            self.n = 0
            self.at = 0

    return Nav(), mx, win, clamp


def main():
    cfg = read("src/config.h")
    audio = read("src/audio.cpp")
    inp = read("src/input.cpp")

    print("=== 1) 常量在位 ===")
    chk("config.h 有 AUTO_HOP_MAX", bool(re.search(r"#define\s+AUTO_HOP_MAX\s+\d+", cfg)), True)
    chk("config.h 有 AUTO_HOP_WINDOW_MS", bool(re.search(r"#define\s+AUTO_HOP_WINDOW_MS\s+\d+", cfg)), True)
    chk("config.h 有 ENC_STEP_CLAMP", bool(re.search(r"#define\s+ENC_STEP_CLAMP\s+\d+", cfg)), True)

    print("=== 2) 源码里两道刹车都接上了 ===")
    a = audio.index("static void advance(int dir) {")
    body = audio[a:a + 1400]
    chk("advance() 里判断 AUTO_HOP_MAX", "AUTO_HOP_MAX" in body, True)
    chk("advance() 到顶后进噪声（不是继续跳）", "enterNoise()" in body, True)
    chk("advance() 到顶后提示「信号不好」", "信号不好" in body, True)
    chk("CMD_PLAY（用户/App 切台）清零刹车", "case CMD_PLAY:  s_autoHop = 0" in audio, True)

    i = inp.index("if (d > ENC_STEP_CLAMP")
    chk("inputLoop 取格有夹子", "d = (d > 0) ? ENC_STEP_CLAMP" in inp[i:i + 400], True)
    j = inp.index("static void commitPending()")
    chk("commitPending 也夹 s_pending", "s_pending > ENC_STEP_CLAMP" in inp[j:j + 500], True)
    chk("/api/enc 报出夹了几次（clamp）", '\\"clamp\\"' in inp, True)

    print("=== 3) 行为仿真：连续自动跳会被刹住 ===")
    nav, mx, win, clamp = sim(cfg)
    seq = [nav.advance(1000 * k) for k in range(1, 11)]     # 每 1 秒跳一次，共 10 次
    chk("前 %d 次正常跳" % mx, seq[:mx], ["hop"] * mx)
    chk("第 %d 次起连续 4 次都是停住（不是跳一格停一下）" % (mx + 1),
        seq[mx:], ["stop"] * (10 - mx))

    print("=== 4) 用户手动切台后能重新跳 ===")
    nav, mx, win, clamp = sim(cfg)
    for k in range(1, mx + 1):
        nav.advance(1000 * k)
    chk("手动切台前：已到上限", nav.advance(1000 * (mx + 1)), "stop")
    nav.manual(1000 * (mx + 2))
    chk("手动切台后：第一跳放行", nav.advance(1000 * (mx + 3)), "hop")

    print("=== 5) 隔一会儿自己恢复（不会永久锁死） ===")
    nav, mx, win, clamp = sim(cfg)
    for k in range(1, mx + 1):
        nav.advance(1000 * k)
    chk("窗口内：停住", nav.advance(1000 * (mx + 1)), "stop")
    chk("过了 %d 秒窗口：重新放行" % (win // 1000),
        nav.advance(1000 * (mx + 1) + win + 1000), "hop")

    print("=== 6) 夹子夹的是「异常大」，正常连甩不受影响 ===")
    def clampd(d):
        if d > clamp or d < -clamp:
            return ("clamp", (clamp if d > 0 else -clamp))
        return ("pass", d)
    chk("甩 10 格（真人上限）：放行", clampd(10), ("pass", 10))
    chk("转 20 格：放行（正好到边）", clampd(20), ("pass", 20))
    chk("吐出 140 格（故障）：夹到 20", clampd(140), ("clamp", 20))
    chk("反向吐 300 格（故障）：夹到 -20", clampd(-300), ("clamp", -20))

    print("\n%d 通过 / %d 失败" % (ok_n, bad_n))
    return 0 if bad_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
