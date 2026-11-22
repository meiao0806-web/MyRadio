# -*- coding: utf-8 -*-
# ============================================================
#  verify_batt_ocv.py —— 电量映射的回归（1.46.57 起）
#
#  A. 从 src/battery.cpp 抠出 kOcvMv / kOcvPct，检查表的合法性
#  B. 用同一套插值复算几个已知电压，和"老直线"对比，打印差多少点
#  C. 8 秒电压平滑 + 死区 3 点 + 每拍最多 1 点：喂噪声/带载跳变看显示会不会抽风
#  D. 假充电：1.46.55 的两条判据都会被"起播跳变"打中，新的不会
#
#  用法：python tools/verify_batt_ocv.py
# ============================================================
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails, oks = [], 0


def ok(cond, msg, extra=''):
    global oks
    if cond:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails.append(msg)
        print('  X   ' + msg + (('  ' + extra) if extra else ''))


src = io.open(os.path.join(ROOT, 'src/battery.cpp'), encoding='utf-8').read()
cfg = io.open(os.path.join(ROOT, 'src/config.h'), encoding='utf-8').read()


def grab_array(name):
    m = re.search(r'static const uint\d+_t\s+%s\[\]\s*=\s*\{([^}]*)\}' % name, src)
    assert m, '找不到 %s' % name
    return [int(x) for x in re.findall(r'\d+', m.group(1))]


def macro(name, default=None):
    m = re.search(r'#define\s+%s\s+(?:\((\d+)UL\*60\*1000\)|(\d+))' % name, cfg)
    if not m:
        return default
    return int(m.group(1)) * 60000 if m.group(1) else int(m.group(2))


MV = grab_array('kOcvMv')
PC = grab_array('kOcvPct')
HYST = macro('BATT_PCT_HYST', 3)
EMPTY = macro('BATT_EMPTY_MV')
SMOOTH = 8                    # battery.cpp 里 s_vSoc += (mvSoc - s_vSoc) / 8

print('== A) 表本身 ==')
ok(len(MV) == len(PC), '两个表长度一致（%d / %d）' % (len(MV), len(PC)))
ok(all(MV[i] > MV[i + 1] for i in range(len(MV) - 1)), '电压严格递减')
ok(all(PC[i] >= PC[i + 1] for i in range(len(PC) - 1)), '百分比单调不增')
ok(PC[0] == 100 and PC[-1] == 0, '端点是 100%% / 0%%（%d / %d）' % (PC[0], PC[-1]))
ok(MV[0] == 4200, '顶 4200mV')
ok(MV[-1] <= EMPTY, '表底 %dmV 不高于关机线 %dmV' % (MV[-1], EMPTY))


def pct_of(mv):
    if mv >= MV[0]:
        return 100
    if mv <= MV[-1]:
        return 0
    for i in range(1, len(MV)):
        if mv >= MV[i]:
            # C 的整数除法朝零截断，Python 的 // 是向下取整 —— 这里必须照 C 来
            return PC[i] + int((mv - MV[i]) * (PC[i - 1] - PC[i]) / (MV[i - 1] - MV[i]))
    return 0


def old_linear(mv):
    # 1.43.0~1.46.56 那条直线的底是 3400，和现在的关机线无关 ——
    # 这里留着它只为了"新表比老直线准"这条对比还能算。
    lo, hi = 3400, 4200
    if mv >= hi:
        return 100
    if mv <= lo:
        return 0
    return int((mv - lo) * 100 / (hi - lo))


print()
print('== B) 新表 vs 老直线（同一电压，差多少点）==')
print('    mV    老直线   新表   差')
for mv in (4200, 4100, 4000, 3945, 3900, 3800, 3750, 3700, 3671, 3600, 3500, 3400):
    a, b = old_linear(mv), pct_of(mv)
    print('  %5d   %4d%%   %4d%%  %+d' % (mv, a, b, b - a))
ok(pct_of(3800) == 49, '3.80V → 49%%（实得 %d%%）' % pct_of(3800))
ok(pct_of(3700) == 29, '3.70V → 29%%（实得 %d%%）' % pct_of(3700))
ok(pct_of(3671) < old_linear(3671) - 5, '3.67V 新表比老直线低 5 点以上（底部不再虚高）')
ok(pct_of(3945) > old_linear(3945), '3.95V 新表比老直线高（平台段本来就该走得快）')


