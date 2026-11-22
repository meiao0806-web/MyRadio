# -*- coding: utf-8 -*-
"""
verify_fm_manual.py —— 1.46.59「FM 改手动调频 + 修只搜到一个台」的回归

两段：
  A. 源码级断言（旋钮不再走台表、进收音不再自动搜台、屏上不再写"没有台"…）
  B. 搜台状态机仿真：拿一个"调谐要 90ms、期间拒收 SEEK"的假芯片，
     分别跑旧算法（每轮都 TUNE 一次）和新算法（每波段只 TUNE 一次），
     要求旧算法 ≤1 个台、新算法拿全。

用法：python tools/verify_fm_manual.py
"""
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL = []
N = [0]

def ok(cond, msg):
    N[0] += 1
    if cond:
        print('  OK  ' + msg)
    else:
        print('  X   ' + msg)
        FAIL.append(msg)

def rd(p):
    return io.open(os.path.join(ROOT, p), encoding='utf-8').read()

fm     = rd('src/fm.cpp')
inp    = rd('src/input.cpp')
mainc  = rd('src/main.cpp')
ui     = rd('src/ui.cpp')
api    = rd('src/api.cpp')
store  = rd('src/store.cpp')
cfg    = rd('src/config.h')
prev   = rd('tools/preview_template.html')

print('== A) 源码级 ==')

# --- 版本 ---
m = re.search(r'#define\s+FW_VER\s+"([\d.]+)"', cfg)
ver = tuple(int(x) for x in m.group(1).split('.')) if m else (0,)
ok(ver >= (1, 46, 62), '版本号 >= 1.46.62（当前 %s）' % '.'.join(map(str, ver)))

# --- 1.46.60：SET_PROPERTY 帧必须是 6 字节 ---
#   多一个保留字节，芯片就会把 属性号/值 错位解析 ⇒ 一条属性都写不进去，
#   而且只回 ERR 位、日志里看不出异常（音量回读 63 而不是 53 就是它）。
i0 = fm.find('static bool setProp(')
i1 = fm.find('return waitCts(200);', i0)
sp = fm[i0:i1]
ok(sp.count('Wire.write(0x00)') == 1,
   'setProp 只有一个保留字节（帧 = 命令+保留+属性2+值2 = 6 字节）')
ok('Wire.write((uint8_t)(prop >> 8))' in sp and 'Wire.write((uint8_t)(val  >> 8))' in sp,
   'setProp 仍按 属性(2) + 值(2) 发')
ok('属性回读' in fm, 'fmBegin 里有属性回读自检（软静音/音量/硬静音/SEEK下限）')
ok('PROP_FM_SOFT_MUTE_MAX_ATTEN, FM_SOFTMUTE_ATTEN' in fm,
   '软静音 0 仍然真的写下去（1.46.60 起这条才真正生效）')

# --- 1.46.62：POWER_UP 的 ARG2 必须是 0b0101（模拟音频输出）---
#   写 0x00 时芯片照样应答、能调、能读 RSSI/SNR，就是耳机口一点声音都没有。
i0 = fm.find('static bool chipPowerUp')
i1 = fm.find('return waitCts(600);', i0)
pu = fm[i0:i1]
ok('Wire.write((uint8_t)0x05);' in pu,
   'POWER_UP ARG2 = 0x05（SI473X_ANALOG_AUDIO，同原厂库）')
ok('0x10 | (am ? 0x01 : 0x00)' in pu,
   'POWER_UP ARG1 保留 XOSCEN=1（实测去掉后芯片不响应）')
ok('fmDbgTick' not in mainc and '开机 5 秒 → 进 FM' not in mainc,
   '临时诊断（fmDbgTick / 开机自动进FM+扫台）已从 main.cpp 删掉')

# --- 旋钮：收音模式走频率，不再走台表 ---
i0 = inp.find('static void radioTurn(int clicks)')
i1 = inp.find('static int stepUnit(int base, int steps);', i0)
body = inp[i0:i1]
ok('fmScanStep(' not in body, 'radioTurn 里不再调 fmScanStep（旋钮=手动调频）')
ok('radioWalk(fmFreq(), clicks, gear)' in body, 'radioTurn 里仍有 radioWalk(fmFreq(), clicks, gear)')

# --- 进收音不再自动搜台 ---
i0 = mainc.find('static void enterFm()')
i1 = mainc.find('static void exitFm()')
ef = mainc[i0:i1]
ok('fmScanWantSoon(' not in ef, 'enterFm 里不再排自动搜台（fmScanWantSoon 已移除）')
ok('fmTune(fmScanFreq(0))' not in ef, 'enterFm 里不再跳到台表第一个台')
ok('fmScanLoad();' in ef, 'enterFm 仍读台表（只给 App 翻台用）')

# --- 屏上不再有「没有台」这一屏 ---
ok('cnCenterS("没有台"' not in ui, 'ui.cpp 删掉了「没有台」大屏')
ok('没有电台' in ui, '网络模式的「没有电台」还在（没误删）')
ok("center('没有台'" not in prev, '预览页也删掉了「没有台」')
ok('正在搜台 %d%%' in ui, '「正在搜台 N%」保留（App 手动搜台时还要用）')

# --- 搜台状态机：每波段只 TUNE 一次 ---
i0 = fm.find('static void scanTick()')
i1 = fm.find('void fmScanSave()', i0)
st = fm[i0:i1]
ok('static bool     s_scanStarted' in fm or 'static bool s_scanStarted' in fm or 's_scanStarted = false;' in fm,
   '有 s_scanStarted 标志')
