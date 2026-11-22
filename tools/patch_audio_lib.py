#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
给 lib/ESP32-audioI2S/src/Audio.cpp 打两处补丁（1.16.0 起必须打）。

    python tools/patch_audio_lib.py            # 打补丁（已打过就跳过）
    python tools/patch_audio_lib.py --check    # 只检查，不改文件

为什么需要：
    蜻蜓 FM 的点播音频是 **m4a（AAC-in-MP4）**，但 CDN 的 Content-Type 回的是
    `audio/mpeg`。ESP32-audioI2S 2.0.6 的 `parseContentType()` 只看这个头判断编码，
    于是拿 mp3 解码器去解 aac —— 表现是「连上了、数据在流、就是没声音」。
    补丁 ① 在 `connecttohost()` 里记下「URL 里出现 .m4a」，
    补丁 ② 在 `parseContentType()` 的 CT_MP3 分支里按这个期望值改判 M4A。
    条件不命中时行为与上游完全一致（只多一条「URL 带 .m4a」的分支）。

升级音频库的正确做法：
    1. 把上游新版 src/ 覆盖到 lib/ESP32-audioI2S/src/（上游 2.0.6 地址见 platformio.ini 注释）；
    2. 重跑本脚本（新版里锚点若变了，脚本会直接报「锚点不唯一/找不到」，不会打歪）；
    3. 两个补丁都搜 `MyRadio 1.16.0 补丁` 就能找到。
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
P = os.path.join(ROOT, "lib", "ESP32-audioI2S", "src", "Audio.cpp")

MARK = "MyRadio 1.16.0 补丁"

OLD1 = '        if(endsWith(extension, ".pls"))  m_expectedPlsFmt = FORMAT_PLS;\n'
NEW1 = ('        if(endsWith(extension, ".pls"))  m_expectedPlsFmt = FORMAT_PLS;\n'
        '        // ---- MyRadio 1.16.0 补丁 ① ----\n'
        '        // 蜻蜓点播：第一跳 audio.qtfm.cn 会 302 到 "..._24.m4a?auth_key=..."，\n'
        '        // extension 里确实带 ".m4a"，但尾巴被 %3Fauth_key… 占着，endsWith 判不出来，\n'
        '        // 这里改用 indexOf。只在「URL 里出现 .m4a」时生效，其它台的判断完全不变。\n'
        '        if(m_expectedCodec == CODEC_NONE && indexOf(extension, ".m4a") >= 0) '
        'm_expectedCodec = CODEC_M4A;\n')

OLD2 = '        case CT_MP3:\n            m_codec = CODEC_MP3;\n'
NEW2 = ('        case CT_MP3:\n'
        '            // ---- MyRadio 1.16.0 补丁 ② ----\n'
        '            // 蜻蜓的 m4a 被 CDN 标成 Content-Type: audio/mpeg。URL 已经说是 m4a 就信 URL，\n'
        '            // 否则会按 mp3 解码整个文件（同步字找不到），机器上表现成「这个台没有声音」。\n'
        '            if(m_expectedCodec == CODEC_M4A) { m_codec = CODEC_M4A; }\n'
        '            else                             { m_codec = CODEC_MP3;  }\n')


def main():
    check_only = "--check" in sys.argv

    if not os.path.isfile(P):
        sys.exit("找不到 %s —— 先把上游音频库放到 lib/ESP32-audioI2S/src/" % P)

    t = io.open(P, encoding="utf-8").read()
    done = t.count(MARK)

    if done >= 2:
        print("已打过补丁（%d 处），跳过" % done)
        return
    if done == 1:
        print("警告：只找到 1 处补丁，另一处可能被覆盖了 —— 建议从上游重新覆盖后重跑本脚本")

    if check_only:
        sys.exit("未打补丁（这是 --check 模式，不改文件）")

    for old, new in ((OLD1, NEW1), (OLD2, NEW2)):
        n = t.count(old)
        if n != 1:
            sys.exit("锚点不唯一/找不到（%d 处）：%r\n"
                     "上游可能改过这段，先手工对照再改本脚本。" % (n, old[:50]))
        t = t.replace(old, new)

    io.open(P, "w", encoding="utf-8", newline="\n").write(t)
    print("已打两处补丁：%s" % os.path.relpath(P, ROOT))


if __name__ == "__main__":
    main()
