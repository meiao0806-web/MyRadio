# -*- coding: utf-8 -*-
# ============================================================
#  verify_batt_dbl.py —— 电量状态机回归（1.46.64 重写）
#
#  1.46.55/1.46.58 那一版验的东西已经不在了（jumpUp、pct_floor、BATT_JUMP_MV、
#  BATT_CHG_RISE_MV、s_trendD ……全部被 1.46.64 的「相对台阶 + 状态机」取代），
#  所以这个文件整体重写：**把 battery.cpp 的新逻辑逐行搬成 python 跑**，
#  再用用户报的三个现象当断言。
#
#  用户原话（1.46.63 上报）：
#    「它没电关机后应该不是真的没电，只是图标显示没电。关机后开机，插上电，
#      电量瞬间变成 77；拔掉电后，一会儿就变成 3。电池是 3.7V 602535。」
#  三句话对应下面 [1][2][3] 三组断言。
#
#  用法：python tools/verify_batt_dbl.py
# ============================================================
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []
oks = 0


def ok(cond, msg, extra=''):
    global oks
    if cond:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails.append(msg)
        print('  X   ' + msg + (('  ' + extra) if extra else ''))


def cdiv(a, b):
    """C 的整数除法朝零截断（python 的 // 是向下取整）"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


# ============================================================
#  一、把 config.h 里的参数抠出来（不手抄，改宏这里自动跟着变）
# ============================================================
cf = io.open(os.path.join(ROOT, 'src', 'config.h'), encoding='utf-8').read()
bc = io.open(os.path.join(ROOT, 'src', 'battery.cpp'), encoding='utf-8').read()


def cf_macro(name):
    """把 `#define X (48UL*1000)` / `#define X 3550` 都还原成整数毫秒"""
    m = re.search(r'#define\s+%s\s+([^\n/]+)' % name, cf)
    if not m:
        return -1
    expr = m.group(1).replace('UL', '').replace('ul', '').strip()
    if not re.fullmatch(r'[0-9+\-*() ]+', expr):
        return -1
    try:
        return int(eval(expr))
    except Exception:
        return -1


EMPTY = cf_macro('BATT_EMPTY_MV')
SEEN_HIGH = cf_macro('BATT_SEEN_HIGH_MV')
LOW_PCT = cf_macro('BATT_LOW_PCT')
HYST = cf_macro('BATT_PCT_HYST')
RISE = cf_macro('BATT_PLUG_RISE_MV')
FALL = cf_macro('BATT_UNPLUG_FALL_MV')
WIN_N = cf_macro('BATT_PLUG_WIN_N')
SETTLE = cf_macro('BATT_PLUG_SETTLE_MS')
LEAVE_DROP = cf_macro('BATT_CHG_LEAVE_DROP_MV')
ABS_PLUG = cf_macro('BATT_ABS_PLUG_MV')
CHG_MS = cf_macro('BATT_CHG_PCT_MS')
MIN_TRUST = cf_macro('BATT_CHG_MIN_TRUST_MS')
UNPLUG_HOLD = cf_macro('BATT_UNPLUG_HOLD_MS')
UNPLUG_MAX = cf_macro('BATT_UNPLUG_MAX_MS')
UNPLUG_FLAT = cf_macro('BATT_UNPLUG_FLAT_MV')
CV_MV = cf_macro('BATT_CV_MV')
CV_FLAT = cf_macro('BATT_CV_FLAT_MV')
FLAT_HOLD = cf_macro('BATT_CV_FLAT_HOLD_MS')
PLATEAU_FULL = cf_macro('BATT_CHG_PLATEAU_FULL_MS')
FULL_CLEAR = cf_macro('BATT_FULL_CLEAR_MV')
FULL_DETECT = cf_macro('BATT_FULL_DETECT_MV')
EMPTY_HOLD = cf_macro('BATT_EMPTY_HOLD_MS')
OFF_MIN_UP = cf_macro('BATT_OFF_MIN_UP_MS')
EXT_MV = cf_macro('BATT_EXT_MV')
EXT_HOLD = cf_macro('BATT_EXT_HOLD_MS')
QUIET_MS = cf_macro('BATT_LOW_QUIET_MS')
REPEAT_MS = cf_macro('BATT_LOW_REPEAT_MS')
PLAUSIBLE = cf_macro('BATT_PLAUSIBLE_MV')
OVERV = cf_macro('BATT_OVERV_MV')
MARGIN = cf_macro('BATT_EMPTY_MARGIN_MV')
CHG_OFF_DEF = cf_macro('BATT_CHG_OFF_DEF')

MV_T = re.search(r'static const uint16_t kOcvMv\[\] = \{([^}]*)\}', bc).group(1)
PC_T = re.search(r'static const uint8_t kOcvPct\[\] = \{([^}]*)\}', bc).group(1)
MV = [int(x) for x in re.findall(r'\d+', MV_T)]
PC = [int(x) for x in re.findall(r'\d+', PC_T)]

print('[0] 参数（直接从 config.h 抠）')
print('    关机 %dmV / 低电 %d%% / 台阶 %dmV / 窗口 %ds / 充电 1%%·%ds / 平台 %d 分钟'
      % (EMPTY, LOW_PCT, RISE, WIN_N, CHG_MS // 1000, PLATEAU_FULL // 60000))
ok(EMPTY > 3000, '关机线 %dmV 在 3.0V 之上（3.0V 以下伤电芯）' % EMPTY)
ok(EMPTY < 3600, '关机线没有抬得太高（否则等于"还有电就关机"）')
ok(SEEN_HIGH < EMPTY, '见过高位(%dmV) < 空电(%dmV)（反了的话自动关机永远不触发）'
   % (SEEN_HIGH, EMPTY))
ok(PLATEAU_FULL >= 30 * 60 * 1000, '满电要平台稳住 ≥30 分钟（600mAh 的 CV 收尾就是这个量级）')
ok(LOW_PCT >= 10, '低电播报 %d%%（关机线在 ~9%%，5%% 报警来不及）' % LOW_PCT)
ok(CHG_MS == 48000, '充电 1%% / 48 秒（600mAh + 0.5~1A ⇒ 40~80 分钟充满）')


def pct_of(mv):
    if mv >= MV[0]:
        return 100
    if mv <= MV[-1]:
        return 0
    for i in range(1, len(MV)):
        if mv >= MV[i]:
            return PC[i] + cdiv((mv - MV[i]) * (PC[i - 1] - PC[i]), MV[i - 1] - MV[i])
    return 0


def ocv_of_pct(p):
    if p >= 100:
        return MV[0]
    if p <= 0:
        return MV[-1]
    for i in range(1, len(MV)):
        if p >= PC[i]:
            return MV[i - 1] + cdiv((p - PC[i - 1]) * (MV[i] - MV[i - 1]), PC[i] - PC[i - 1])
    return MV[-1]


# ============================================================
#  二、battery.cpp 的镜像（1.46.64）
# ============================================================
ST_DISC, ST_PLUG_WAIT, ST_CHG, ST_UNPLUG_WAIT = 0, 1, 2, 3


class Batt:
    def __init__(self):
        self.now = 0
        self.boot = 0
        self.mv = -1
        self.pct = -1
        self.ok_ = False
        self.seen_high = False
        self.plaus = 0
        self.st = ST_DISC
        self.st_at = 0
        self.enter_at = 0
        self.first_done = False
        self.chg_by_abs = False
        self.chg_enter_mv = 0
        self.chg_peak = 0
        self.ocv = -1
        self.vsoc = -1
        self.ocv_ready = True
        self.anchor = -1
        self.flat_at = 0
        self.flat_mv = 0
        self.chg_step = CHG_OFF_DEF
        self.full = False
        self.ext = False
        self.ext_at = 0
        self.win = []
        self.empty_at = 0
        self.off_said = False
        self.powered_off = False
        self.off_pct = -1
        self.off_mv = -1
        self.low_at = 0
        self.ann_at = 0
        self.announced = False
        self.voice = []
        self.scale = 1.0
        self.scale_tried = False
        self.saved = -1
        self.log = []
        self.free_mv = 0            # 测试里显式给：关功放能读到多少（0 = 没有功放脚）
        self.playing = False

    # ---- 工具 ----
    def win_push(self, v):
        self.win.append(v)
        if len(self.win) > WIN_N:
            self.win.pop(0)

    def slew(self, target):
        target = max(0, min(100, target))
        if self.pct < 0:
            self.pct = target
            return
        if target >= 100:                       # 端点不走死区（同 slewTo）
            self.pct = 100
            return
        if target <= 0 and self.pct <= 4:
            self.pct = 0
            return
        if target <= self.pct - HYST:
            self.pct -= 1
        elif target >= self.pct + HYST:
            self.pct += 1
        self.pct = max(0, min(100, self.pct))

    # ---- 状态进出 ----
    def chg_enter(self, by_abs):
        self.st, self.st_at, self.enter_at = ST_CHG, self.now, self.now
        self.chg_by_abs = by_abs
        self.chg_enter_mv = self.mv
        self.chg_peak = self.mv
        self.flat_at = 0
        if by_abs and self.mv >= FULL_DETECT:
            p = 98
        elif self.saved >= 0 and self.saved < pct_of(self.mv) - 25:
            p = self.saved
        elif self.pct >= 0:
            p = self.pct
        else:
            p = pct_of(self.mv - self.chg_step)
        p = max(0, min(99, p))
        self.anchor = p
        if self.pct < 0 or self.pct > p + 30:
            self.pct = p
        self.log.append('chg@%d anc=%d' % (self.now, p))

    def chg_leave(self):
        dur = self.now - self.enter_at
        self.st, self.st_at = ST_UNPLUG_WAIT, self.now
        drop = self.chg_peak - self.mv
        if 80 <= drop <= 900:
            self.chg_step = cdiv(self.chg_step * 3 + drop, 4)
        if dur < MIN_TRUST and self.anchor >= 0 and self.pct > self.anchor:
            self.pct = self.anchor
        self.flat_at = 0
        self.ocv_ready = False
        self.ocv = -1
        self.log.append('unplug@%d dur=%ds' % (self.now, dur // 1000))

    def tick(self, raw):
        self.now += 1000
        now = self.now

        mv = int(raw)
        self.mv = mv if self.mv < 0 else self.mv + cdiv(mv - self.mv, 6)
        self.win_push(self.mv)
        rise = self.mv - min(self.win)
        fall = max(self.win) - self.mv

        # ---- 开机第一次：先判"是不是插着电" ----
        if not self.first_done:
            self.first_done = True
            saved_far = (self.saved >= 0 and self.saved <= pct_of(self.mv) - 25
                         and self.mv >= 3800)
            if self.mv >= ABS_PLUG:
                self.chg_enter(True)
            elif saved_far:
                self.chg_enter(True)

        # ---- 状态机（只看相对台阶）----
        if self.st == ST_DISC:
            if rise >= RISE:
                self.st, self.st_at = ST_PLUG_WAIT, now
        elif self.st == ST_PLUG_WAIT:
            if rise < RISE // 2 and self.mv < ABS_PLUG:
                self.st = ST_DISC                      # 落回去了 = 起播/换歌
            elif now - self.st_at >= SETTLE:
                if rise >= (RISE * 3) // 4 or self.mv >= ABS_PLUG:
                    self.chg_enter(False)
                else:
                    self.st = ST_DISC
        elif self.st == ST_CHG:
            if (fall >= FALL or self.mv <= self.chg_enter_mv - LEAVE_DROP
                    or (self.chg_by_abs and self.mv < FULL_CLEAR)):
                self.chg_leave()
        elif self.st == ST_UNPLUG_WAIT:
            if now - self.st_at >= UNPLUG_HOLD and (
                    max(self.win) - min(self.win) < UNPLUG_FLAT
                    or now - self.st_at > UNPLUG_MAX):
                self.st = ST_DISC
                self.vsoc = ocv_of_pct(self.pct)
                self.ocv = -1
                self.ocv_ready = True
                self.log.append('disc@%d vsoc=%d' % (now, self.vsoc))

        # ---- 目标百分比 ----
        if self.st == ST_CHG:
            if self.mv > self.chg_peak:
                self.chg_peak = self.mv
            if self.mv >= CV_MV - 40:                  # 平台跟踪
                if not self.flat_at:
                    self.flat_at, self.flat_mv = now, self.mv
                elif abs(self.mv - self.flat_mv) > CV_FLAT:
                    self.flat_at, self.flat_mv = now, self.mv
            else:
                self.flat_at = 0
            target = self.anchor + cdiv(now - self.enter_at, CHG_MS)
            target = min(99, max(self.anchor, target))
            if self.full:
                target = 100
            self.slew(target)
        elif self.st in (ST_PLUG_WAIT, ST_UNPLUG_WAIT):
            pass                                       # 判定窗口内屏上不动
        else:
            if self.playing and self.free_mv > 0:
                self.ocv = self.free_mv
            else:
                self.ocv = self.mv
            ocv = max(3000, min(OVERV, self.ocv))
            if self.vsoc < 0:
                self.vsoc = ocv
            else:
                self.vsoc += cdiv(ocv - self.vsoc, 8)
            self.slew(pct_of(self.vsoc))

        # ---- 0% 复核 + 地板 ----
        if self.st == ST_DISC and self.pct <= 0 and self.free_mv >= EMPTY + MARGIN:
            self.pct = max(1, pct_of(self.free_mv))

        if self.mv >= SEEN_HIGH:
            self.seen_high = True

        # ---- 外接电源 ----
        if self.st == ST_CHG or self.mv >= EXT_MV:
            if not self.ext_at:
                self.ext_at = now
            if not self.ext and (self.st == ST_CHG or now - self.ext_at >= EXT_HOLD):
                self.ext = True
        else:
            self.ext_at = 0
            self.ext = False

        # ---- 满电：平台稳住够久 ----
        plateau = (self.st == ST_CHG and self.flat_at
                   and now - self.flat_at >= FLAT_HOLD and self.mv >= CV_MV - 40)
        if plateau and not self.full and now - self.flat_at >= PLATEAU_FULL:
            self.full = True
            self.log.append('full@%d' % now)
        if self.st != ST_CHG and self.mv < FULL_CLEAR:
            self.full = False

        # ---- 分压系数自校准 ----
        if plateau and now - self.flat_at >= FLAT_HOLD and self.mv >= 4130 and not self.scale_tried:
            self.scale_tried = True
            ns = self.scale * (4200.0 / self.mv)
            ns = max(0.90, min(1.12, ns))
            if ns > self.scale * 1.015 or ns < self.scale * 0.985:
                self.log.append('scale %.4f->%.4f' % (self.scale, ns))
                self.scale = ns

        # ---- 可信度 ----
        if PLAUSIBLE <= self.mv <= OVERV:
            self.plaus = min(3, self.plaus + 1)
        else:
            self.plaus = 0
        if (self.seen_high or self.plaus >= 3) and not self.ok_:
            self.ok_ = True

        # ---- 低电播报 ----
        low_now = self.ok_ and self.pct >= 0 and self.pct <= LOW_PCT
        quiet = self.st in (ST_CHG, ST_PLUG_WAIT)
        if self.st == ST_CHG:
            self.low_at = 0
        if quiet:
            if not self.announced:
                self.announced = True
                if not self.ann_at:
                    self.ann_at = now
        elif low_now:
            if not self.low_at:
                self.low_at = now
            if (now - self.boot >= QUIET_MS and now - self.low_at >= QUIET_MS
                    and (not self.announced or now - self.ann_at > REPEAT_MS)):
                self.announced = True
                self.ann_at = now
                self.voice.append(('low', now))
        else:
            self.low_at = 0
            self.announced = False

        # ---- 没电：撑满才动手 ----
        mv_empty = self.vsoc if self.vsoc > 0 else self.mv
        if (self.ok_ and self.seen_high and self.st == ST_DISC and not self.ext
                and mv_empty <= EMPTY):
            if not self.empty_at:
                self.empty_at = now
        else:
            self.empty_at = 0
        if (not self.off_said and self.empty_at
                and now - self.empty_at > EMPTY_HOLD
                and now - self.boot > OFF_MIN_UP):
            self.off_said = True
            self.off_pct = self.pct
            self.off_mv = mv_empty
            self.voice.append(('off', now))
            self.log.append('off@%d pct=%d ocv=%d' % (now, self.pct, mv_empty))
            self.powered_off = True

    def run(self, raw, secs):
        for _ in range(secs):
            self.tick(raw)


# ============================================================
print('\n[1] 「拔掉电一会儿就变成 3」—— 带载终端 3.43V 不该报 3%')
b = Batt()
b.playing = True
b.free_mv = 3700                 # 关功放后开路 3.70V（负载压降 274mV，实测值）
b.run(3430, 90)
ok(25 <= b.pct <= 33, '屏上是 %d%%（OCV 3.70V 查表 29%%），不是 3%%' % b.pct,
   'pct=%d' % b.pct)

print('\n[2] 「插上电瞬间变成 77」—— 从 %d%% 插电，头两分钟不许跳' % b.pct)
p0 = b.pct
b.playing = False
peak = p0
for _ in range(120):             # 插上：终端 3.43 → 4.01V（+580mV，实测台阶）
    b.tick(4010)
    peak = max(peak, b.pct)
ok(b.st == ST_CHG, '30 秒内认出充电（st=%d，%s）'
   % (b.st, [x for x in b.log if x.startswith('chg')]))
ok(peak - p0 <= 2, '插上 2 分钟最多涨 %d 点（%d%% → %d%%），不是 29→77' % (peak - p0, p0, peak))
ok(b.pct >= p0 - 1, '也没有掉下去（%d%%）' % b.pct)

print('\n[3] 充电期间按时间爬：1%% / %d 秒' % (CHG_MS // 1000))
b.run(4010, 60 * 30 - 120)       # 累计充 30 分钟
gained = b.pct - b.anchor
ok(32 <= gained <= 38, '充 30 分钟涨了 %d 点（锚点 %d%% → %d%%，1800s/48s ≈ 37）'
   % (gained, b.anchor, b.pct))

print('\n[4] 拔掉电：先冻住，散完电荷再采信（不许跳回 %d%%）' % p0)
b.playing = True
b.free_mv = 3780                 # 真充进去一些：开路 3.70 → 3.78
before = b.pct
b.run(3780, 60)
ok(b.st == ST_UNPLUG_WAIT, '拔线被认出来（st=%d）' % b.st)
ok(abs(b.pct - before) <= 1, '前 60 秒屏上冻住（%d%% → %d%%）' % (before, b.pct))
b.run(3780, 120)
ok(b.st == ST_DISC, '窗口平了之后回到放电态（st=%d）' % b.st)
ok(abs(b.pct - pct_of(3780)) <= 6,
   '对齐到开路 3.78V = %d%%（实得 %d%%）' % (pct_of(3780), b.pct))
ok(b.pct >= p0, '充了半小时不该比插电前低（%d%% >= %d%%）' % (b.pct, p0))

print('\n[5] 起播/换歌的 +122mV 跳变不许当成插电')
b2 = Batt()
b2.playing = True
b2.free_mv = 3700
b2.run(3700, 60)
b2.run(3822, 300)                # 实测起播跳 +122mV，持续 5 分钟
ok(b2.st == ST_DISC, '没被判成充电（st=%d）' % b2.st, 'log=%s' % b2.log)
ok(b2.pct > 20, '电量没被抬高（%d%%）' % b2.pct)

print('\n[6] 真插电（+580mV）认得出，拔线（-230mV）退得出来')
b3 = Batt()
b3.playing = True
b3.free_mv = 3800
b3.run(3800, 120)
b3.run(4380, 60)
ok(b3.st == ST_CHG, '认得出（st=%d）' % b3.st, 'log=%s' % b3.log)
b3.run(4150, 60)                 # 拔掉，但表面电荷还在
ok(b3.st == ST_UNPLUG_WAIT, '掉了 230mV → 退出充电（st=%d）' % b3.st)

print('\n[7] 短充 5 分钟不许涨进度（「插上看一眼就拔」）')
b4 = Batt()
b4.run(3700, 60)
base = b4.pct
b4.run(4200, 300)
ok(b4.st == ST_CHG, '在充电（st=%d）' % b4.st)
b4.run(3700, 5)
ok(b4.pct <= base + 1, '插 5 分钟又拔：%d%% → %d%%（没白涨）' % (base, b4.pct))

print('\n[8] 满电只认「平台稳住 %d 分钟」' % (PLATEAU_FULL // 60000))
b5 = Batt()
b5.run(3700, 60)
b5.run(4200, 20 * 60)
ok(b5.st == ST_CHG and not b5.full, '插 20 分钟还不满（终端已经 4.20V 也不认）')
b5.run(4200, 30 * 60)
ok(b5.full, '平台撑到 %d 分钟才算满' % (PLATEAU_FULL // 60000))
b5.run(4200, 20)
ok(b5.pct == 100, '满电显示 100%%（实得 %d%%）' % b5.pct)

print('\n[9] 关机线：屏上还剩 ~%d%% 就关，不是 0%%，也不放到 3.0V' % pct_of(EMPTY))
b6 = Batt()
b6.seen_high = True
mv = 3720
while mv > 3400 and not b6.powered_off:
    for _ in range(6):           # 约 6mV/分钟，比真机快一点但同量级
        b6.tick(mv)
        if b6.powered_off:
            break
    mv -= 1
ok(b6.powered_off, '开路走到 %dmV 附近关机了（%s）'
   % (EMPTY, [x for x in b6.log if x.startswith('off')]))
ok(b6.off_pct >= 8, '关机那一刻屏上还有 %d%%（不是 0%%，用户看得见"该充了"）' % b6.off_pct)
ok(pct_of(EMPTY) >= 8, '关机线 %dmV 在表上就是 %d%%（留 10%% 余量）' % (EMPTY, pct_of(EMPTY)))
ok(b6.off_mv >= 3000, '关机时开路 %dmV 还在 3.0V 之上（深放伤电芯）' % b6.off_mv)
ok(any(x[0] == 'off' for x in b6.voice), '关机前播了 VO_OFF')

print('\n[10] 充电时不许念「电量低」')
b7 = Batt()
b7.run(3600, 120)                # 3.60V ≈ 14%，低电
n_low = len([x for x in b7.voice if x[0] == 'low'])
ok(n_low >= 1, '放电到 14%% 会念（%d 次）' % n_low)
b7.run(4260, 120)
ok(b7.st == ST_CHG, '在充电（st=%d）' % b7.st)
ok(len([x for x in b7.voice if x[0] == 'low']) == n_low,
   '插上就不念了（总次数还是 %d）' % n_low)

print('\n[11] 分压系数自校准（CV 平台 = 4.20V 硬基准）')
b8 = Batt()
b8.run(3700, 60)
b8.run(4170, 6 * 60)             # 平台 4165：比 4200 低 0.8% → 小于 1.5% 阈值，不动
ok(abs(b8.scale - 1.0) < 1e-6, '偏差 0.8%% 不动（scale=%.4f，mv=%d）' % (b8.scale, b8.mv))
b8b = Batt()
b8b.run(3700, 60)
b8b.run(4135, 6 * 60)            # 平台 4130：比 4200 低 1.7% → 学
ok(b8b.scale > 1.01, '偏差 1.7%% 学了（scale=%.4f，%s）'
   % (b8b.scale, [x for x in b8b.log if x.startswith('scale')]))

# ============================================================
#  三、源码静态断言（镜像会不会失真，靠这一节兜）
# ============================================================
print('\n[12] 源码里的关键点')


def strip_comments(t):
    """断言"代码里没有它"之前先剥掉 // 注释 —— 头部注释正是在讲这个坑"""
    return re.sub(r'//[^\n]*', '', t)