ok('if (!s_scanStarted) {' in st, 'scanTick 里"只在开头定位一次"的分支存在')
j = st.find('if (!s_scanStarted) {')
k = st.find('tuneFmRaw(s_scanCur / 10)', j)
ok(j >= 0 and k > j, 'tuneFmRaw 被包在 if (!s_scanStarted) 里面')
ok('waitStc(' in st, '定位之后等 STC（不是只等 CTS）')
ok(st.count('tuneFmRaw(s_scanCur / 10)') == 1, 'scanTick 里 tuneFmRaw 只出现 1 次（不会每轮重调）')
# 波段切换 / 开搜 / 中止 都要把标志清掉
ok(fm.count('s_scanStarted = false;') >= 4, 's_scanStarted 在开搜/换波段/切模式/中止 4 处都清了')

# --- 波段顶端不再被当成一个台 ---
ok('f > s_scanCur && f < hi' in st, '判据是 f < hi（顶端 108.0 不算台）')
ok('f > s_scanCur && f <= hi' not in st, '旧的 <= hi 已改掉')

# --- SEEK 失败要重试，不是立刻判完 ---
ok('s_scanStall >= 6' in st, 'SEEK 没前进时重试 6 轮才跳过本波段')

# --- 软静音放开（两处：fmBegin + switchMode）---
ok('FM_SOFTMUTE_ATTEN' in cfg, 'config.h 有 FM_SOFTMUTE_ATTEN')
ok(re.search(r'#define\s+FM_SOFTMUTE_ATTEN\s+0', cfg) is not None, 'FM_SOFTMUTE_ATTEN = 0（关软静音）')
ok(fm.count('setProp(PROP_FM_SOFT_MUTE_MAX_ATTEN') == 2,
   '软静音在 fmBegin 和 switchMode 两处都设了（POWER_UP 会复位属性）')

# --- 频率记忆 ---
ok(re.search(r'if \(k == "fmfreq"\)', store) is not None, 'store.cpp 读回 fmfreq')
ok('fmfreq=%d' in store, 'store.cpp 存盘带 fmfreq')
ok('storeSaveDebounced();' in fm, 'fmTune 里落盘（1.5 秒合并）')
ok('A.fmFreq = k;' in fm, 'fmTune 同步 A.fmFreq（屏上跟手）')

# --- App 翻台兜底 ---
ok('fmScanCount() > 1' in api, 'App 上一台/下一台：有台表走台表')
ok('radioWalk(f0, dir >= 0 ? 1 : -1, 5)' in api, 'App 上一台/下一台：没台表走 ±0.5MHz')

print('\n== B) 搜台状态机仿真（假芯片：调谐 90ms，期间拒收 SEEK）==')

BAND_LO, BAND_HI = 64000, 108000
STATIONS = [87500, 91500, 93800, 98000, 104300, 106800]
TUNE_MS  = 90        # 芯片调谐要这么久
CTS_MS   = 60        # tuneFmRaw 里的 waitCts(60)：只等"命令被收下"


class Chip:
    def __init__(self):
        self.freq = 98000
        self.busy_until = 0

    def tune(self, khz, t):
        self.freq = khz
        self.busy_until = t + TUNE_MS
        return t + CTS_MS          # 命令被收下的时刻（tuneFmRaw 返回）

    def seek(self, t):
        """返回 (新频率, 是否到顶)。还在调谐里就拒收，频率不动。"""
        if t < self.busy_until:
            return self.freq, False
        up = [s for s in STATIONS if s > self.freq]
        if up:
            self.freq = min(up)
            return self.freq, False
        self.freq = BAND_HI
        return BAND_HI, True


def scan(per_round_tune):
    """per_round_tune=True 跑旧算法；False 跑新算法。返回搜到的台。"""
    chip = Chip()
    found, cur, t = [], BAND_LO, 0
    started = False
    for _ in range(400):
        if per_round_tune:
            tune_at = chip.tune(cur, t)          # 每轮都 TUNE
            t = tune_at                           # 60ms 后发 SEEK
            f, top = chip.seek(t)
            if f > cur and f <= BAND_HI:          # 旧判据：<= hi
                found.append(f); cur = f
            else:
                break                             # 整个波段当场判完
            t += 40
        else:
            if not started:                       # 只在开头定位一次
                t = chip.tune(cur, t)
                t = t - CTS_MS + max(TUNE_MS, CTS_MS)   # waitStc 等到真锁定
                started = True
                t += 30
                continue
            f, top = chip.seek(t)
            if f > cur and f < BAND_HI:           # 新判据：< hi
                found.append(f); cur = f
            elif f >= BAND_HI:
                break
            t += 40 + 5                            # 每轮还有一次等待/轮询开销
    return found


old = scan(True)
new = scan(False)
print('  旧算法：%d 个台 %s' % (len(old), old))
print('  新算法：%d 个台 %s' % (len(new), new))
ok(len(old) <= 1, '旧算法最多只搜到 1 个台（复现用户说的「只有一个台」）')
ok(len(new) == len(STATIONS), '新算法把 %d 个台全搜到' % len(STATIONS))
ok(BAND_HI not in new, '波段顶端 108.0MHz 没被当成一个台')

print('\n通过 %d / 失败 %d' % (N[0], len(FAIL)))
for f in FAIL:
    print('  FAIL:', f)
sys.exit(1 if FAIL else 0)
