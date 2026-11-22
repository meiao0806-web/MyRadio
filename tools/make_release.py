#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把整个工程打成一个 zip，直接发给别人就能用。

    python tools/make_release.py

包含：固件源码、**lib/ 下的音频库（vendored + 补丁，少它编不过）**、手机 App、预览页、
      说明书（md + html）、改动记录、完整台表、全部工具脚本、
      android/（APK 外壳工程，含 keystore，能重打出同一个签名的包）和已编好的 MyRadio-*.apk。
排除：编译缓存、Python 缓存、旧 zip。

zip 里会多一个 MANIFEST.txt，列清包内所有文件和大小。
"""
import datetime
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 要打进去的目录 / 文件（相对工程根）
DIRS = ["src", "tools", "preview", "lib", "android"]   # lib = vendored 的 ESP32-audioI2S（含 m4a 补丁）
                                                       # android = APK 外壳工程（1.19.2 起）
FILES = [
    "README.md",
    "说明书.md",
    "说明书.html",
    "CHANGELOG.md",
    "MyRadio-远程版.html",
    "台表-全国.md",
    "台表-全国.csv",
    "原厂对照报告.md",
    "platformio.ini",
    "partitions.csv",
    ".gitignore",
]

# 任何一级目录名命中就跳过
SKIP_DIRS = {".pio", ".vscode", "__pycache__", ".git", "node_modules", "release", ".idea",
             "build", ".gradle", "outputs"}   # build/.gradle = gradle 的中间产物，几十 MB
SKIP_EXT = {".zip", ".pyc", ".pyo", ".tmp", ".bak", ".bin", ".elf", ".map"}

# 已编译的固件：存在才带上（1.20.0 起会编译了，以前包里只有源码）
# (工程内路径, zip 内路径)
FIRMWARE = [
    # 1.43.0：_dist/ 早已改名 dist/，命名也统一成「版本-用途@偏移」
    ("dist/MyRadio-%s-并口@0x0.bin", "firmware/固件-整片烧写-0x0.bin"),
    (".pio/build/myradio/firmware.bin", "firmware/固件-只刷APP-0x10000.bin"),
    (".pio/build/myradio/bootloader.bin", "firmware/bootloader.bin"),
    (".pio/build/myradio/partitions.bin", "firmware/partitions.bin"),
    # 1.21.1：硬件自检固件（确认 I2S 引脚用）。它不带主版本号，
    #   直接从 firmware/ 里那只已编好的原件再打一遍，别去猜文件名。
    ("firmware/硬件自检-整片烧写-0x0.bin", "firmware/硬件自检-整片烧写-0x0.bin"),
]


def fw_version():
    s = open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+FW_VER\s+"([^"]+)"', s)
    return m.group(1) if m else "0.0.0"


def apk_version():
    """android/app/build.gradle 里的 versionName；没有 android/ 就返回空串。"""
    p = os.path.join(ROOT, "android", "app", "build.gradle")
    if not os.path.isfile(p):
        return ""
    s = open(p, encoding="utf-8").read()
    m = re.search(r'versionName\s+"([^"]+)"', s)
    return m.group(1) if m else ""


def collect():
    """返回 [(绝对路径, zip 内相对路径)]"""
    out = []
    for fn in FILES:
        p = os.path.join(ROOT, fn)
        if os.path.isfile(p):
            out.append((p, fn))
        else:
            print("  ! 缺少 %s（跳过）" % fn)
    for d in DIRS:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            print("  ! 缺少目录 %s（跳过）" % d)
            continue
        for cur, subs, names in os.walk(base):
            subs[:] = [x for x in subs if x not in SKIP_DIRS]
            for n in sorted(names):
                # 下划线开头 = 临时脚本 / 调试产物，不进包
                if n.startswith("_"):
                    continue
                if os.path.splitext(n)[1].lower() in SKIP_EXT:
                    continue
                p = os.path.join(cur, n)
                out.append((p, os.path.relpath(p, ROOT).replace("\\", "/")))
    # 已编译固件（1.20.0 起）
    from_fw = fw_version()
    for src, dst in FIRMWARE:
        src = src.replace("%s", from_fw)
        fp = os.path.join(ROOT, src.replace("/", os.sep))
        if os.path.isfile(fp):
            out.append((fp, dst))
        else:
            print("  ! 缺少固件 %s（跳过，包里就只有源码）" % src)

    # 根目录里已经打好的 APK 一起带上（拿一个包就齐了）
    import glob
    for p in sorted(glob.glob(os.path.join(ROOT, "MyRadio-*.apk"))):
        out.append((p, os.path.basename(p)))
    return sorted(out, key=lambda t: t[1])


def stale_check():
    """打包前提醒：生成物比源数据旧，说明忘了重跑生成脚本"""
    def m(p):
        return os.path.getmtime(p) if os.path.exists(p) else 0

    pairs = [
        (os.path.join(HERE, "台表-精选.md"),
         os.path.join(ROOT, "src", "preset_stations.h"), "gen_preset_curated.py"),
        (os.path.join(ROOT, "src", "preset_stations.h"),
         os.path.join(ROOT, "台表-全国.md"), "gen_stationlist.py"),
        (os.path.join(ROOT, "src", "preset_stations.h"),
         os.path.join(ROOT, "说明书.html"), "gen_manual.py"),
        (os.path.join(ROOT, "src", "ui.cpp"),
         os.path.join(ROOT, "preview", "index.html"), "gen_webpreview.py"),
        (os.path.join(ROOT, "src", "webapp.h"),
         os.path.join(ROOT, "preview", "index.html"), "gen_webpreview.py"),
        (os.path.join(ROOT, "src", "webapp.h"),
         os.path.join(ROOT, "MyRadio-远程版.html"), "gen_remote.py"),
    ]

    # App 源码比 webapp.h 新 → 忘了拼装（这条最容易漏，因为改的是 tools/app/）
    app_dir = os.path.join(HERE, "app")
    app_src = [os.path.join(app_dir, n) for n in ("base.html", "body.html", "style.css")]
    js_dir = os.path.join(app_dir, "js")
    if os.path.isdir(js_dir):
        app_src += [os.path.join(js_dir, n) for n in os.listdir(js_dir)]
    icon_json = os.path.join(HERE, "icons", "lucide.json")
    if os.path.exists(icon_json):
        app_src.append(icon_json)

    warned = False
    for src, gen, fix in pairs:
        if m(src) > m(gen) + 1:
            if not warned:
                print("提醒：以下生成物比源文件旧，建议先重跑生成脚本")
                warned = True
            print("  ! %s  ->  请跑 python tools/%s"
                  % (os.path.relpath(gen, ROOT), fix))

    if max([m(p) for p in app_src] or [0]) > m(os.path.join(ROOT, "src", "webapp.h")) + 1:
        if not warned:
            print("提醒：以下生成物比源文件旧，建议先重跑生成脚本")
        print("  ! src/webapp.h  ->  请跑 python tools/gen_webapp.py")


def _vtuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3])


def main():
    fw = fw_version()
    av = apk_version()
    # 包名取两边较大的号：APK 常单独改（不能盖掉上一版），
    # 但固件号升级时（如 1.20.0 加台表）也必须让包名跟着走。
    ver = fw if _vtuple(fw) >= _vtuple(av or "0") else av
    pkg = "MyRadio-v%s" % ver
    out_dir = os.path.dirname(ROOT)
    out_zip = os.path.join(out_dir, pkg + ".zip")

    stale_check()

    items = collect()
    if not items:
        sys.exit("没有找到要打包的文件")

    total = sum(os.path.getsize(p) for p, _ in items)
    newest = max(os.path.getmtime(p) for p, _ in items)

    manifest = [
        "%s —— 完整工程包" % pkg,
        "",
        "固件版本 : %s（src/config.h 的 FW_VER）" % fw,
        "手机 App : %s  →  MyRadio-%s.apk（随包，自签，Android 6.0 起）"
        % (av or "（无 android 工程）", av or fw),
        "打包时间 : %s" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "文件数   : %d（含本清单）" % (len(items) + 1),
        "解压后   : %.1f MB" % (total / 1024 / 1024),
        "",
        "先看这三样：",
        "  说明书.html      使用说明（双击打开，台表可搜索）",
        "  preview/index.html  固件预览页（双击打开，可拖旋钮 / 试手机 App）",
        "  README.md        技术文档（接线、编译、接口、跨网协议）",
        "",
        "要发给在外地的家人：",
        "  MyRadio-远程版.html  存到手机里（微信收藏 / 文件）就能遥控这台机器。",
        "                      1.19.1 起打开就列出家里那几台机器，点一下就连，一个字不用输",
        "                      （机器开着「免配对」时；关掉了才要手输遥控码）。",
        "",
        "装成手机 App（不想每次输网址）：",
        "  MyRadio-%s.apk  直接装（手机要允许一次「未知来源」）。"
        "在家自动找机器，点一下进界面；在外地打开就自动走远程，不用点任何东西"
        "（1.42.0 起「离线遥控」按钮删了）。" % (av or fw),
        "",
        "!! 首次上机先刷「硬件自检」，确认 I2S 引脚（1.21.1）：",
        "  firmware/硬件自检-整片烧写-0x0.bin  → 写到 0x0，串口 115200 看 [hw] 组 N/10",
        "  听到 1kHz 连续音就记下组号，改 src/config.h 的 PIN_I2S_BCK/LCK/DIN 三行重编。",
        "  不确认这一步，主固件可能看起来正常但没声音（I2S 引脚错）。",
        "  详见 原厂对照报告.md 第二节。",
        "",
        "刷机（1.20.0 起包里带已编译固件 firmware/）：",
        "  整片烧写：固件-整片烧写-0x0.bin → esptool 写到 0x0（会覆盖 WiFi 配置，刷完重配）",
        "  只换固件：固件-只刷APP-0x10000.bin → 写到 0x10000（保留 WiFi 与台表设置，推荐）",
        "  命令：python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x10000 firmware/固件-只刷APP-0x10000.bin",
        "",
        "改手机 App 的人注意：源码在 tools/app/，改完要跑",
        "  python tools/gen_webapp.py      生成 src/webapp.h（固件里那份，别手改）",
        "",
        "1.19.0 的电量与播报：",
        "  电量低于 15% 念「电量低，请充电」，开路电压掉到 3550mV 撑满 90 秒念「没电了，关机」再断 IO15。",
        "  两句 MP3 编在 src/voice_data.h（生成物，142KB，别手改）：",
        "    python tools/gen_voice.py        已有 mp3 就只重生成头文件（离线可用）",
        "    python tools/gen_voice.py --tts  改词后重新合成（要联网 + edge-tts）",
        "  电量接线（100k/100k 分压进 IO4）与分压比见 README 3.5d；"
        "1.43.0 起按原厂口径只乘 batfactor=1.71，没有「校准」这一步了；",
        "  首次编译请确认 4MB app 分区装得下（多了约 142KB rodata）。",
        "",
        "改 APK 外壳的人注意：源码在 android/，出包：",
        "  python tools/build_apk.py        编 APK（tools/build_apk.py --icon 顺带重画图标）",
        "  assets/remote.html 由脚本从 MyRadio-远程版.html 复制，别手改；",
        "  重装同一个包必须用 android/myradio.keystore（换签名 = 覆盖安装失败）。",
        "",
        "音频库说明：lib/ESP32-audioI2S 是上游 2.0.6 + 两处补丁（蜻蜓 m4a 被 CDN 标成 mp3）。",
        "  升级上游后重跑 python tools/patch_audio_lib.py；别改回 GitHub 在线依赖。",
        "",
        "-" * 62,
        "文件清单：",
        "",
    ]
    for p, rel in items:
        manifest.append("  %8.1f KB  %s" % (os.path.getsize(p) / 1024, rel))

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(pkg + "/MANIFEST.txt",
                   "\n".join(manifest) + "\n")
        for p, rel in items:
            z.write(p, pkg + "/" + rel)

    print("打包完成")
    print("  输出 : %s" % out_zip)
    print("  版本 : %s" % ver)
    print("  文件 : %d 个，压缩后 %.1f MB（原始 %.1f MB）"
          % (len(items) + 1, os.path.getsize(out_zip) / 1024 / 1024, total / 1024 / 1024))


if __name__ == "__main__":
    main()
