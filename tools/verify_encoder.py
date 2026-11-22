#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_encoder.py —— 用真编码器表离线跑取格判据，验证「一格一台」

    python tools/verify_encoder.py

为什么值得写：旋钮问题只能靠手拧复现，而「丢格」和「抖动多算」是**相反**的两个方向，
改了 A 很容易坏了 B。这里把 encIsr 的判据原样复刻到 python，喂各种真实会发生的
边沿序列，两个方向一起验：

  · 正常一格（4 个边沿）         → 必须走 1 格
  · 中断丢一个中间态（对角跳变） → 修完必须仍走 1 格（1.46.49 的核心）
  · 原地抖动 / 转出去又转回来    → 必须走 0 格（1.46.49 也收紧了这条）
  · 连续快转 N 格                → 必须走 N 格
"""
import sys

# 与 input.cpp 完全一致：索引 = (prev<<2)|ab，prev/ab = (A<<1)|B
TBL = [0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0]
DET = 3          # 真机 /api/enc: detHex = 11


def run(seq, verbose=False):
    """把一个 AB 序列喂进判据，返回兑现的格数（带符号）"""
    phase = 0
    mark = 0
    arm = 0
    skip = 0
    steps = 0
    ab_prev = seq[0]
    detail = []

    for ab in seq[1:]:
        prev = ab_prev
        ab_prev = ab
        if ab == prev:
            continue
        st = TBL[(prev << 2) | ab]
        if st == 0:
            skip = 1                      # 对角跳变：丢了一个中间态
            continue
        phase += st

        if ab == (DET ^ 0x03):            # 走到对侧
            arm = 1
        elif ab == DET:                   # 回到停位 → 结算
            net = phase - mark
            fired = False
            d = 0
            if net == 0:
                if arm or skip:
                    detail.append(('往返', net))
            elif arm or skip:
                d = 1 if net > 0 else -1
                fired = True
                detail.append(('兑现', net, d, 'skip' if skip else 'arm'))
            else:
                detail.append(('半格', net))
            if fired:
                steps += d
            arm = 0
            skip = 0
            mark = phase

    if verbose:
        for x in detail:
            print('    ', x)
    return steps


# ---- 构造正/反两个方向的「一格」序列 ----
# detent=11，中途必到对侧 00。两个方向各走一条合法路径。
FWD = [3, 1, 0, 2, 3]      # 11 → 01 → 00 → 10 → 11
BWD = [3, 2, 0, 1, 3]      # 11 → 10 → 00 → 01 → 11

oks = fails = 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  + ' + msg + (('  ' + extra) if extra else ''))
    else:
        fails += 1
        print('  X ' + msg + (('  ' + extra) if extra else ''))


print('[1] 正常一格')
ok(abs(run(FWD)) == 1, '正向一格 = 1 格（实际 ' + str(run(FWD)) + '）')
ok(abs(run([3] + BWD)) == 1, '反向一格 = 1 格（实际 ' + str(run([3] + BWD)) + '）')

print('\n[2] 中断丢一个中间态（对角跳变）—— 1.46.49 的核心')
# 正向：11→01→(丢00)→10→11，即 01 直接跳 10
lostF = [3, 1, 2, 3]
r = run([3] + lostF)
ok(abs(r) == 1, '正向丢样本仍走 1 格（实际 ' + str(r) + '）',
   '' if abs(r) == 1 else ' → 这就是「转两格才走一台」')
# 反向：11→10→(丢00)→01→11
lostB = [3, 2, 1, 3]
r = run(lostB)
ok(abs(r) == 1, '反向丢样本仍走 1 格（实际 ' + str(r) + '）')

print('\n[3] 原地抖动 / 转出去又回来 —— 必须 0 格')
ok(run([3, 1, 3]) == 0, '抖一下 11→01→11 = 0 格')
ok(run([3, 2, 3]) == 0, '抖一下 11→10→11 = 0 格')
ok(run([3, 1, 0, 1, 3]) == 0, '到对侧又退回 11→01→00→01→11 = 0 格')
ok(run([3, 2, 0, 2, 3]) == 0, '到对侧又退回 11→10→00→10→11 = 0 格')

print('\n[4] 连续快转')
seq = [3]
for _ in range(3):
    seq += FWD[1:]
r = run(seq)
ok(abs(r) == 3, '正转 3 格 = 3（实际 ' + str(r) + '）')

seq = [3]
for _ in range(2):
    seq += lostF            # 每格都丢一个样本
r = run(seq)
ok(abs(r) == 2, '连转 2 格且每格都丢样本 = 2（实际 ' + str(r) + '）',
   '' if abs(r) == 2 else ' → 快转时会少走')

print('\n[5] 一格拆成两次停靠（老人慢慢挪）不能多算')
# 11→01→11 算抖动（0 格），接着 11→01→00→10→11 才是一格
ok(run([3, 1, 3, 1, 0, 2, 3]) == 1, '抖一下 + 走一格 = 1 格（实际 '
   + str(run([3, 1, 3, 1, 0, 2, 3])) + '）')

print('\n[6] 旧判据的回归点：|net| 阈值 2 → 1 只影响「丢样本」那类')
# 构造 net=+1 的丢样本序列，确认现在兑现
r = run([3] + lostF)
ok(r != 0, '丢样本那类现在会兑现（旧代码要求 |net|>=2，会漏）')


print(chr(10) + '[7] 真机代码里的三处（1.46.49/50）')
import io as _io, os as _os
src = _io.open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'src', 'input.cpp'),
               encoding='utf-8').read()
ok('if (net == 0)' in src, '净相位 0（往返）不兑现 —— 不会再「轻碰就换台」')
ok('} else if (s_encArm || s_encSkip) {' in src, 'arm 或 skip 成立且 |net|>=1 就兑现（阈值 2→1）')
ok('s_encLastDir' in src and 'dir   = (s_encLastDir' not in src,
   '没有用 lastDir 猜方向（1.46.48 的错，已撤）')
ok('detent 学错' in src, '有「detent 学错自愈」')
ok('(ab == 0x00 || ab == 0x03) ? ab : 0x03' in src,
   '开机只接受 00/11 当停位初值（读到 01/10 就按 11 起步）')
ok('s_encNet' in src, '记录最近 8 次净相位，可远程看分布')

print('\n%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