# ------------------------------------------------------------
#  C. 平滑 + 死区（按时间推进，模拟每拍 1 秒 = BATT_SAMPLE_MS）
# ------------------------------------------------------------
class Soc:
    """照 battery.cpp：电压域 1/8 指数平滑 → 查表 → 死区 HYST 点 → 每拍最多挪 1 点"""

    def __init__(self, hyst=HYST, div=SMOOTH):
        self.pct = -1
        self.vsoc = -1
        self.t = 0
        self.hyst = hyst
        self.div = div
        self.maxstep = 0

    def tick(self, mv):
        self.t += 1000
        if self.vsoc < 0:
            self.vsoc = mv
        else:
            # C 的除法朝零截断
            d = mv - self.vsoc
            q = abs(d) // self.div
            self.vsoc += q if d >= 0 else -q
        soc = pct_of(self.vsoc)
        if self.pct < 0:
            self.pct = soc
        else:
            before = self.pct
            if soc <= self.pct - self.hyst:
                self.pct = max(0, self.pct - 1)
            elif soc >= self.pct + self.hyst:
                self.pct = min(100, self.pct + 1)
            self.maxstep = max(self.maxstep, abs(self.pct - before))
        return self.pct

    def hold(self, mv, secs):
        return [self.tick(mv) for _ in range(secs)]


print()
print('== C) 8 秒电压平滑 + 死区 %d 点 + 每拍最多 1 点 ==' % HYST)

# 1. 稳态：3.85V 应该显示表值本身（58），不再四舍五入到 60
s1 = Soc()
s1.hold(3850, 30)
ok(s1.pct == pct_of(3850),
   '3.85V 稳住 → 显示就是表值 %d%%（实得 %d%%）' % (pct_of(3850), s1.pct))

# 2a. 小噪声：±25mV 抖 2 分钟，一次都不动
s2 = Soc()
s2.hold(3850, 30)
base = s2.pct
for i in range(120):
    s2.tick(3850 + (25 if i % 2 else -25))
ok(s2.pct == base, '±25mV 抖 2 分钟，显示一点不动（%d%% → %d%%）' % (base, s2.pct))

# 2b. 中噪声：±60mV（≈±9 个点）抖 2 分钟，最多偏 1 点
s2b = Soc()
s2b.hold(3850, 30)
base = s2b.pct
for i in range(120):
    s2b.tick(3850 + (60 if i % 2 else -60))
ok(abs(s2b.pct - base) <= 1,
   '±60mV（比死区还大）抖 2 分钟，最多偏 1 点（%d%% → %d%%）' % (base, s2b.pct))

# 2c. 大噪声：±100mV（WiFi 突发的量级）抖 2 分钟，最多偏 3 点
s2c = Soc()
s2c.hold(3850, 30)
base = s2c.pct
for i in range(120):
    s2c.tick(3850 + (100 if i % 2 else -100))
ok(abs(s2c.pct - base) <= 3,
   '±100mV 抖 2 分钟，最多偏 3 点（%d%% → %d%%）' % (base, s2c.pct))

# 3. 缓慢放电：1mV/s 从 3.90 走到 3.45
s3 = Soc()
s3.hold(3900, 30)
path = [s3.tick(mv) for mv in range(3900, 3449, -1)]
ok(all(path[i] >= path[i + 1] for i in range(len(path) - 1)), '一路下降，没有回涨')
err = max(abs(p - pct_of(3900 - i - 1)) for i, p in enumerate(path))
ok(err <= 4, '全程偏差 ≤ 4 点（实测最大 %d 点）' % err)
ok(abs(s3.pct - pct_of(3450)) <= 3,
   '3.45V 时显示 %d%%，表值 %d%%，差 ≤ 3' % (s3.pct, pct_of(3450)))
ok(len(set(path)) >= 50, '整段走了 %d 个不同的值（1%% 精度，不是卡住不动）' % len(set(path)))

# 4. 单拍最多走 1 点：200mV 台阶不会"啪"地跳 30 点
s4 = Soc()
s4.hold(3800, 30)
before = s4.pct
up = []
for _ in range(90):
    up.append(s4.tick(4000))