# 版本号不再写死：改成「config.h 的 FW_VER == CHANGELOG 最新条目」。
#   写死过一次的代价是：每次升版本这条就假红（1.46.65 就撞上了），
#   而真正该守的是「升了版本就必须写改动记录」。
fw = re.search(r'#define\s+FW_VER\s+"([0-9.]+)"', cf)
_clf = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'CHANGELOG.md')
cl = re.search(r'^##\s+([0-9.]+)', io.open(_clf, encoding='utf-8').read(), re.M)
ok(fw is not None and cl is not None and fw.group(1) == cl.group(1),
   'FW_VER == CHANGELOG 最新条目（%s / %s）' % (fw.group(1) if fw else '?', cl.group(1) if cl else '?'))
for m in ('ST_DISC', 'ST_PLUG_WAIT', 'ST_CHG', 'ST_UNPLUG_WAIT'):
    ok(m in bc, '状态机里有 %s' % m)
ok('enum { ST_DISC = 0, ST_PLUG_WAIT, ST_CHG, ST_UNPLUG_WAIT };' in bc, '状态枚举一行写完')
ok('BATT_PLUG_NEAR_MV' not in strip_comments(bc) and 'BATT_PLUG_NEAR_MV' not in strip_comments(cf),
   '致命门槛 BATT_PLUG_NEAR_MV(4050) 已彻底删除（就是它让充电分支从没执行过；注释里还提）')
