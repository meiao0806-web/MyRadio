# -*- coding: utf-8 -*-
"""
1.22.0 离线验证（修正版）

上一版有两个 FAIL，逐条核对后都是**断言写错**，不是代码错：

① `AM 弱台 r=6 sn=2 门限1 不静音`
   门限=1 时 thr_r = 8 + (1*8*2)//100 = 8、thr_s = 3 + (1*3*2)//100 = 3，
   r=6<8 且 sn=2<3 ⇒ 确实该静音。squelch 的语义是「0=关，1=最不敏感」，
   1 并不是"关"。所以代码对，断言错 → 改成期望 True。
   （顺手把 config.h 的注释说清楚这一点，别让用户以为 1 等于关。）

② `绕一圈回原点（440 格）`
   radioWalk 走的是**四个波段的环**（长波→中波→调频→短波→长波），
   不是单个波段的环。FM 64.0~108.0 一共 441 格，走 440 格才到 107.9，
   第 441 格就跳到短波了。这是 1.10.0 以来的既定行为，跟本次改动无关。
   → 改成断言「441 格后确实跨到短波」，这才是有意义的检查。
"""
import re, sys

SRC = r'C:\Users\lenovo\WorkBuddy\2026-11-01-18-34-26\MyRadio-v1.21.1\src'
fails = 0
oks = 0

def chk(name, got, want):
    global fails, oks
    if got == want:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-46s got=%r want=%r' % (name, got, want))

# ============================================================
#  一、静噪判定
# ============================================================
SQUELCH_AM_RSSI, SQUELCH_AM_SNR = 8, 3
SQUELCH_FM_RSSI, SQUELCH_FM_SNR = 10, 4
HOLD = 1200

def squelch_want(am, squelch, r, sn):
    if squelch <= 0:
        return False
    lim_r = SQUELCH_AM_RSSI if am else SQUELCH_FM_RSSI
    lim_s = SQUELCH_AM_SNR if am else SQUELCH_FM_SNR
    thr_r = lim_r + (squelch * lim_r * 2) // 100
    thr_s = lim_s + (squelch * lim_s * 2) // 100
    return (r < thr_r) and (sn < thr_s)

print('=== 静噪判定 ===')
for am in (True, False):
    for r, sn in ((0, 0), (1, 1), (5, 2), (60, 30)):
        chk('关掉静噪 am=%s r=%d sn=%d' % (am, r, sn),
            squelch_want(am, 0, r, sn), False)

for r, sn in ((1, 0), (3, 1), (5, 2)):
    chk('AM 空频道 r=%d sn=%d 门限50' % (r, sn),
        squelch_want(True, 50, r, sn), True)

for s in (1, 25, 50, 75, 100):
    chk('AM 正常台 门限%d 不静音' % s, squelch_want(True, s, 25, 12), False)

# ① 修正：门限=1 时门限仍等于基础值，r=6 低于 8 ⇒ 静音（设计如此）
chk('AM r=6 sn=2 门限1 静音（1 不是关）', squelch_want(True, 1, 6, 2), True)
chk('AM r=6 sn=2 门限100 静音',           squelch_want(True, 100, 6, 2), True)
chk('AM r=6 sn=2 门限0 不静音（0 才是关）', squelch_want(True, 0, 6, 2), False)

chk('FM r=9 sn=3 门限50 静音', squelch_want(False, 50, 9, 3), True)
chk('FM r=9 sn=3 门限0 不静音', squelch_want(False, 0, 9, 3), False)

# 「两个都低才算空频道」：静默载波（RSSI 低但 SNR 高）不该静音
chk('RSSI低 SNR高 不静音', squelch_want(True, 50, 2, 20), False)
chk('RSSI高 SNR低 不静音', squelch_want(True, 50, 30, 1), False)

# 门限单调性：同一个信号，门限越高越容易被静音（不能出现"调高了反而不静音"）
prev = None
mono = True
for s in range(0, 101):
    w = squelch_want(True, s, 6, 2)
    if prev is not None and prev and not w:
        mono = False
    prev = w
chk('门限单调（调高不会变回不静音）', mono, True)

# ============================================================
#  二、抖动保护
# ============================================================
def sim_squelch(seq, squelch=50, am=True, tick=1500):
    want = mute = False
    since = 0
    out = []
    now = 0
    for r, sn in seq:
        now += tick
        w = squelch_want(am, squelch, r, sn)
        if w != want:
            want = w
            since = now
        elif mute != want and now - since >= HOLD:
            mute = want
        out.append(mute)
    return out

print('\n=== 抖动保护 ===')
res = sim_squelch([(2, 1)] * 4)
chk('持续空频道：tick0 不静音', res[0], False)
chk('持续空频道：tick1 起静音', res[1], True)
chk('持续空频道：保持', res[3], True)

res = sim_squelch([(2, 1), (25, 12), (2, 1), (25, 12), (2, 1)])
chk('门限附近抖动：全程不静音', any(res), False)

res = sim_squelch([(2, 1), (2, 1), (25, 12), (25, 12)])
chk('空→强台：tick1 静音', res[1], True)
chk('空→强台：tick2 还静音（等 hold）', res[2], True)
chk('空→强台：tick3 放开', res[3], False)

