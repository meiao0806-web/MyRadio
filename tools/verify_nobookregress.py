#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_nobookregress.py —— 守住 1.46.37 的三条「不许退回去」

背景（用户原话）：
  「有声书听一会调走再回来又从第一集开头开始播了，需要接着播，
    断网后没有立刻切到收频段，收频段搜不到，没有声音噪音也没有」

三条都是「一个变量/一个条件写错了整条路就废」，而且都不会在桌面上跑出来，
所以用静态断言把它们钉住：

  1. 断网时不许推进集表（nextEpOrStation 早退 + 三个调用点都带 A.wifiOk）
  2. 退出收音必须中断搜台（否则 s_scanBusy 永远 true，下次搜不到）
  3. 掉线切收音 6 秒；搜台 0 个台要落到 FM 98.0（别丢在短波末端没声）

    python tools/verify_nobookregress.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

oks = 0
fails = 0


def ok(cond, msg, extra=''):
    global oks, fails
    if cond:
        oks += 1
        print('  + ' + msg + (('  ' + extra) if extra else ''))
    else:
        fails += 1
        print('  X ' + msg + (('  ' + extra) if extra else ''))


def read(f):
    return io.open(os.path.join(SRC, f), encoding='utf-8').read()


audio = read('audio.cpp')
fmc = read('fm.cpp')
fmh = read('fm.h')
mainc = read('main.cpp')
conf = read('config.h')

print('[1] 断网时不许推进集表（这是「又从第一集开头」的根）')
i = audio.find('static void nextEpOrStation(bool eof) {')
nxt = audio[i:i + 2600] if i >= 0 else ''
ok(i >= 0, '找到 nextEpOrStation')
ok('!eof && !A.wifiOk' in nxt,
   '开头有「网络不通就不换集」的早退',
   '' if '!eof && !A.wifiOk' in nxt else '→ 断网会一路 episStep 绕回第 1 集')
# 断网早退必须在 resumeClear / episSetCur 之前
pos_guard = nxt.find('!eof && !A.wifiOk')
pos_clear = nxt.find('resumeClear')
pos_step = nxt.find('episSetCur')
ok(pos_guard >= 0 and (pos_clear < 0 or pos_guard < pos_clear),
   '早退在 resumeClear 之前（断点也不会被清）',
   'guard=%d clear=%d' % (pos_guard, pos_clear))
ok(pos_guard >= 0 and (pos_step < 0 or pos_guard < pos_step),
   '早退在 episSetCur 之前（进度不会被推）',
   'guard=%d step=%d' % (pos_guard, pos_step))

# 三个调用点都要带 A.wifiOk。只挑**带失败计数**的条件 ——
# 别把 `if (s_epiB >= 0) resumeClear(...)` 这类也卷进来。
calls = re.findall(r'if \(s_epiB >= 0[^)]*(?:s_epiFail|s_noiseTries)[^)]*\)', audio)
guarded = [c for c in calls if 'A.wifiOk' in c]
ok(len(calls) >= 3, '找到 %d 处「书 + 失败计数」判断' % len(calls))
ok(len(calls) > 0 and len(guarded) == len(calls),
   '全部 %d 处都带了 A.wifiOk' % len(calls),
   '' if len(guarded) == len(calls) else '裸的：%s' % [c for c in calls if 'A.wifiOk' not in c])

print('\n[2] 退出收音必须中断搜台（这是「搜不到、没声没噪音」的根）')
ok('void fmScanAbort()' in fmc, 'fm.cpp 有 fmScanAbort()')
ok('void fmScanAbort();' in fmh, 'fm.h 声明了 fmScanAbort()')
j = mainc.find('static void exitFm()')
exitfm = mainc[j:j + 700] if j >= 0 else ''
ok('fmScanAbort()' in exitfm, 'exitFm() 里调了 fmScanAbort()',
   '' if 'fmScanAbort()' in exitfm else '→ s_scanBusy 会永远卡 true')
k = mainc.find('static void enterFm()')
enterfm = mainc[k:k + 700] if k >= 0 else ''
ok('fmScanAbort' in enterfm, 'enterFm() 也先清一次（双保险）')
# abort 必须能清 busy
ab = fmc[fmc.find('void fmScanAbort()'):][:400]
ok('s_scanBusy = false' in ab, 'abort 真的把 s_scanBusy 置 false')
ok('s_scanN > 0' in ab and 'fmScanSave' in ab, 'abort 保留已搜到的台并落盘')
# 超时兜底
st = fmc[fmc.find('void fmScanStart()'):][:900]
ok('120000' in st or '120000u' in st, 'fmScanStart 有 120 秒卡死兜底')
ok('fmScanAbort()' in st, '卡住时会强制重来')
ok('芯片没就绪' in st, '早退有日志（不再一声不响）')

print('\n[3] 掉线切收音的时间 + 搜台 0 个台的兜底')
m = re.search(r'#define\s+FM_NET_LOST_MS_RE\s+(\d+)', conf)
val = int(m.group(1)) if m else -1
ok(val == 6000, 'FM_NET_LOST_MS_RE = %d（要 6000）' % val,
   '' if val == 6000 else '→ 20 秒显然不是「立刻」')
ok('98000' in fmc, '搜台 0 个台时落到 FM 98.0（别丢在短波末端）')
# 1.46.45 起这条兜底挪进了 scanFinishTune()，日志也换了说法
ok('一个台都没有' in fmc or '一个台都没搜到' in fmc,
   '这条路径有注释/日志，现场能查')

print('\n[4] 别把「调走再回来接着播」的路拆了')
si = audio.find('static void startIndex(')
startidx = audio[si:si + 2600] if si >= 0 else ''
ok('episCur(bk)' in startidx, '起播用 episCur(bk)（上次听到那一集）')
ok('episTakeNext(bk)' in startidx, '「下次从第 X 集」仍然只取一次')

print('\n%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
