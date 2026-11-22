#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""m4a 解码参数回归（1.46.21）。

背景：喜马拉雅的有声书是 m4a（裸 AAC 帧），库必须知道「声道数 / 核心采样率」
才能解码。这两个值只能从 moov→esds→DecoderSpecificInfo(ASC) 里读。

1.46.16 之前写死 2ch/44100，于是「换一本书就兹拉一下重启」。
1.46.16 改成解析 ASC，但只认**单字节长度**（`05 07 ...`），碰上 FFmpeg 打包的
`05 80 80 80 07 ...`（4 字节变长长度）就整本解析失败 → 退回 44100/2ch → 还是崩。

本脚本把真机上崩溃那几本书的 esds 片段当夹具，验证：
  ① 新的 ASC 解析器（含变长长度）能解出正确参数；
  ② 旧解析器确实解不出 `官场现形记`（证明这不是「没坏别修」）；
  ③ findNextSync 的采样率取值：ASC 说的算，不看容器率；
  ④ 换歌时 stopSong 会把参数清掉（不清就是「换一本崩一本」）。

用法：python tools/verify_m4a.py
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP = os.path.join(ROOT, "lib", "ESP32-audioI2S", "src", "Audio.cpp")

# ---- 真机夹具：从「会重启」的那几本书里抠出来的 esds 附近 80 字节 -------------
# 每段都以 ...esds 开头，后面跟着 ES_Descriptor / DecoderConfig / 0x05。
FIX = {
    #                    期望 aot, sfi, ch
    "三国演义": ("ac4400000000002c6573647300000000031e000100041640150000000000fa030000fa03050713"
                 "8856e5a548800601020000002073747473000000000000000200002fc5000008000000000100"
                 "000342", (2, 7, 1)),
    "上下五千年": ("ac4400000000002a6573647300000000031c000100041440150000000000fa010000fa01050513"
                   "9056e5a00601020000002073747473000000000000000200001bef000008000000000100"
                   "0006c20000", (2, 7, 2)),
    "一路高升": ("ac4400000000002a6573647300000000031c000100041440150000000000fa020000fa02050513"
                 "9056e5a00601020000002073747473000000000000000200001f97000008000000000100"
                 "0005930000", (2, 7, 2)),
    # 官场现形记：长度字段是 4 字节形式（03 80 80 80 27 / 05 80 80 80 07）
    "官场现形记": ("ac4400000000003865736473000000000380808027000100048080801940150000000000bb"
                   "800000bb800580808007138856e5a548800680808001020000002073747473000000000000"
                   "000200001f9a", (2, 7, 1)),
}

RATE_TAB = [96000, 88200, 64000, 48000, 44100, 32000,
            24000, 22050, 16000, 12000, 11025, 8000, 7350]

ok_n = bad_n = 0


def chk(name, got, want):
    global ok_n, bad_n
    if got == want:
        ok_n += 1
        print("  OK   %s" % name)
    else:
        bad_n += 1
        print("  FAIL %s  期望 %r 实际 %r" % (name, want, got))


def asc_scan(buf, old=False):
    """等价于 Audio.cpp 里的 m4aAscScan()。old=True 复刻 1.46.16 的单字节版本。"""
    n = len(buf)
    for i in range(n - 8):
        if buf[i:i + 4] != b"esds":
            continue
        lim = min(i + 260, n)
        for k in range(i + 8, lim - 3):
            if buf[k] != 0x05:
                continue
            if old:
                alen = buf[k + 1]
                p = k + 2
                if alen < 2 or alen > 8 or p + alen > n:
                    continue
            else:
                p = k + 1
                if p < n and (buf[p] & 0x80):
                    if p + 3 >= n:
                        continue
                    alen = (((buf[p] & 0x7F) << 21) | ((buf[p + 1] & 0x7F) << 14) |
                            ((buf[p + 2] & 0x7F) << 7) | (buf[p + 3] & 0x7F))
                    p += 4
                else:
                    if p >= n:
                        continue
                    alen = buf[p]
                    p += 1
                if alen < 2 or alen > 8 or p + alen > n:
                    continue
            a = buf[p:p + alen]
            if len(a) < 2:
                continue
            aot = (a[0] >> 3) & 0x1F
            if not (1 <= aot <= 5 or aot == 29):
                continue
            sfi = ((a[0] & 0x07) << 1) | ((a[1] & 0x80) >> 7)
            chc = (a[1] >> 3) & 0x0F
            return aot, sfi, chc
    return None


