# -*- coding: utf-8 -*-
"""verify_app_nav.py —— App 1.26.6「不再停在网页无法打开」的静态回归

用户原话（2026-11-05 截图）：
    「app 显示这个」 —— http://192.168.4.1/ net::ERR_CONNECTION_TIMED_OUT

不变量（这几条任意一条破掉，那个报错页就会回来）：
  A. 开机不许「盲跳」上次地址，必须先探活；热点地址 192.168.4.1 不许当 lastIp；
  B. onReceivedError 一律接管，不许带时间窗；
  C. 进机器页只有 openDevice 一个入口，且带 6 秒看门狗；
  D. 够不着就退到跨网遥控（有 lastIp）或首页（没配过网）；
  E. 三处版本号一致，出包后的 dex 里没有旧版本残留。
"""
import io
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, 'android/app/src/main/java/com/myradio/app/MainActivity.java')
GRADLE = os.path.join(ROOT, 'android/app/build.gradle')

N_OK = 0
N_BAD = 0


def ok(cond, label, extra=''):
    global N_OK, N_BAD
    if cond:
        N_OK += 1
        print('  OK   %s' % label)
    else:
        N_BAD += 1
        print('  FAIL %s %s' % (label, extra))


def read(p):
    return io.open(p, encoding='utf-8', errors='replace').read()


m = read(MAIN)
g = read(GRADLE)

ver_gradle = re.search(r'versionName\s+"([^"]+)"', g).group(1)
vc_gradle = re.search(r'versionCode\s+(\d+)', g).group(1)
ver_app = re.search(r'APP_VER = "([^"]+)"', m).group(1)

print('--- A 开机/地址（盲跳 192.168.4.1 的根因）')
ok('< 8000' not in m, '没有「8 秒窗口」那种时间判据')
ok('openDevice(lastIp' in m, '开机走 openDevice（先探活）')
ok('Provision.AP_IP.equals(lastIp)' in m, '开机把热点地址从 lastIp 里划掉')
ok('Provision.AP_IP.equals(ip) ? "" : ip' in m, 'remember() 不把热点地址写进 lastIp')
ok('Provision.AP_IP.equals(v) ? "" : v' in m, 'lastIp() 对外也不吐热点地址')

print('--- B 失败一律接管')
ok(re.search(r'public void onReceivedError[\s\S]{0,900}?fallback\(', m) is not None,
   'onReceivedError 里直接 fallback()')
ok('web.reload()' not in m.split('onReceivedError')[1][:600], '不会在原地 reload 死循环')

print('--- C 单一入口 + 看门狗')
ok(m.count('go("http://" + ip + "/")') == 1, 'go 到机器页只有 1 处（openDevice 内）',
   '实际 %d' % m.count('go("http://" + ip + "/")'))
ok(m.count('openDevice(') >= 6, 'openDevice 调用点 >= 6', '实际 %d' % m.count('openDevice('))
ok('postDelayed(watchdog, 6000)' in m, '6 秒看门狗')
ok(re.search(r'watchdog = new Runnable\(\)[\s\S]{0,400}?fallback\(', m) is not None,
   '看门狗触发 → fallback')
ok('pageLoaded = true' in m and 'pageLoaded = false' in m, 'pageLoaded 有置位/复位')

print('--- D 退路')
ok('load(REMOTE)' in m and 'load(HOME)' in m, '远程页 / 首页两条出口都在')
ok(re.search(r'private void fallback[\s\S]{0,700}?load\(REMOTE\)', m) is not None,
   'fallback 优先切跨网遥控')
ok('openDevice(Provision.AP_IP, "我的收音机", false, true)' in m,
   '配网页「直接打开」够不着时留在原页（stayOnFail）')
ok('手机没连上「我的收音机」热点' in m, '热点没连上有明确提示')

print('--- E 版本')
ok(ver_gradle == ver_app, 'gradle versionName == APP_VER', '%s / %s' % (ver_gradle, ver_app))
ok(tuple(map(int, ver_gradle.split('.'))) >= (1, 26, 6), 'APK 版本 >= 1.26.6', ver_gradle)

apk = os.path.join(ROOT, 'MyRadio-%s.apk' % ver_gradle)
print('--- F 出包核对（%s）' % os.path.basename(apk))
if not os.path.isfile(apk):
    ok(False, 'APK 存在', apk)
else:
    ok(True, 'APK 存在 %.0f KB' % (os.path.getsize(apk) / 1024))
    z = zipfile.ZipFile(apk)
    dex = z.read('classes.dex')
    ok(dex.count(ver_gradle.encode()) >= 1, 'dex 里有 %s' % ver_gradle)
    ok(dex.count(b'openDevice') >= 1 and dex.count(b'watchdog') >= 1,
       'dex 里有 openDevice / watchdog（不是旧壳）')
    old = [v for v in re.findall(rb'1\.2[0-9]\.[0-9]', dex) if v.decode() != ver_gradle]
    ok(not old, 'dex 里没有别的 1.2x 版本号残留', str(set(x.decode() for x in old))[:80])
    remote = z.read('assets/remote.html')
    disk = open(os.path.join(ROOT, 'MyRadio-远程版.html'), 'rb').read()
    ok(remote == disk, 'assets/remote.html == MyRadio-远程版.html')
    ok(z.getinfo('META-INF/CERT.RSA').file_size > 0, '已签名')

print('\n通过 %d / 失败 %d' % (N_OK, N_BAD))
sys.exit(1 if N_BAD else 0)