ok(s4.maxstep <= 1, '任何一拍最多挪 1 点（实测最大 %d 点）' % s4.maxstep)
ok(s4.pct > before + 10, '但确实跟上去了（%d%% → %d%%，不能永远偏低）' % (before, s4.pct))
ok(abs(s4.pct - pct_of(4000)) <= HYST,
   '90 秒后停在表值附近（%d%% vs 表值 %d%%，差 ≤ %d）' % (s4.pct, pct_of(4000), HYST))

# 5. 底部不被抹成 0%：3.45V 的表值是 4%，老的分档会显示 0
s5 = Soc()
s5.hold(3450, 30)
ok(s5.pct == pct_of(3450) == 4,
   '3.45V 显示 %d%%（旧的分档方案会抹成 0%%，看着像已经没电）' % s5.pct)

# 6. 增益误差的灵敏度：电压整体偏 3% 会差多少点？
#    这条不是"应该通过"的断言，是把代价钉在这里 —— 3.95V 附近表很陡，
#    3% 的电压误差 ≈ 33 个点。所以 BATT_DIV 不准，光靠曲线救不回来。
off = Soc()
off.hold(3945 * 0.97, 30)
ok(abs(off.pct - pct_of(3945 * 0.97)) <= HYST,
   '偏 3%% 时显示 %d%%，等于表值 %d%%（读数对不上就是分压比的问题）'
   % (off.pct, pct_of(3945 * 0.97)))
print('   提示：3.95V ±3%% 电压误差 = %.0f 个点，分压比必须先准'
      % abs(pct_of(3945 * 1.03) - pct_of(3945 * 0.97)))

# 7. 代码里不该再有 5% 分档的残留
ok('BATT_PCT_STEP' not in src and 'BATT_PCT_STEP' not in cfg,
   '已去掉 BATT_PCT_STEP（显示不再是 5%% 一档）')
ok(re.search(r's_vSoc\s*=\s*-1;', src) and 's_vSoc = ocvOfPct(s_pct)' in src,
   '拔线时把平滑状态按当前显示重设（不然会按 1 点/秒 爬回虚高）')


# ------------------------------------------------------------
#  D. 插拔判据（1.46.64：只看相对台阶，不看绝对电压）
#     旧版要 s_mv >= 4050 才算在充电 —— 这块板 + 602535 在 CC 段只到 3.9~4.0V，
#     够不到 ⇒ 充电分支从来没执行过。这就是「插上 77%、拔掉 3%」的真因。
# ------------------------------------------------------------
RISE = macro('BATT_PLUG_RISE_MV')
FALL = macro('BATT_UNPLUG_FALL_MV')
ABS_PLUG = macro('BATT_ABS_PLUG_MV')


def strip_comments(t):
    return re.sub(r'//[^\n]*', '', t)


print()
print('== D) 插拔判据 ==')
ok(RISE == 170, '插电台阶 %dmV（实测 +580mV，起播跳变 +122mV，取中间）' % RISE)
ok(FALL == 170, '拔线台阶 %dmV（和插电同量级）' % FALL)
ok(122 < RISE, '起播跳变 +122mV 够不到 %dmV —— 不会误判成插电' % RISE)
ok(580 >= RISE, '真插电 +580mV 越过 %dmV' % RISE)
ok(230 >= FALL, '拔线 -230mV 越过 %dmV' % FALL)
ok(ABS_PLUG >= 4100, '绝对兜底 %dmV（开机那一拍没有窗口可用）' % ABS_PLUG)

nc = strip_comments(src)
ok('BATT_PLUG_NEAR_MV' not in nc,
   'BATT_PLUG_NEAR_MV(4050) 只剩注释里的历史说明，代码里不再引用')
ok('BATT_PLUG_WIN_N' in src and 'winMax() - winMin()' in src, '120 秒窗口在')
ok('BATT_CHG_PCT_MS' in src and 's_anchor' in src, '充电按时间爬（锚点 + 时长）')
ok('BATT_CHG_PLATEAU_FULL_MS' in src, '满电按恒压平台时长判')
ok('4200.0f / (float)s_mv' in src, 'CV 平台自校准（4.20V 硬基准）')
ok(re.search(r'if \(target >= 100\) \{ s_pct = 100; return; \}', src) is not None,
   '端点不走死区（否则永远停在 98% 到不了 100%）')

print()
print('通过 %d / 失败 %d' % (oks, len(fails)))
if fails:
    for f in fails:
        print('  FAIL:', f)
    sys.exit(1)