def params(aot, sfi, chc):
    """等价于 read_M4A_Header 里写进 m_aac* 的那三个值。"""
    base = RATE_TAB[sfi] if sfi < 13 else 0
    ch = chc if chc in (1, 2) else 0
    return base, ch


def sr_for(base, container):
    """等价于 findNextSync 的 sr 选择（1.46.21 新写法 vs 1.46.16 旧写法）。"""
    if base > 0:
        return base                       # 新：ASC 说的算
    return 44100


def sr_for_old(base, aot, container):
    sr = 44100
    if base > 0:
        out = container or 44100
        if aot in (5, 29):
            sr = base
        elif base * 2 == out:
            sr = base
    return sr


def main():
    cpp = io.open(CPP, encoding="utf-8").read()

    print("=== 1) 真实夹具：新解析器能解出来 ===")
    for name, (hx, want) in FIX.items():
        got = asc_scan(bytes.fromhex(hx))
        chk("%s → aot/sfi/ch = %s" % (name, want), got, want)

    print("=== 2) 旧解析器在 4 字节长度上会失手（证明修复有意义）===")
    got = asc_scan(bytes.fromhex(FIX["官场现形记"][0]), old=True)
    chk("1.46.16 解析器解不出 官场现形记", got, None)
    got = asc_scan(bytes.fromhex(FIX["三国演义"][0]), old=True)
    chk("1.46.16 解析器能解出 三国演义（单字节长度）", got, FIX["三国演义"][1])

    print("=== 3) 采样率取值 ===")
    chk("base=22050 / 容器 44100 → 22050", sr_for(22050, 44100), 22050)
    # 这条是 1.46.16 的窟窿：容器率本身就是 22050 时，旧写法反而停在 44100
    chk("base=22050 / 容器 22050 → 22050（旧写法会给 44100）",
        sr_for(22050, 22050), 22050)
    chk("旧写法在容器 22050 上确实给 44100（对照）",
        sr_for_old(22050, 2, 22050), 44100)
    chk("没解析到 ASC → 保持 44100", sr_for(0, 44100), 44100)

    print("=== 4) 源码级：换歌必须复位 m4a 参数 ===")
    m = re.search(r"uint32_t Audio::stopSong\(\)\s*\{(.{0,1600}?)\n\}", cpp, re.S)
    body = m.group(1) if m else ""
    for f in ("m_aacAot      = 0", "m_aacBaseRate = 0", "m_aacChans    = 0"):
        chk("stopSong 里 %s" % f.strip(), f in body.replace("=0", "= 0"), True)
    chk("stopSong 里清 m_m4aTry", "m_m4aTry" in body, True)
    chk("stopSong 里清 m_noProgress", "m_noProgress" in body, True)

    print("=== 5) 源码级：ASC 扫描支持变长长度 + aot 29 ===")
    chk("有 0x80 变长长度分支", "data[p] & 0x80" in cpp, True)
    chk("放行 aot==29（HE-AAC v2）", "aot != 29" in cpp, True)
    chk("不再有单字节长度老写法", "int alen = data[k+1];" in cpp, False)

    print("=== 6) 源码级：无进展兜底有三档 ===")
    chk("三档参数表 tRate", "{ 22050, 22050, 44100 }" in cpp, True)
    chk("三档声道表 tCh", "{ 1, 2, 2 }" in cpp, True)
    chk("阈值降到 60（原来 300 太慢）", "> 60) {" in cpp, True)

    print("\n%d 通过 / %d 失败" % (ok_n, bad_n))
    return 0 if bad_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
