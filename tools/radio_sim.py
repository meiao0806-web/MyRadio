# -*- coding: utf-8 -*-
"""
radio_sim.py —— 离线复刻 src/fm.cpp 的「波段环」，不用板子也不用浏览器。

复刻的三件事：
  1. radioBand / radioBandLo / radioBandHi / radioBandStep  —— 波段表
  2. radioSnap / radioWalk                                  —— 跨段与绕圈
  3. radioGear                                              —— 转速 → 加速倍数

再顺手把 ui.cpp 的显示格式（调频「FM 98.0」/ 中波「中波 981」）也钉住。

用法：
  python tools/radio_sim.py            跑断言（通过打印「通过 N / 失败 M」）
  python tools/radio_sim.py --vectors  额外写出 tools/_radio_vectors.json
                                       给 tools/verify_radio.js 用同一批向量
"""
import json
import os
import sys

# ---- 与 config.h 一一对应（改那边记得改这里；本文件只是复刻）----
LW, MW, FM, SW = 0, 1, 2, 3
BANDS = [
    dict(k='LW', lo=153,   hi=279,    step=9),     # 长波
    dict(k='MW', lo=531,   hi=1602,   step=9),     # 中波（中国 9kHz 频道栅格）
    dict(k='FM', lo=64000, hi=108000, step=100),   # 调频 0.1MHz
    dict(k='SW', lo=2300,  hi=26100,  step=5),     # 短波
]
START = 98000

fails = []
oks = 0


def check(cond, msg):
    global oks
    if cond:
        oks += 1
    else:
        fails.append(msg)


def band_of(khz):
    for i, b in enumerate(BANDS):
        if b['lo'] <= khz <= b['hi']:
            return i
    near = closest(khz)
    for i, b in enumerate(BANDS):
        if b['lo'] <= near <= b['hi']:
            return i
    return 0


def closest(khz):
    best, bd = BANDS[0]['lo'], None
    for b in BANDS:
        for e in (b['lo'], b['hi']):
            d = abs(khz - e)
            if bd is None or d < bd:
                bd, best = d, e
    return best


def snap(khz):
    for b in BANDS:
        if b['lo'] <= khz <= b['hi']:
            return khz
    return closest(khz)


def walk(khz, clicks, gear=1):
    k = snap(khz)
    bi = band_of(k)
    gear = max(1, gear)
    n = min(abs(clicks), 400)
    for _ in range(n):
        st = BANDS[bi]['step'] * gear
        if clicks > 0:
            v = k + st
            if v > BANDS[bi]['hi']:
                bi = (bi + 1) % len(BANDS)
                v = BANDS[bi]['lo']
            k = v
        else:
            v = k - st
            if v < BANDS[bi]['lo']:
                bi = (bi + len(BANDS) - 1) % len(BANDS)
                v = BANDS[bi]['hi']
            k = v
    return k


def gear(ms_per_click, khz=None):
    if ms_per_click <= 60:
        return 30
    if ms_per_click <= 130:
        return 10
    if ms_per_click <= 400:
        return 5
    return 1


def band_word(khz):
    return {'LW': u'长波', 'MW': u'中波', 'SW': u'短波'}.get(BANDS[band_of(khz)]['k'], u'调频')


def freq_text(khz):
    if BANDS[band_of(khz)]['k'] == 'FM':
        return 'FM %.1f' % (khz / 1000.0)
    return '%s %d' % (band_word(khz), khz)


def in_gap(khz):
    return not any(b['lo'] <= khz <= b['hi'] for b in BANDS)


# ============================================================
#  1. 波段表本身
# ============================================================
for b in BANDS:
    check((b['hi'] - b['lo']) % b['step'] == 0,
          u'%s 的上下边界必须落在自己的栅格上：(%d-%d)%%%d != 0'
          % (b['k'], b['hi'], b['lo'], b['step']))

check(band_of(198) == LW, u'198kHz 应属长波')
check(band_of(981) == MW, u'981kHz 应属中波')
check(band_of(6055) == SW, u'6055kHz 应属短波')
check(band_of(98000) == FM, u'98000kHz 应属调频')
check(band_of(1000) == MW, u'1000kHz 应属中波')
check(band_of(64000) == FM, u'64000（64.0MHz）应属调频')
check(band_of(450) == MW, u'空档 450kHz 应吸到最近的中波（离 531 更近）')
check(band_of(400) == LW, u'空档 400kHz 应吸到最近的长波（离 279 更近）')

# ============================================================
#  2. 跨段：边界再走一格直接进下一个波段（中间空档不占格子）
# ============================================================
check(walk(1602, +1, 1) == 64000, u'中波最高端再上一格 → 调频最低端')
check(walk(64000, -1, 1) == 1602, u'调频最低端再下一格 → 中波最高端（老人最常走的一步）')
check(walk(108000, +1, 1) == 2300, u'调频最高端再上一格 → 短波最低端')
check(walk(2300, -1, 1) == 108000, u'短波最低端再下一格 → 调频最高端')
check(walk(26100, +1, 1) == 153, u'短波最高端再上一格 → 绕回长波最低端')
check(walk(153, -1, 1) == 26100, u'长波最低端再下一格 → 绕回短波最高端')
check(walk(279, +1, 1) == 531, u'长波最高端再上一格 → 中波最低端')
check(walk(531, -1, 1) == 279, u'中波最低端再下一格 → 长波最高端')