ok('s_chgTrack' not in bc and 's_chgPrev' not in bc and 's_cvBase' not in bc,
   '按"电压涨幅"推电量的那套残留已清掉（改成纯时间爬）')

chg_seg = bc[bc.index('static void chgTick'):bc.index('void batteryTick')]
ok('BATT_CHG_PCT_MS' in chg_seg and 's_anchor' in chg_seg, '充电分支按时间爬（锚点 + 时间）')
ok('pctOf(s_mv)' not in chg_seg, '充电分支不拿终端电压查表')
ok('s_flatAt' in chg_seg and 'BATT_CV_FLAT_MV' in chg_seg, '平台跟踪在 chgTick 里')
ok('BATT_CHG_PLATEAU_FULL_MS' in bc, '满电判据是平台时长（不是电压到位）')

un_seg = bc[bc.index('case ST_UNPLUG_WAIT:'):bc.index('s_prevForFlat = s_mv;')]
ok('winMax() - winMin() < BATT_UNPLUG_FLAT_MV' in un_seg,
   '拔线后等"平"用的是窗口跨度，不是相邻两点比较')

disc_seg = bc[bc.index('// ---- 放电：开路电压 → 表 ----'):bc.index('// ---- 要显示 0% 了')]
ok('readNoLoadMv(BATT_OCV_MUTE_MS)' in disc_seg, '放电时关功放取开路电压')
ok('pctOf(s_vSoc)' in disc_seg, '查表用的是平滑后的开路电压')

