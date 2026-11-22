# -*- coding: utf-8 -*-
"""verify_time_tz.py —— 1.46.63「屏上时间慢 8 小时」的静态回归

用户原话（2026-11-05）：「收音机时间不对」。
实测：设备 /api/state 报 clock=17:27，本机本地是 01:27、UTC 是 17:27
  ⇒ 拿到的时间是对的，**时区偏移丢了**，整块显示按 UTC 走（星期也跟着错一天）。

真因（Arduino core 2.0.17）：
  configTime(8*3600, 0, ...) → setTimeZone(-28800, 0)
  setTimeZone() 里 `if (daylight != 3600)` 把 daylight=0 也算进 DST 分支，
  拼出 TZ="UTC-8DST-8"（无切换规则 + 假 DST 名）→ newlib 解析失败 → 退回 UTC。

不变量（任一条破掉，时间就会再慢 8 小时）：
  A. 开机时无条件 setenv("TZ","CST-8",1) + tzset()，且早于 WiFi 启动；
  B. 校时用 configTzTime("CST-8", ...)，不许再用 configTime 的偏移版；
  C. 重试间隔 >= 5 分钟（30 秒会把还没回来的 SNTP 请求掐掉）；
  D. 第一次拿到时间要打日志（以后一眼能看出校时成没成）；
  E. 全工程没有第二处 configTime(偏移, ...) 的漏网调用。

注意：本文件里那些「configTime(8*3600, ...)」出现在**注释**里当反面教材，
所以所有代码检查都先剥注释 —— 不然断言会自己把自己判失败（1.46.63 实测踩过）。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(ROOT, 'src/net.cpp')
CFG = os.path.join(ROOT, 'src/config.h')

N_OK = 0
N_BAD = 0


def ok(cond, label, extra=''):
    global N_OK, N_BAD
    if cond:
        N_OK += 1
        print(u'  OK   %s' % label)
    else:
        N_BAD += 1
        print(u'  FAIL %s %s' % (label, extra))


def read(p):
    with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(s):
    """去掉 // 行注释和 /* */ 块注释，字符串字面量原样保留。"""
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(s[i])
                if s[i] == '\\':
                    if i + 1 < n:
                        out.append(s[i + 1])
                        i += 2
                        continue
                elif s[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            j = s.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


net_raw = read(NET)
net = strip_comments(net_raw)
cfg = read(CFG)

print(u'== A. 开机就先钉时区 ==')
i_begin = net.find('void netBegin() {')
i_next = net.find('\nvoid ', i_begin + 10)
body = net[i_begin:(i_next if i_next > 0 else i_begin + 4000)]
ok('setenv("TZ", "CST-8", 1)' in body, u'netBegin 里 setenv TZ=CST-8')
ok('tzset()' in body, u'netBegin 里 tzset()')
i_tz = body.find('setenv("TZ"')
i_wifi = body.find('WiFi.begin')
ok(i_tz >= 0 and (i_wifi < 0 or i_tz < i_wifi), u'时区在 WiFi.begin 之前设好',
   '(tz@%d wifi@%d)' % (i_tz, i_wifi))

print(u'== B. 校时走 configTzTime ==')
ok('configTzTime("CST-8"' in net, u'configTzTime("CST-8", ...) 存在')
ok(not re.search(r'configTime\(\s*8\s*\*\s*3600', net), u'不再用 configTime(8*3600, ...) 偏移版')
ok('ntp.aliyun.com' in net and 'ntp.tencent.com' in net, u'两个国内 NTP 服务器都在')

print(u'== C. 重试间隔 ==')
m = re.search(r's_lastNtp\s*>\s*(\d+)', net)
gap = int(m.group(1)) if m else -1
ok(gap >= 300000, u'校时重试间隔 >= 5 分钟', '(实际 %d ms)' % gap)

print(u'== D. 校时成功日志 ==')
ok(u'时间已同步' in net, u'第一次拿到时间会打印日志')

print(u'== E. 全工程无漏网 ==')
bad = []
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h', '.c')):
            continue
        s = strip_comments(read(os.path.join(dirpath, fn)))
        for mm in re.finditer(r'configTime\(([^;]{0,60})', s):
            arg = mm.group(1)
            if re.search(r'\d+\s*\*\s*3600|\d{4,5}\s*,', arg):
                bad.append('%s: configTime(%s' % (fn, arg.strip()[:40]))
ok(not bad, u'没有别处再用 configTime 偏移版', str(bad))

print(u'== F. 版本号 ==')
m = re.search(r'#define\s+FW_VER\s+"([\d.]+)"', cfg)
ver = m.group(1) if m else '?'
ok(ver == '1.46.63', u'FW_VER = 1.46.63', '(实际 %s)' % ver)

print(u'\n通过 %d / 失败 %d' % (N_OK, N_BAD))
sys.exit(1 if N_BAD else 0)