# ============================================================
#  3. 一格一格走满一圈，正好回到起点
# ============================================================
steps_per_band = [(b['hi'] - b['lo']) // b['step'] + 1 for b in BANDS]
ring_len = sum(steps_per_band)
check(ring_len == 15 + 120 + 441 + 4761, u'整圈格数 = %d（长波15+中波120+调频441+短波4761）' % ring_len)

k = 531
for _ in range(ring_len):
    k = walk(k, +1, 1)
check(k == 531, u'从 531 一格一格走 %d 格应回到 531，实际 %d' % (ring_len, k))

k = 531
for _ in range(ring_len):
    k = walk(k, -1, 1)
check(k == 531, u'反方向走满一圈也应回到 531，实际 %d' % k)

# ============================================================
#  4. 随便怎么转都不会停在空档里（2000 次随机）
# ============================================================
import random
random.seed(20261101)
bad = 0
for _ in range(2000):
    st = random.choice([153, 531, 981, 1602, 2300, 6055, 26100, 64000, 98000, 108000])
    k = walk(st, random.choice([-40, -7, -1, 1, 3, 12, 40]),
             random.choice([1, 5, 10, 30]))
    if in_gap(k):
        bad += 1
check(bad == 0, u'随机 2000 次转动落在空档的次数 = %d（应为 0）' % bad)

# 单步：任何波段的任何一格都合法
bad = 0
for b in BANDS:
    k = b['lo']
    for _ in range((b['hi'] - b['lo']) // b['step'] + 1):
        if in_gap(k) or band_of(k) != BANDS.index(b):
            bad += 1
        k = walk(k, +1, 1)
check(bad == 0, u'逐格走满四个波段，越界/串段 = %d（应为 0）' % bad)

# ============================================================
#  5. 加速档位（用户要求：快转一格 10~30 个频道）
# ============================================================
check(gear(800) == 1, u'慢慢转（800ms/格）→ 1 倍，精调')
check(gear(200) == 5, u'正常速度（200ms/格）→ 5 倍')
check(gear(100) == 10, u'快转（100ms/格）→ 10 倍')
check(gear(40) == 30, u'甩起来（40ms/格）→ 30 倍')

for b in BANDS:
    g10 = gear(100) * b['step']
    g30 = gear(40) * b['step']
    check(g10 > 0 and g30 > g10, u'%s 快转步长 %d kHz < 甩动步长 %d kHz' % (b['k'], g10, g30))

check(gear(40) * BANDS[MW]['step'] == 270, u'中波甩一格 = 270kHz（30 个频道）')
check(gear(100) * BANDS[MW]['step'] == 90, u'中波快转一格 = 90kHz（10 个频道）')
check(gear(40) * BANDS[FM]['step'] == 3000, u'调频甩一格 = 3MHz')
check(gear(100) * BANDS[FM]['step'] == 1000, u'调频快转一格 = 1MHz（10 个频道）')
check(gear(40) * BANDS[SW]['step'] == 150, u'短波甩一格 = 150kHz')

# 中波整段：正常速度几下就走完了（老人不用一格一格挪）
# 注意 gear=5 时步长 45kHz，1602 这个端点会被跳过直接进调频 —— 这是有意的（粗调不抠端点）
clicks = 1
k = walk(BANDS[MW]['lo'], +1, 5)
while band_of(k) == MW and clicks < 200:
    k = walk(k, +1, 5)
    clicks += 1
check(band_of(k) == FM, u'中波正常速度往上转，应一路转到调频（实际落在 %d）' % k)
check(clicks <= 25, u'中波 531→（走出波段）正常速度只要 %d 格（一格一个频道要 119 格）' % clicks)

# ============================================================
#  6. 显示格式（ui.cpp / core.js / 预览页三处同一规则）
# ============================================================
check(freq_text(98000) == 'FM 98.0', u'98000 → %s' % freq_text(98000))
check(freq_text(64000) == 'FM 64.0', u'64000 → %s' % freq_text(64000))
check(freq_text(981) == u'中波 981', u'981 → %s' % freq_text(981))
check(freq_text(198) == u'长波 198', u'198 → %s' % freq_text(198))
check(freq_text(6055) == u'短波 6055', u'6055 → %s' % freq_text(6055))

# ============================================================
#  7. 开机默认
# ============================================================
check(snap(START) == 98000, u'开机默认 98.0MHz 应原样落在调频')
check(band_of(START) == FM, u'开机默认应属调频')
for x in (0, 100, 300, 2000, 30000, 200000):
    check(not in_gap(snap(x)), u'越界值 %d 应被吸进波段（吸到 %d）' % (x, snap(x)))

# ============================================================
#  输出
# ============================================================
vectors = []
for st in (153, 279, 531, 981, 1602, 2300, 6055, 26100, 64000, 98000, 108000):
    for cl in (-30, -7, -1, 1, 5, 30):
        for g in (1, 5, 10, 30):
            vectors.append({'khz': st, 'clicks': cl, 'gear': g, 'want': walk(st, cl, g)})
for ms, want in ((40, 30), (60, 30), (61, 10), (130, 10), (131, 5), (400, 5), (401, 1), (2000, 1)):
    vectors.append({'ms': ms, 'gear': gear(ms), 'want': want})

if '--vectors' in sys.argv:
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_radio_vectors.json')
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(vectors, f, ensure_ascii=False)
    print(u'向量写出 %s（%d 条）' % (out, len(vectors)))

print(u'通过 %d / 失败 %d' % (len(fails) and oks or oks, len(fails)))
for m in fails:
    print(u'  ✗ ' + m)
sys.exit(1 if fails else 0)
