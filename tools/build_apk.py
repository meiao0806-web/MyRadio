#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""build_apk.py —— 一条命令打出 MyRadio 的 Android APK

做的事（顺序固定）：
  1. 把最新的 MyRadio-远程版.html 复制成 assets/remote.html
     （APK 里那份离线遥控页必须和发给用户的网页是同一个文件，别手抄）
  2. 写 android/local.properties 指向本机 SDK
  3. 跑 gradle assembleRelease（零第三方依赖，AGP 8.4.0 + JDK17 + SDK34）
  4. 把产物复制成 MyRadio-<versionName>.apk，放项目根 + 交付目录
  5. 打印 aapt 摘要和 sha256

用法：
  python tools/build_apk.py                 # 正常出包
  python tools/build_apk.py --icon          # 顺带重画桌面图标
  python tools/build_apk.py --no-copy       # 只编译，不往交付目录拷
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AND = os.path.join(ROOT, "android")
OUT_APK = os.path.join(AND, "app", "build", "outputs", "apk", "release", "app-release.apk")
DELIVER = r"C:\Users\lenovo\WorkBuddy AI\2026-11-01-09-11-54\MyRadio"

TOOLS = r"C:\Users\lenovo\WorkBuddy\2026-08-07-21-09-35\shouzhang-app\.build-android"
JDK = os.path.join(TOOLS, "jdk17")
SDK = os.path.join(TOOLS, "sdk")
GRADLE = os.path.join(TOOLS, "gradle-8.6", "bin", "gradle.bat")
AAPT = os.path.join(SDK, "build-tools", "34.0.0", "aapt.exe")


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def version():
    g = read(os.path.join(AND, "app", "build.gradle"))
    m = re.search(r'versionName\s+"([^"]+)"', g)
    return m.group(1) if m else "0.0.0"


def main():
    if "--icon" in sys.argv:
        print("[1/5] 重画图标")
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "gen_apk_icon.py")])
    else:
        print("[1/5] 图标沿用现有 res/mipmap-*")

    print("[2/5] 同步 assets/remote.html")
    src = os.path.join(ROOT, "MyRadio-远程版.html")
    dst = os.path.join(AND, "app", "src", "main", "assets", "remote.html")
    if not os.path.exists(src):
        sys.exit("  缺 MyRadio-远程版.html —— 先跑 python tools/gen_remote.py")
    shutil.copy2(src, dst)
    print("      %d B  (两边一致)" % os.path.getsize(dst))

    for p, what in ((JDK, "jdk17"), (SDK, "sdk"), (GRADLE, "gradle-8.6")):
        if not os.path.exists(p):
            sys.exit("  工具链缺 %s: %s" % (what, p))

    with open(os.path.join(AND, "local.properties"), "w", encoding="utf-8") as f:
        f.write("sdk.dir=" + SDK.replace("\\", "/") + "\n")

    ver = version()
    print("[3/5] gradle assembleRelease  (versionName %s)" % ver)
    env = dict(os.environ)
    env["JAVA_HOME"] = JDK
    env["ANDROID_HOME"] = SDK
    env["ANDROID_SDK_ROOT"] = SDK
    env["PATH"] = os.path.join(JDK, "bin") + os.pathsep + env.get("PATH", "")
    r = subprocess.run([GRADLE, "--no-daemon", "assembleRelease"], cwd=AND, env=env)
    if r.returncode != 0:
        sys.exit("  编译失败，看上面的 gradle 输出")

    if not os.path.exists(OUT_APK):
        sys.exit("  没找到产物 %s" % OUT_APK)

    print("[4/5] 复制产物")
    name = "MyRadio-%s.apk" % ver
    targets = [os.path.join(ROOT, name)]
    if "--no-copy" not in sys.argv and os.path.isdir(DELIVER):
        targets.append(os.path.join(DELIVER, name))
    for t in targets:
        shutil.copy2(OUT_APK, t)
        print("      %s  %d B" % (t, os.path.getsize(t)))

    print("[5/5] 校验")
    if os.path.exists(AAPT):
        o = subprocess.run([AAPT, "dump", "badging", OUT_APK],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        for ln in (o.stdout or "").splitlines():
            if ln.startswith(("package:", "sdkVersion", "targetSdkVersion",
                              "launchable-activity", "application-label")):
                print("      " + ln.strip())
    h = hashlib.sha256(open(OUT_APK, "rb").read()).hexdigest()
    print("      sha256 " + h)
    print("\n完成：%s" % name)


if __name__ == "__main__":
    main()
