# -*- coding: utf-8 -*-
"""
1.26.0 离线验证：按住旋钮调音量的「音量大字」状态机

要验的四条（都是用户原话拆出来的）：
  ① 按住并转 -> volHold 为真，屏上显示「音量 X」
  ② 转的时候数字跟着变（不是松手才变）
  ③ 松手 -> volHold 变假，台名恢复
  ④ 不会卡住：即使松开事件漏了，2.5 秒也自己退出

另外三条防护：
  ⑤ 红外遥控的音量键**不该**进 volHold（用户手里没按旋钮）
  ⑥ App 调音量也不该进 volHold
  ⑦ 音量夹在 [VOL_MIN, VOL_MAX]
"""
import re, sys

SRC = r'C:\Users\lenovo\WorkBuddy\2026-11-01-18-34-26\MyRadio-v1.21.1\src'
fails = oks = 0

def chk(name, got, want):
    global fails, oks
    if got == want:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-52s got=%r want=%r' % (name, got, want))

cfg = open(SRC + r'\config.h', encoding='utf-8').read()
def cfgval(k):
    m = re.search(r'#define\s+%s\s+(\d+)' % k, cfg)
    assert m, k
    return int(m.group(1))

# 1.44.0 起音量域回原厂口径，旧的 VOL_MIN/VOL_MAX 已删，这里跟着改成 FVOL_
VOL_MIN = cfgval('FVOL_MIN')
VOL_MAX = cfgval('FVOL_MAX')
HOLD_STEP = cfgval('VOL_HOLD_STEP')

# ------------------------------------------------------------
#  复刻 input.cpp 的 inputLoop 里与本功能有关的骨架
# ------------------------------------------------------------
class Dev:
    def __init__(self):
        self.vol = 45
        self.volHold = False
        self.volHoldAt = 0
        self.t = 0
        self.drawn = []           # 记录每帧台名区该显示什么

    # 按住旋钮转一下（对应 input.cpp：if (s_stable) { volHoldBegin(); doVol(...); }）
    def turn_while_held(self, d=+1):
        self.t += 100
        self.volHoldBegin()
        self.doVol(d)

    # 普通转动（没按住）
    def turn_normal(self, d=+1):
        self.t += 100
        # 不改 vol，改的是台/频率
        self.settle()

    def volHoldBegin(self):
        self.volHold = True
        self.volHoldAt = self.t
        self.paint()

    def volHoldEnd(self):
        if self.volHold:
            self.volHold = False
            self.paint()

    def doVol(self, d):
        base = self.vol
        self.vol = max(VOL_MIN, min(VOL_MAX, base + d * HOLD_STEP))
        self.volHoldAt = self.t
        self.paint()              # 数字立刻变（这就是"跟手"）

    # 主循环每轮都会跑的那句：松开就退，或超时兜底
    def settle(self, pressed=True):
        if self.volHold and ((not pressed) or (self.t - self.volHoldAt > 2500)):
            self.volHoldEnd()

    def release(self):
        self.t += 50
        self.settle(pressed=False)

    def paint(self):
        # 对应 ui.cpp drawFreq：volHold 时显示「音量 X」，否则台名
        self.drawn.append('音量 %d' % self.vol if self.volHold else '台名')

print('=== ① 按住转动 → 显示音量 ===')
d = Dev()
chk('初始不显示音量', d.volHold, False)
d.turn_while_held(+1)
chk('按住转一下 → volHold 为真', d.volHold, True)
chk('屏上显示「音量 47」', d.drawn[-1], '音量 %d' % (45 + HOLD_STEP))

print('\n=== ② 数字跟着变（每次转都重画，且递增/递减）===')
d = Dev()
d.turn_while_held(+1)
v1 = d.vol
d.turn_while_held(+1)
v2 = d.vol
d.turn_while_held(-1)
v3 = d.vol
chk('连转 +1 两次，音量变大', v2 > v1, True)
chk('再 -1 一次，音量回落到 v1', v3, v1)
chk('每一步都重画过（帧数 >= 3）', len(d.drawn) >= 3, True)
chk('最后一次显示的数字 = 当前音量', d.drawn[-1], '音量 %d' % v3)
chk('数字不是等松手才变（中间帧已是新值）', ('音量 %d' % v2) in d.drawn, True)

print('\n=== ③ 松手 → 恢复台名 ===')
d = Dev()
d.turn_while_held(+1)
chk('松手前是音量', d.drawn[-1].startswith('音量'), True)
d.release()
chk('松手后 volHold 为假', d.volHold, False)
chk('松手后恢复台名', d.drawn[-1], '台名')

print('\n=== ④ 兜底：松开事件漏了也不会卡住 ===')
d = Dev()
d.turn_while_held(+1)
# 之后一直"按住"但不再转动，时间流逝 2.6 秒
d.t += 2600
d.settle(pressed=True)
chk('2.6 秒没动 → 自动退出', d.volHold, False)
chk('屏回到台名', d.drawn[-1], '台名')