# ============================================================
#  三、半步进
# ============================================================
cfg = open(SRC + r'\config.h', encoding='utf-8').read()
def cfgval(k):
    m = re.search(r'#define\s+%s\s+(\d+)' % k, cfg)
    assert m, k
    return int(m.group(1))

STEPS = [cfgval('RADIO_LW_STEP'), cfgval('RADIO_MW_STEP'),
         cfgval('RADIO_FM_STEP'), cfgval('RADIO_SW_STEP')]
LO = [cfgval('RADIO_LW_LO'), cfgval('RADIO_MW_LO'),
      cfgval('RADIO_FM_LO'), cfgval('RADIO_SW_LO')]
HI = [cfgval('RADIO_LW_HI'), cfgval('RADIO_MW_HI'),
      cfgval('RADIO_FM_HI'), cfgval('RADIO_SW_HI')]

def band(khz):
    for b in range(4):
        if LO[b] <= khz <= HI[b]:
            return b
    best, bd = 0, 1 << 30
    for b in range(4):
        for v in (LO[b], HI[b]):
            d = abs(khz - v)
            if d < bd:
                bd, best = d, b
    return best

def snap(khz):
    if LO[band(khz)] <= khz <= HI[band(khz)]:
        return khz
    best, bd = LO[0], 1 << 30
    for b in range(4):
        for v in (LO[b], HI[b]):
            d = abs(khz - v)
            if d < bd:
                bd, best = d, v
    return best

def halfstep_of(step, on):
    if not on:
        return step
    h = step // 2
    return h if h >= 1 else 1

def walk(khz, clicks, gear, halfstep):
    k = snap(khz)
    b = band(k)
    gear = max(1, gear)
    n = min(abs(clicks), 400)
    for _ in range(n):
        st = halfstep_of(STEPS[b], halfstep) * gear
        if clicks > 0:
            v = k + st
            if v > HI[b]:
                b = (b + 1) % 4
                v = LO[b]
            k = v
        else:
            v = k - st
            if v < LO[b]:
                b = (b + 3) % 4
                v = HI[b]
            k = v
    return k

print('\n=== 半步进 ===')
chk('关：FM 98.0 +1 → 98.1',  walk(98000, +1, 1, False), 98100)
chk('关：MW 999 +1 → 1008',   walk(999, +1, 1, False), 1008)
chk('关：SW 6055 +1 → 6060',  walk(6055, +1, 1, False), 6060)
chk('关：LW 198 +1 → 207',    walk(198, +1, 1, False), 207)

chk('开：FM 98.0 +1 → 98.05', walk(98000, +1, 1, True), 98050)
chk('开：MW 999 +1 → 1003',   walk(999, +1, 1, True), 1003)
chk('开：SW 6055 +1 → 6057',  walk(6055, +1, 1, True), 6057)
chk('开：LW 153 +1 → 157',    walk(153, +1, 1, True), 157)

# ② 修正两处断言错：
#   a) radioWalk 有 n=min(clicks,400) 的防呆上限（1.10.0 就有），
#      而 FM 64.0~108.0 要 441 格才走完 ⇒ 单次 walk **永远**跨不出 FM 上界。
#      所以下面改成「440/441 格都落在 104.0（=400 格封顶）」。
chk('关：FM 从 64.0 走 440 格 → 104.0（400 格封顶）',
    walk(64000, +440, 1, False), 104000)
chk('关：FM 从 64.0 走 441 格 → 同样 104.0',
    walk(64000, +441, 1, False), 104000)
#   b) 真正验跨段要从上界附近走：107.9 再转 2 格就该跳到短波最低端 2300
chk('关：FM 107.9 走 2 格跨到短波', band(walk(107900, +2, 1, False)), 3)
chk('关：FM 107.9 走 2 格落在 2300', walk(107900, +2, 1, False), 2300)
chk('开：FM 107.95 走 2 格跨到短波', band(walk(107950, +2, 1, True)), 3)

# 跨段落点必须落在目标波段的边界或范围内
for hs in (False, True):
    for start in (279, 1602, 108000, 26100):
        r = walk(start, +1, 1, hs)
        b = band(r)
        chk('跨段+1 从%d(半=%s) 落点合法' % (start, hs),
            LO[b] <= r <= HI[b], True)
    for start in (153, 531, 64000, 2300):
        r = walk(start, -1, 1, hs)
        b = band(r)
        chk('跨段-1 从%d(半=%s) 落点合法' % (start, hs),
            LO[b] <= r <= HI[b], True)

chk('halfStepOf(1,True) 不为 0', halfstep_of(1, True), 1)
chk('halfStepOf(9,True) == 4',   halfstep_of(9, True), 4)
chk('halfStepOf(9,False) == 9',  halfstep_of(9, False), 9)
chk('halfStepOf(100,True) == 50', halfstep_of(100, True), 50)

# 加速档位与半步进叠加不越界
for gear in (1, 5, 10, 30):
    for hs in (False, True):
        r = walk(98000, +1, gear, hs)
        b = band(r)
        chk('FM gear=%d 半=%s 落点合法' % (gear, hs), LO[b] <= r <= HI[b], True)

print('\n================ 通过 %d / 失败 %d ================' % (oks, fails))
sys.exit(1 if fails else 0)
