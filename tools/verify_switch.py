#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_switch.py —— 守住 1.46.39「能查、能自愈、静噪不误伤」这三件事

    python tools/verify_switch.py

用户这一轮报的六条里，有四条属于「发生时没有网络、事后再也查不到」类：
  · 不联网的频道接收没有声音
  · 连上/连不上网的切换速度慢
  · 断网后再有网，没有回到网络广播
所以这一版的重点不是「猜一个修法」，而是**先把证据留下来**：

  1. 事件日志（evlog）覆盖了 WiFi/模式/搜台/静噪这些关键点，能远程 /api/log 读
  2. /api/fmprobe 能主动读出芯片真实状态（RSSI/SNR/硬静音/音量）
  3. 退出收音有自愈（两次 tick 还在收音模式就强制退）
  4. 静噪对 FM 不再放到 3 倍门限（会把正常台整片压死）
  5. 静噪真的压住声音时，屏上写「无信号」而不是照旧显示台名
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

oks = fails = 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  + ' + msg + (('  ' + extra) if extra else ''))
    else:
        fails += 1
        print('  X ' + msg + (('  ' + extra) if extra else ''))


def rd(f):
    return io.open(os.path.join(SRC, f), encoding='utf-8').read()


mainc = rd('main.cpp')
netc = rd('net.cpp')
fmc = rd('fm.cpp')
fmh = rd('fm.h')
uic = rd('ui.cpp')
conf = rd('config.h')

print('[1] 事件日志（断网那段时间的唯一证据）')
ev_h = os.path.join(SRC, 'evlog.h')
ev_c = os.path.join(SRC, 'evlog.cpp')
ok(os.path.isfile(ev_h) and os.path.isfile(ev_c), 'evlog.h / evlog.cpp 都在')
ev = io.open(ev_c, encoding='utf-8').read() if os.path.isfile(ev_c) else ''
ok('portMUX' in ev, '环形缓冲有自旋锁（audioTask 和主循环都写）')
ok('EV_N' in ev and 'EV_LEN' in ev, '有容量常量')
ok('evlogBoot' in mainc, 'setup 里初始化了（带版本号）')
# 关键插桩点
for tag, where, what in [
    ('WiFi 断/连', mainc, 'evlog("net", "WiFi %s"'),
    ('进收音', mainc, 'evlog("fm", "进收音'),
    ('退收音', mainc, 'evlog("fm", "退回收音'),
    ('切收音的等待时长', mainc, 'evlog("fm", "断网 %lu ms'),
]:
    ok(what in where, '插桩：' + tag)
ok('/api/log' in netc and 'hLog' in netc, '/api/log 路由在')
ok('evlogJson' in netc, 'handler 调了 evlogJson')

print('\n[2] /api/fmprobe（主动读芯片，查「为什么不响」）')
ok('void fmProbe(' in fmc, 'fm.cpp 实现了 fmProbe')
ok('void fmProbe(' in fmh, 'fm.h 声明了 fmProbe')
ok('CMD_GET_PROPERTY' in fmc, '会用 GET_PROPERTY 读硬静音/音量寄存器')
ok('getProp(PROP_RX_HARD_MUTE' in fmc, '读 HARD_MUTE（静音没解开一目了然）')
ok('getProp(PROP_RX_VOLUME' in fmc, '读芯片音量（0 = 当然没声音）')
ok('/api/fmprobe' in netc and 'hFmProbe' in netc, '/api/fmprobe 路由在')
ok('squelchAuto' in netc, '回包里带「是不是被静噪压着」')

print('\n[3] 退出收音的自愈（「断网后再有网没回网络」）')
ok('强制退收音（自愈）' in mainc, '有「连着两次还在收音模式就强制退」')
ok('s_stuck' in mainc, '有卡住计数')

print('\n[4] 静噪不再把 FM 整片压死')
m = re.search(r'const int mul = s_am \? (\d) : (\d);', fmc)
ok(bool(m), '找到了按波段区分的门限倍数')
if m:
    am_mul, fm_mul = int(m.group(1)), int(m.group(2))
    ok(fm_mul == 1, 'FM 的倍数 = %d（原来是 2 → 3 倍门限会把正常台压死）' % fm_mul)
    ok(am_mul == 2, 'AM/SW 的倍数 = %d（那边空频道确实是沙沙，保留压住）' % am_mul)

print('\n[5] 静噪压住声音时屏上必须说出来')
ok('fmSquelched()) return "无信号"' in uic, 'psText() 会返回「无信号」')
ok('fmSquelched()) return "无台"' in uic, '短词版是「无台」')
ok('fmSquelched()) return C_WARN' in uic, '状态色转告警色')

print('\n[6] 切换速度')
mm = re.search(r'#define\s+FM_NET_LOST_MS_RE\s+(\d+)', conf)
ok(bool(mm) and int(mm.group(1)) == 6000,
   '掉线后切收音 = %s ms（1.46.34 是 20000）' % (mm.group(1) if mm else '?'))

print('\n[7] 上一版的成果没被碰坏')
audio = rd('audio.cpp')
ok('!eof && !A.wifiOk' in audio, '断网仍不推进集表（1.46.37）')
ok('fmScanAbort' in mainc and 'fmScanAbort()' in mainc, '退出收音仍会中断搜台（1.46.37）')
ok('savePosNow()' in audio, '有声书位置仍在离开时立即记（1.46.38）')

print('\n%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