# ---- 1.46.66：这两条是真机上才露出来的，仿真喂的都是理想值，所以一直没红 ----
ok('f - s_mv' in disc_seg, '压降 = 开路 − 带载（正数）；写成 s_mv - f 会得负数被滤掉')
ok('sag > -60' in disc_seg, '压降下限 -60（原来 -120，把真机 -270 全滤掉了）')
ok('audioIsPlaying() || A.fmMode' in disc_seg, '收音模式也算「功放在响」，否则拿带载值当开路值')
ok('BATT_QUIET_AS_OCV_MS' in disc_seg, '连续安静才认开路（audioIsPlaying 播放中会抖）')
ok('BATT_QUIET_AS_OCV_MS' in cf, 'config.h 里有这个窗口')

off_seg = bc[bc.index('// ---- 没电：撑满'):bc.index('if (s_offSaid && (!voiceBusy()')]
ok('mvEmpty <= BATT_EMPTY_MV' in off_seg, '关机看开路电压（mvEmpty），不看带载值')
ok('s_seenHigh' in off_seg, '关机还有「见过高位」这道闸')

ok('s_scale = ns' in bc and '4200.0f / (float)s_mv' in bc, 'CV 平台自校准在')
ok('BATT_SEEN_HIGH_MV' in cf and 'BATT_EMPTY_MV' in cf, '两个电压阈值都在 config.h')
for k in ('bst', 'banc', 'bocv', 'bstep', 'bscale'):
    ok(('\\"%s\\"' % k) in bc, '/api/batt 里有诊断字段 %s' % k)

