#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_voice.py —— 生成「语音播报」用的中文片段，并内嵌进固件

为什么要有这一步：
  1.19.0 起收音机要在两件事上「说话」：
    · 电量低于 15%  →  电量低，请充电
    · 真没电了     →  没电了，关机
  机器上没有 TTS（也不能指望它联网时才说话 —— 没电时可能正好没网），
  所以把这两句**提前合成好、编进固件**，开机时写进 LittleFS，播放走本地文件。
  两段加起来 30KB 左右：固件里一份（PROGMEM 数组），LittleFS 里一份（播放用）。

产物：
  tools/voice/low.mp3      → src/voice_data.h 里的 kVoLow[]
  tools/voice/off.mp3      → src/voice_data.h 里的 kVoOff[]

用法：
  python tools/gen_voice.py             # 已有 mp3 就只重生成头文件（离线可用）
  python tools/gen_voice.py --tts       # 重新合成（需要联网 + pip install edge-tts）
  python tools/gen_voice.py --list      # 只列出两句话和当前文件大小

注意：
  · 换句子就改下面的 CLIPS，再跑 --tts。改完记得重跑 gen_webapp 之类无关，
    但要重新编译固件（voice_data.h 是 #include 进去的）。
  · 语速略慢一点（-5%）：这机器的用户是老人。
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "src", "voice_data.h")
VOICE_DIR = os.path.join(HERE, "voice")

# id 必须和 src/voice.h 里的 enum 对上（VO_LOW / VO_OFF）
CLIPS = [
    dict(id="low",  cname="kVoLow",  text="电量低，请充电",   desc="电量低于 15% 时播"),
    dict(id="off",  cname="kVoOff",  text="没电了，关机",     desc="电量耗尽、即将关机时播"),
    # 1.40.1：搜台开始时念一句。老人转旋钮发现台表是空的会以为坏了，
    #   给一声「正在搜台」比在屏上写一行字更直接（他可能没在看屏）。
    dict(id="scan", cname="kVoScan", text="正在搜台，请稍等", desc="自动搜台开始时播"),
]

VOICE = "zh-CN-XiaoxiaoNeural"
RATE = "-5%"


def synth(clip):
    """用 edge-tts 合成一段。返回 True/False。"""
    try:
        import asyncio
        import edge_tts
    except ImportError:
        print("  没装 edge-tts：pip install edge-tts")
        return False
    os.makedirs(VOICE_DIR, exist_ok=True)
    path = os.path.join(VOICE_DIR, clip["id"] + ".mp3")

    async def go():
        c = edge_tts.Communicate(clip["text"], VOICE, rate=RATE)
        await c.save(path)

    try:
        asyncio.run(go())
    except Exception as e:
        print("  合成失败：%s: %s" % (type(e).__name__, e))
        return False
    print("  %s  %-28s %5d B" % (clip["id"] + ".mp3", clip["text"], os.path.getsize(path)))
    return True


def emit_header():
    parts = []
    total = 0
    for c in CLIPS:
        p = os.path.join(VOICE_DIR, c["id"] + ".mp3")
        if not os.path.isfile(p):
            sys.exit("缺少 %s，先跑 python tools/gen_voice.py --tts" % p)
        data = open(p, "rb").read()
        total += len(data)
        rows = []
        for i in range(0, len(data), 16):
            rows.append("  " + ",".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        parts.append(
            "// %s —— 「%s」  %d 字节（%s）\n"
            "static const uint8_t %s[] = {\n%s\n};\n"
            % (c["id"] + ".mp3", c["text"], len(data), c["desc"], c["cname"], "\n".join(rows))
        )

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "// ============================================================\n"
            "//  voice_data.h —— 语音播报的 MP3（自动生成，别手改）\n"
            "//\n"
            "//  重新生成： python tools/gen_voice.py --tts\n"
            "//  句子在 tools/gen_voice.py 的 CLIPS 里；改完要重新编译固件。\n"
            "//\n"
            "//  为什么编进固件而不是随附在 LittleFS 镜像里：\n"
            "//    出厂只烧一个固件就得能说话。首次开机 voiceBegin() 会把这些字节\n"
            "//    写进 LittleFS（/v/low.mp3 这类），播放时走 Audio 库的本地文件通路。\n"
            "// ============================================================\n"
            "#pragma once\n#include <stdint.h>\n\n"
            + "\n".join(parts)
            + "\n// 两份加起来 %d 字节\n#define VOICE_BYTES_TOTAL %d\n" % (total, total)
        )
    print("写出 %s  (%.1f KB)" % (OUT, os.path.getsize(OUT) / 1024.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tts", action="store_true", help="重新用 edge-tts 合成 mp3")
    ap.add_argument("--list", action="store_true", help="只列出内容")
    a = ap.parse_args()

    if a.list:
        for c in CLIPS:
            p = os.path.join(VOICE_DIR, c["id"] + ".mp3")
            sz = os.path.getsize(p) if os.path.isfile(p) else -1
            print("%-4s %-22s %-30s %s" % (c["id"], c["text"], c["desc"],
                                           ("%d B" % sz) if sz >= 0 else "还没合成"))
        return

    if a.tts:
        ok = all(synth(c) for c in CLIPS)
        if not ok:
            sys.exit("合成没全成功，头文件用旧的")
    emit_header()


if __name__ == "__main__":
    main()
