#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
mkdist.py —— 一次编译产物 → 可直接发出去的几种镜像

    python tools/mkdist.py

做三件事（每改一版都跑一遍，别手工敲 esptool）：
  1. 用 esptool merge_bin 生成 `dist/MyRadio-<版本>-并口@0x0.bin`（整片烧写，0x0）
  2. 另存一份 `dist/MyRadio-<版本>-只刷APP@0x10000.bin`（保留 WiFi / 台表，推荐）
  3. 把 `firmware/` 里那四只（整片 / 只刷APP / bootloader / partitions）同步成这一版，
     并把 dist 里**旧版本**的镜像挪进 `dist/_old/`（不删，能回退）

版本号取自 src/config.h 的 FW_VER，不认命令行参数 —— 免得 bin 名字和固件里印的对不上。
镜像布局（与 platformio.ini / partitions.csv 对应，别改）：
    0x0     bootloader.bin
    0x8000  partitions.bin
    0xe000  boot_app0.bin（framework 自带）
    0x10000 firmware.bin
flash 参数固定 dio / 40MHz / 16MB —— 1.34.0 起就没变过；改板子才需要动。
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, ".pio", "build", "myradio")
DIST = os.path.join(ROOT, "dist")
OLD = os.path.join(DIST, "_old")
FW = os.path.join(ROOT, "firmware")
BOOT_APP0 = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
ESPTOOL = os.path.join(os.path.expanduser("~"), ".platformio", "packages",
                       "tool-esptoolpy", "esptool.py")


def fw_version():
    s = open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+FW_VER\s+"([^"]+)"', s)
    if not m:
        sys.exit("src/config.h 里找不到 FW_VER")
    return m.group(1)


def need(p):
    if not os.path.isfile(p):
        sys.exit("找不到 %s —— 先跑 pio run" % p)
    return p


def main():
    ver = fw_version()
    app = need(os.path.join(BUILD, "firmware.bin"))
    boot = need(os.path.join(BUILD, "bootloader.bin"))
    part = need(os.path.join(BUILD, "partitions.bin"))
    need(BOOT_APP0)

    os.makedirs(DIST, exist_ok=True)
    os.makedirs(OLD, exist_ok=True)

    merged = os.path.join(DIST, "MyRadio-%s-并口@0x0.bin" % ver)
    cmd = [sys.executable, ESPTOOL, "--chip", "esp32s3", "merge_bin",
           "-o", merged,
           "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "16MB",
           "0x0", boot,
           "0x8000", part,
           "0xe000", BOOT_APP0,
           "0x10000", app]
    print("merge_bin 0x0 …")
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        sys.exit("merge_bin 失败")

    dst_app = os.path.join(DIST, "MyRadio-%s-只刷APP@0x10000.bin" % ver)
    shutil.copy2(app, dst_app)

    os.makedirs(FW, exist_ok=True)
    shutil.copy2(merged, os.path.join(FW, "固件-整片烧写-0x0.bin"))
    shutil.copy2(app, os.path.join(FW, "固件-只刷APP-0x10000.bin"))
    shutil.copy2(boot, os.path.join(FW, "bootloader.bin"))
    shutil.copy2(part, os.path.join(FW, "partitions.bin"))

    # 旧版本的 dist 镜像归档（保留 inventory，不删）
    for n in sorted(os.listdir(DIST)):
        if not n.startswith("MyRadio-") or not n.endswith(".bin"):
            continue
        if ("-%s-" % ver) in n:
            continue
        shutil.move(os.path.join(DIST, n), os.path.join(OLD, n))
        print("归档 %s -> dist/_old/" % n)

    # 固件里必须只印着一个版本号，且没有已知死域名残留
    raw = open(app, "rb").read()
    bad = [b"jt.ximalaya", b"mobile.ximalaya.com/mobile/redirect"]
    print("\n版本串：%s ×%d   旧版本残留 checked" % (ver, raw.count(ver.encode())))
    for b in bad:
        print("  %-42s %d" % (b.decode(), raw.count(b)))

    print("")
    for p in [merged, dst_app, os.path.join(FW, "固件-整片烧写-0x0.bin"),
              os.path.join(FW, "固件-只刷APP-0x10000.bin"),
              os.path.join(FW, "bootloader.bin"), os.path.join(FW, "partitions.bin")]:
        raw = open(p, "rb").read()
        print("%-46s %9d B  sha256 %s" %
              (os.path.relpath(p, ROOT), len(raw), hashlib.sha256(raw).hexdigest()[:16]))


if __name__ == "__main__":
    main()
