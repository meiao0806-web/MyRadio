# -*- coding: utf-8 -*-
"""
1.46.20 离线验证：旋钮「不管转多快，一格就是一格」

用户报障（1.46.20）：
  · 扭动一格调了 3 格，「可能跟扭动速度有关」
  · 非常慢地轻轻动一点点，台变了，松手又变回来
  · 大部分时候一格一台，有时候一格好几台；回转也是

历史：1.45.5 ~ 1.46.19 全是「累计原始边沿 ÷ ENC_DIV」。
这条路走不通的硬证据：真机 /api/enc 抓到每格边沿数 = 1 / 4 / 17 / 49（不是常数），
任何固定除数都会把边沿多的一格算成三四格、把边沿少的一格留到下次凑。

1.46.20 换成**认停位（detent）**：
  一格 = 走完一个正交循环（11→01→00→10→11），中途必然经过**对侧电平**。
   ① 见到对侧电平 → arm
   ② 回到 detent  → 按这一趟的净相位出 ±1（一次停位最多一格）
   ③ 没到对侧就回 detent（手抖 / 半格退回）→ 净位移 0，不出格

1.46.29 补一条：ISR 被 WiFi / 刷屏打断时，两次进中断之间 A、B 各变一次，
对侧那个样本会被整段吞掉（编码器状态表里查出来是「对角跳变」），
s_encArm 一直是 0 ⇒ 这一格白转（用户原话「扭一格没有换台，再扭一下才换台」）。
所以见过对角跳变时，净位移 >= 2 也算一格；没见过对角跳变仍然只认 arm ——
否则「原地抖动 11→01→11」的净位移也正好是 2，又要把老毛病招回来。

这个脚本做两件事：
  A) 源码级：detent 判据在、按边沿数的老换算不在了
  B) 行为级：把 ISR 逐行复刻成 Python，喂真实 EC11 序列（含弹跳、半格、快甩），
     断言「一个 detent 最多一格」

跑法：python tools/verify_enc.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

fails = oks = 0


def chk(name, got, want):
    global fails, oks
    if got == want:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-58s got=%r want=%r' % (name, got, want))


cfg = io.open(os.path.join(SRC, 'config.h'), encoding='utf-8').read()
inp = io.open(os.path.join(SRC, 'input.cpp'), encoding='utf-8').read()


def cfgval(k):
    m = re.search(r'#define\s+%s\s+(\d+)' % k, cfg)
    if not m:
        print('  FAIL 找不到宏 %s' % k)
        sys.exit(1)
    return int(m.group(1))


LEARN_MS = cfgval('ENC_DET_LEARN_MS')

print('=== 0) 宏值 ===')
chk('ENC_DET_LEARN_MS 在 300~3000ms（太短会学到中间态，太长自愈迟钝）',
    1 if 300 <= LEARN_MS <= 3000 else 0, 1)

print('=== 1) 源码级：换成认停位，老的「÷边沿数」不许回来 ===')
chk('不再按边沿数换算（没有 accNow / s_encAcc）',
    ('accNow' in inp) or ('s_encAcc' in inp), False)
chk('不再有「余数停 60ms 兑现一格」（ENC_GAP_MS）', 'ENC_GAP_MS' in inp, False)
chk('不再有回弹时间窗（ENC_BACK_MS）', 'ENC_BACK_MS' in inp, False)
chk('ISR 里认对侧电平（s_encDet ^ 0x03）', 's_encDet ^ 0x03' in inp, True)
chk('1.46.29 对角跳变打标记（s_encSkip）', 's_encSkip = 1;' in inp, True)
chk('1.46.29 兜底条件带 skip 守卫（不是裸的 net >= 2）',
    '(s_encSkip && (net >= 2 || net <= -2))' in inp, True)
chk('1.46.29 回停位清 skip', 's_encSkip = 0;' in inp, True)
chk('ISR 里回到 detent 才兑现（ab == s_encDet）', 'ab == s_encDet' in inp, True)
chk('兑现按净相位定符号（s_encPhase - s_encMark）',
    's_encPhase - s_encMark' in inp, True)
chk('一次停位最多 ±1（有 ++ 和 --，没有 += net）',
    ('s_encSteps++' in inp) and ('s_encSteps--' in inp) and ('s_encSteps += net' not in inp), True)
chk('inputLoop 里取走的是格数（s_encSteps）', 'd = (int)s_encSteps' in inp, True)
chk('detent 电平会自愈（ENC_DET_LEARN_MS）', 'ENC_DET_LEARN_MS' in inp, True)
chk('开机把当前 AB 当 detent 初值', 's_encDet   = ab;' in inp, True)

m = re.search(r'ENC_TBL\[16\]\s*=\s*\{([^}]*)\}', inp)
assert m, '找不到 ENC_TBL'
TBL = [int(x) for x in m.group(1).replace(' ', '').split(',') if x.strip()]
STANDARD = [0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0]
chk('ENC_TBL 与标准正交状态表一致', TBL, STANDARD)


# ============================================================
#  逐行复刻 input.cpp 的 encIsr
# ============================================================
class Enc:
    """detent 锁存器。ab 两位：bit1=A，bit0=B。"""

    # 正向环：00 → 10 → 11 → 01 → 00，每一步 ENC_TBL 都查得 +1。
    # 从 detent 出发沿这个环走 4 步 = 手上的一格。
    RING = (0b00, 0b10, 0b11, 0b01)

    def __init__(self, det=3):
        self.ab = det
        self.det = det
        self.arm = 0
        self.skip = 0
        self.phase = 0
        self.mark = 0
        self.steps = 0
        self.run = 0
        self.peaks = []

    def edge(self, ab):
        prev = self.ab
        if ab == prev:
            return
        self.ab = ab
        st = TBL[(prev << 2) | ab]
        if st == 0:
            self.skip = 1          # 1.46.29：对角跳变 = 中间有个状态被吞了
            return
        self.phase += st
        self.run += 1
        if ab == (self.det ^ 0x03):
            self.arm = 1
        elif ab == self.det:
            net0 = self.phase - self.mark
            if self.arm or (self.skip and (net0 >= 2 or net0 <= -2)):
                net = self.phase - self.mark
                if net > 0:
                    self.steps += 1
                elif net < 0:
                    self.steps -= 1
                self.peaks.append(self.run)
            self.arm = 0
            self.skip = 0
            self.mark = self.phase
            self.run = 0

    def take(self):
        d = self.steps
        self.steps = 0
        return d

    def walk(self, steps, bounce=0):
        """从当前电平沿正交环走 steps 个边沿（正=顺时针）。bounce=每步弹跳几次。"""
        idx = self.RING.index(self.ab)
        dirn = 1 if steps > 0 else -1
        for _ in range(abs(steps)):
            prev = self.ab
            idx = (idx + dirn) % 4
            ab = self.RING[idx]
            self.edge(ab)
            if bounce:
                for _ in range(bounce):
                    self.edge(prev)
                    self.edge(ab)

    def turn(self, n, bounce=0):
        """转 n 格"""
        self.walk(4 * n, bounce)


print('=== 2) 行为级：一格 = 一格 ===')

e = Enc()
e.turn(+1)
chk('正向一格 → +1', e.take(), 1)

e = Enc()
e.turn(-1)
chk('反向一格 → -1', e.take(), -1)

e = Enc()
e.turn(+1, bounce=12)
chk('快转一格 + 严重弹跳（约 100 个边沿）→ 仍只 +1', e.take(), 1)
chk('  记下的这一格原始边沿确实很多', 1 if e.peaks and e.peaks[-1] > 40 else 0, 1)

e = Enc()
e.turn(+3)
chk('快速连转 3 格 → +3（不是 +9）', e.take(), 3)

e = Enc()
e.turn(+10, bounce=6)
chk('带弹跳快甩 10 格 → +10', e.take(), 10)

e = Enc()
e.turn(-3, bounce=6)
chk('反向快甩 3 格 → -3（不是 -9）', e.take(), -3)

e = Enc()
e.walk(+2)      # 走到对侧（半格）
e.walk(-2)      # 退回原 detent
chk('只走半格又退回 → 0 格（台不该变）', e.take(), 0)

e = Enc()
for _ in range(20):
    e.edge(0b01)
    e.edge(0b11)
chk('原地抖动 20 次 → 0 格', e.take(), 0)
chk('  抖动也没让 arm 挂住', e.arm, 0)

e = Enc()
e.walk(+2)
chk('停在半格不出格', e.take(), 0)

# ---- 1.46.29：ISR 被延迟，对侧那个样本被整段吞掉 ----
# 正常一格是 11→01→00→10→11；下面把 00 掐掉，模拟「两次中断之间 A、B 各变一次」
e = Enc()
e.edge(0b01)
e.edge(0b10)      # 本该是 00 的那一位被吞掉（对角跳变）
e.edge(0b11)
chk('丢样本（正向一格被吞掉 00）→ 仍出 +1', e.take(), 1)

e = Enc()
e.edge(0b10)
e.edge(0b01)      # 反方向同样丢一个
e.edge(0b11)
chk('丢样本（反向一格被吞掉 00）→ 仍出 -1', e.take(), -1)

e = Enc()
for _ in range(3):
    e.edge(0b01)
    e.edge(0b10)
    e.edge(0b11)
chk('连续 3 次丢样本 → 3 格（不吞台）', e.take(), 3)

# 反例：原地抖动不能因为「净位移恰好是 2」就出格
e = Enc()
for _ in range(6):
    e.edge(0b01)
    e.edge(0b11)
chk('原地抖动（无对角跳变）→ 仍是 0 格，老毛病不许回来', e.take(), 0)

for D in (0b00, 0b01, 0b10, 0b11):
    e = Enc(det=D)
    e.turn(+1)
    chk('detent=%02d 的编码器正向一格 → +1' % int(bin(D)[2:]), e.take(), 1)
    e.turn(-1)
    chk('detent=%02d 的编码器反向一格 → -1' % int(bin(D)[2:]), e.take(), -1)

e = Enc()
outs = []
for _ in range(5):
    e.turn(+1)
    outs.append(e.take())
chk('慢慢拧 5 次，每次都只出 1 格', outs, [1, 1, 1, 1, 1])

e = Enc()
e.turn(+1)
e.turn(-1)
e.turn(+1)
chk('一转一退再一转 → 净 +1', e.take(), 1)

# 真机最脏的一格：每一步都抖 8 次（约 70 个原始边沿）
e = Enc()
e.turn(+1, bounce=8)
chk('最脏的一格（约 70 个原始边沿）→ 仍只 +1', e.take(), 1)

print('=== 3) 用户场景整段复演 ===')
e = Enc()
e.turn(+1, bounce=8)
chk('场景A 老人快转一格 → 1 台（旧固件会 3 台）', e.take(), 1)

e = Enc()
e.edge(0b01)
e.edge(0b11)
chk('场景B 轻碰一下又松手 → 0 台', e.take(), 0)

e = Enc()
for _ in range(3):
    e.turn(+1, bounce=2)
chk('场景C 正常速度连拧 3 格 → 3 台', e.take(), 3)

e = Enc()
e.turn(+1, bounce=4)
e.turn(-1, bounce=4)
chk('场景D 拧过去又拧回来 → 净 0 台', e.take(), 0)

print()
print('通过 %d / 失败 %d' % (oks, fails))
sys.exit(0 if fails == 0 else 1)