# 但 2.4 秒还没超时，应该仍然是音量
d = Dev()
d.turn_while_held(+1)
d.t += 2400
d.settle(pressed=True)
chk('2.4 秒没动 → 仍是音量（没提前退）', d.volHold, True)

# 一直在转就一直显示（不会因为总时长超时而中断）
d = Dev()
d.turn_while_held(+1)
for _ in range(20):                      # 每 100ms 转一下，共 2 秒
    d.turn_while_held(+1)
    d.settle(pressed=True)
chk('持续转动 2 秒内一直显示音量', d.volHold, True)

print('\n=== ⑤⑥ 别的音量入口不该进 volHold ===')
# 红外遥控：走 doVol 但**不**调 volHoldBegin（改动就是这么设计的）
class DevIr(Dev):
    def ir_vol(self, d):
        self.t += 100
        self.doVol(d)          # 只调 doVol，不 volHoldBegin
        self.settle(pressed=False)
d = DevIr()
d.ir_vol(+1)
chk('遥控音量键 → volHold 仍为假', d.volHold, False)
chk('遥控音量键 → 屏上是台名', d.drawn[-1], '台名')
chk('但音量确实变了', d.vol, 45 + HOLD_STEP)

# App /api/vol：走 audioSetVol，压根不进 inputLoop
d = DevIr()
d.vol = max(VOL_MIN, min(VOL_MAX, 80))
d.paint()
chk('App 设音量 → volHold 仍为假', d.volHold, False)
chk('App 设音量 → 屏上是台名', d.drawn[-1], '台名')

print('\n=== ⑦ 音量夹紧 ===')
d = Dev()
d.vol = 100
d.turn_while_held(+1)
chk('上限不越界（最大 %d）' % VOL_MAX, d.vol, VOL_MAX)
d = Dev()
d.vol = VOL_MIN
d.turn_while_held(-1)
chk('下限不越界（最小 %d）' % VOL_MIN, d.vol, VOL_MIN)
chk('下限就是用户要的 6', VOL_MIN, 6)

# 从最小再转几次仍然是 6，且屏上显示 6（不是 0）
d = Dev()
d.vol = VOL_MIN
for _ in range(3):
    d.turn_while_held(-1)
chk('连转到底仍是 6', d.vol, 6)
chk('屏上显示「音量 6」而不是 0', d.drawn[-1], '音量 6')

print('\n=== ⑧ 源文件里确实按这个结构实现了 ===')
src_in = open(SRC + r'\input.cpp', encoding='utf-8').read()
src_ui = open(SRC + r'\ui.cpp', encoding='utf-8').read()
src_au = open(SRC + r'\audio.cpp', encoding='utf-8').read()

chk('input.cpp 有 volHoldBegin/End', 
    ('static void volHoldBegin' in src_in) and ('static void volHoldEnd' in src_in), True)
# 断言修正：定义那行是 `volHoldBegin() {`（不带分号），只有调用点带分号。
# 上一版把「>= 2 次」写成了对 count(带分号) 的判断，必然 1 次 —— 是断言错，不是代码错。
chk('有 1 处定义 + 1 处调用',
    src_in.count('static void volHoldBegin') == 1 and src_in.count('volHoldBegin();') == 1, True)
chk('调用点就在 s_stable 分支里（按住才生效）',
    src_in.find('if (s_stable) {') < src_in.find('volHoldBegin();') < src_in.find('} else if (A.fmMode) {', src_in.find('volHoldBegin();')), True)
chk('松手 + 超时兜底存在', 
    'A.volHold && (!s_stable || now - A.volHoldAt > 2500)' in src_in, True)
chk('ui.cpp drawFreq 有音量分支', 'if (A.volHold)' in src_ui, True)
# 1.39.0 起音量大字改成 cnCenterS + FS36、颜色 C_NAME（不再是 C_ACCENT 高亮）
chk('ui.cpp 用大字显示音量',
    'cnCenterS(vb, MAIN_Y + (MAIN_H - BIG_PX) / 2, C_NAME, C_BG, FS36)' in src_ui, True)
chk('ui.cpp 只让 needFreq 重绘（不整屏刷）',
    'if (vh != l_volHold || vv != l_volHoldVal) needFreq = true;' in src_ui, True)
# 1.34.0 删了 A.dirty = true（顶栏由 needTop 自己发现），只留同步写 + 解静音
chk('audio.cpp 同步写 A.vol（数字跟手的前提）',
    'A.vol = v;\n  A.muted = false;' in src_au, True)

print('\n================ 通过 %d / 失败 %d ================' % (oks, fails))
sys.exit(1 if fails else 0)