# ============================================================
#  四、双击 → 回到第一个台（跟电量无关，1.46.55 一并加的，留着一起跑）
# ============================================================
print('\n[13] 双击 → 回到第一个台')
ic = io.open(os.path.join(ROOT, 'src', 'input.cpp'), encoding='utf-8').read()
ok('goFirstStation' in ic, '有 goFirstStation()')
i = ic.find('static void goFirstStation')
j = ic.find('static void updatePreview')
gfs = ic[i:j] if 0 <= i < j else ''
ok('albumEntry(0)' in gfs, '网络模式落到台表第 0 项的单元入口')
ok('A.cur = land' in gfs and 'audioPlayIndex(land)' in gfs, '先改 A.cur 再发播放命令')
ok('fmScanFreq(0)' in gfs, '收音模式回到搜到的第一个台')
ok('98000' in gfs, '一个 FM 台都没有时落到 98.0')
ok('s_pending = 0' in gfs and 'A.preview = -1' in gfs, '双击时清掉待判格数和预览')

m3 = re.search(r'else if \(s_waitDbl && now - s_upAt < BTN_DBL_MS\) \{(.*?)\n      \} else \{', ic, re.S)
ok(bool(m3), '找到双击分支')
ok('goFirstStation()' in (m3.group(1) if m3 else ''), '双击分支里调了 goFirstStation()')
ok('s_volTurned' in ic and 's_apLong' in ic, '长按 / 按住转音量的逻辑没被带坏')

print('\n' + ('全部 %d / %d 通过' % (oks, oks + len(fails))))
if fails:
    print('失败：')
    for f in fails:
        print('  -', f)
sys.exit(1 if fails else 0)
