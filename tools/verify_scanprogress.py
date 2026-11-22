#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_scanprogress.py —— 搜台进度「一直 0%」的两处根因，钉死不再回退

    python tools/verify_scanprogress.py

用户报：「正在搜台一直百分之 0」

两处独立原因，缺一个修都不算修好：
  A. 算法：`s_scanBand * 100 / 4` —— FM 是第一个波段，整个 FM 搜完之前恒为 0
  B. 重绘：`needFreq` 里根本没比「进度」 —— 搜台不动 A.cur/A.fmMode/A.fmFreq，
           于是主区只在开始那一帧画过一次，之后再也不刷新
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


fmc = io.open(os.path.join(SRC, 'fm.cpp'), encoding='utf-8').read()
uic = io.open(os.path.join(SRC, 'ui.cpp'), encoding='utf-8').read()
mainc = io.open(os.path.join(SRC, 'main.cpp'), encoding='utf-8').read()

print('[A] 算法：进度必须按频率插值，不能按波段整块跳')
i = fmc.find('int  fmScanProgress()')
seg = fmc[i:i + 700] if i >= 0 else ''
ok(i >= 0, '找到 fmScanProgress')
ok('s_scanBand * 100 / 4' not in seg, '不再是「波段 × 25」')
ok('within' in seg and 'radioBandLo' in seg, '按本波段内相对位置插值')
ok('p > 99' in seg, '99 封顶（100 只留给真的搜完）')

# 与 config.h / fm.cpp 完全一致：
#   kBands = { LW 153~279, MW 531~1602, FM 64000~108000, SW 2300~26100 }
#   kScanOrder = { RADIO_FM, RADIO_MW, RADIO_LW, RADIO_SW } = { 2, 1, 0, 3 }
BANDS = {0: (153, 279), 1: (531, 1602), 2: (64000, 108000), 3: (2300, 26100)}
ORDER = [2, 1, 0, 3]


def prog(band_idx, cur):
    lo, hi = BANDS[ORDER[band_idx]]
    within = 0
    if hi > lo and cur > lo:
        within = int((cur - lo) * 100 / (hi - lo))
        within = max(0, min(100, within))
    p = (band_idx * 100 + within) // 4
    return max(0, min(99, p))


print('\n[模拟] 第一个波段（FM 64.0→108.0MHz）内进度是否连续上升')
vals = [prog(0, k) for k in range(64000, 108001, 4000)]
print('   ', vals)
ok(vals[0] == 0, '起点是 0%')
ok(vals[-1] >= 20, '到波段顶已接近 25pct，实际 ' + str(vals[-1]) + 'pct')
ok(len(set(vals)) >= 5, '波段内有 ' + str(len(set(vals))) + ' 个不同的百分比，原来只有 1 个')
ok(all(vals[k] <= vals[k + 1] for k in range(len(vals) - 1)), '单调不回落')

print('\n[模拟] 四个波段全程')
allv = []
for b in range(4):
    lo, hi = BANDS[ORDER[b]]
    step = max(1, (hi - lo) // 12)
    allv += [prog(b, k) for k in range(lo, hi + 1, step)]
print('   ', allv)
ok(allv == sorted(allv), '全局单调')
ok(max(allv) <= 99, '永远不会自己报到 100%')
ok(len(set(allv)) > 20, '全程有 ' + str(len(set(allv))) + ' 个不同值，够看出在动')

print('\n[B] 重绘：进度变化必须让 needFreq 成立')
j = uic.find('bool needFreq')
nseg = uic[j:j + 900] if j >= 0 else ''
ok('l_scanPct' in nseg, 'needFreq 里比了进度')
ok('l_scanN' in nseg, 'needFreq 里比了「已找到个数」')
ok('static int  l_scanPct' in uic, '有快照变量 l_scanPct')
ok('l_scanPct = fmScanProgress();' in uic, '每帧会更新快照（否则每 40ms 白重画一次）')

print('\n[C] 屏幕文案：光有百分比不够，要有「已找到几个」')
k = uic.find('正在搜台')
line = uic[k - 200:k + 300] if k >= 0 else ''
ok('已找到' in line and 'fmScanCount()' in line, '显示「正在搜台 X%  已找到 N 个」')

print('\n[D] 顺带守住的（1.46.42）')
ok('scanBackup()' in fmc and 'scanRestoreIfEmpty' in fmc,
   '搜台前备份旧表、搜空就恢复（防「台表变 0 个」）')
ok('fmScanWantSoon(2500)' in mainc, '开机进收音后延后 2.5 秒才搜（避开 WiFi 抢连窗口）')
ok('s_scanModeFail' in fmc, '切波段连续失败会认输（防 POWER_UP 死循环）')
ok('s_scanStall' in fmc, 'SEEK 不前进 30 轮会跳过（防无限转）')


print('\n[E] 搜台收尾必须把芯片调回去（1.46.45 P0）')
fmc2 = io.open(os.path.join(SRC, 'fm.cpp'), encoding='utf-8').read()
ok('static void scanFinishTune()' in fmc2, '有 scanFinishTune()')
# 三条收尾路径都要调
cnt = fmc2.count('scanFinishTune();')
ok(cnt >= 3, 'scanFinishTune() 被调用 ' + str(cnt) + ' 次（搜完/中止/切波段失败）')
i2 = fmc2.find('static void scanFinishTune() {')
body = fmc2[i2:i2 + 900] if i2 >= 0 else ''
ok('s_applied  = -1' in body or 's_applied = -1' in body,
   '用 s_applied = -1 哨兵逼 applyTune 一定执行')
ok('applyTune();' in body, '收尾里立刻 applyTune（顺带打断 SEEK）')
ok('A.fmFreq  = s_khz' in body or 'A.fmFreq = s_khz' in body, '同步 A.fmFreq')
# 只查「真正赋值」的行，注释里提到 s_am 不算（我自己就被注释坑了一次）
real = [l.strip() for l in body.split(chr(10))
        if l.strip().startswith('s_am') and '=' in l]
ok(not real,
   '收尾里**不**预先设 s_am —— 那是 1.46.46 修掉的错（会让 applyTune 跳过切模式）',
   '' if not real else ' 裸赋值: ' + str(real))

print('\n[F] 进度可远程读（1.46.47）')
netc = io.open(os.path.join(SRC, 'net.cpp'), encoding='utf-8').read()
ok('pct' in netc, '/api/fmstations 回 pct')
OK_TAIL = True

print('\n%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
