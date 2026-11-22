# -*- coding: utf-8 -*-
"""
verify_agcnoise.py —— 「自动增益 / 无信号噪声」回归（1.46.0 起，1.46.30 重写）

三件事：
  ① 把 audio_process_extern 里的 AGC 逐行复刻成 Python，喂不同响度的源，
     断言"收敛后各台落在同一个响度上"（用户说的"一会大一会小"）；
  ② 1.46.30：有声书单独一组 —— 人声峰均比高，断言它也能抬到 BOOK 目标，
     并且用旧参数（峰值上限 42000）跑同一份数据会**抬不动**（这就是"书比台小很多"的根因）；
  ③ 静音块 → 补噪声的计数逻辑，以及源码守卫（改坏就红）。

1.46.30 之前的版本把峰均比写成 3 dB（peak = rms×1.41），跟真实节目差 7~15 dB，
所以"峰值上限把增益卡死"这件事一直没被测出来 —— 现在按实测的峰均比喂。
"""
import io, math, os, re, sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL = []
BLK_S = 0.023          # 一块 ≈ 23 ms（1024 帧 / 44.1kHz）


def chk(name, cond, extra=''):
    if cond:
        print('  OK   %s' % name)
    else:
        print('  FAIL %s  %s' % (name, extra))
        FAIL.append(name)


def src(rel):
    return io.open(os.path.join(BASE, rel), encoding='utf-8').read()


def cfg(name, default=None):
    s = src('src/config.h')
    m = re.search(r'^#define\s+%s\s+([-\w.]+)' % name, s, re.M)
    if not m:
        return default
    v = m.group(1).rstrip('f')
    try:
        return float(v) if '.' in v else int(v)
    except ValueError:
        return default


# ---------------------------------------------------------------- 参数
TGT      = cfg('AGC_TARGET_RMS')
BTGT     = cfg('AGC_TARGET_RMS_BOOK')
MAXG     = cfg('AGC_MAX_GAIN')
BMAXG    = cfg('AGC_MAX_GAIN_BOOK')
MING     = cfg('AGC_MIN_GAIN')
STEP_UP  = cfg('AGC_STEP_UP')
STEP_DN  = cfg('AGC_STEP_DN')
UP_GATE  = cfg('AGC_UP_GATE_PK')
FAST     = cfg('AGC_FAST_GAIN')
DECAY    = cfg('AGC_PK_ENV_DECAY')
CEIL     = cfg('AGC_PEAK_CEIL')
BCEIL    = cfg('AGC_PEAK_CEIL_BOOK')
SILPK    = cfg('AGC_SILENT_PK')
SILBLK   = cfg('NOISE_SIL_BLOCKS')
CACHE_N  = cfg('AGC_GAIN_CACHE_N')

print('AGC：台 target=%s max=%s ceil=%s | 书 target=%s max=%s ceil=%s' % (TGT, MAXG, CEIL, BTGT, BMAXG, BCEIL))
print('     step_up=%s step_dn=%s 上加门=%s 快速接入=%s 包络衰减=%s' % (STEP_UP, STEP_DN, UP_GATE, FAST, DECAY))


def db2rms(db):
    return 32768.0 * (10 ** (db / 20.0))


def rms2db(x):
    return 20 * math.log10(max(x, 1e-9) / 32768.0)


def limiter(v):
    """nzLim：26000 以下直通，往上 R·x/(x+R) 渐近 34000，最后夹 32600"""
    a = abs(v)
    if a <= 26000:
        return v
    x = a - 26000
    y = 26000 + 8000 * x / (x + 8000.0)
    y = min(y, 32600)
    return y if v >= 0 else -y


class Agc:
    """逐行复刻 audio.cpp 里的 AGC（1.46.30 口径）"""

    def __init__(self, book=False, gain=1.0, ceil=None):
        self.book = book
        self.gain = gain
        self.env = 0.0
        self.fresh = True
        self.tgt = BTGT if book else TGT
        self.gmax = BMAXG if book else MAXG
        self.ceil = BCEIL if book else CEIL
        if ceil is not None:
            self.ceil = ceil          # 用来复现旧参数

    def block(self, rms, pk):
        self.env = max(pk, self.env * DECAY)
        if pk < SILPK:                       # 静音块不动增益
            return self.gain
        if rms <= 1.0:
            return self.gain
        want = self.tgt / rms
        want = min(want, self.gmax)
        want = max(want, MING)
        pe = self.env if self.env else pk
        if pe > 0:
            want = min(want, self.ceil / pe)
            want = max(want, 0.05)
        cur = self.gain
        if want > cur and pk < UP_GATE:       # 轻的块不许加增益
            want = cur
        if want > cur:                       # 乘性：恒定 dB/秒
            cur = min(cur * (1.0 + STEP_UP), want)
        else:
            cur = max(cur * (1.0 - STEP_DN), want)
        if self.fresh and pk >= UP_GATE:      # 换台头一块：直接接到 3 倍以内
            self.fresh = False
            cur = max(cur, min(want, FAST))
        self.gain = cur
        return cur


def run(rms, pk, blocks=1200, book=False, gain=1.0, ceil=None):
    a = Agc(book=book, gain=gain, ceil=ceil)
    for _ in range(blocks):
        a.block(rms, pk)
    return a


# ---------------------------------------------------------------- ① 电台
print('\n[1] 电台：14 dB 的源差收敛到同一响度（峰均比按实测 10 dB）')
stations = [(-8.2, '成都FM96.5 最响'), (-11.6, '成都年代音乐'), (-15.2, '成都经济广播'),
            (-20.0, '成都FM94.6 最轻'), (-23.1, '西安新闻'), (-17.0, '唐诗电台')]
outs, gains = [], []
for db, nm in stations:
    rms = db2rms(db)
    pk = rms * 3.16                        # 10 dB 峰均比
    a = run(rms, pk)
    out = rms2db(rms * a.gain)
    outs.append(out)
    gains.append(a.gain)
    print('    %-18s 源 %6.1f dBFS  增益 %.2f 倍  输出 %6.1f dBFS  峰值上限 %.1f 倍'
          % (nm, db, a.gain, out, a.ceil / pk))
spread_before = max(d for d, _ in stations) - min(d for d, _ in stations)
spread_after = max(outs) - min(outs)
print('    跨度：处理前 %.1f dB → 处理后 %.1f dB' % (spread_before, spread_after))
chk('源跨度 ≥ 12 dB（这就是用户说的"一会大一会小"）', spread_before >= 12.0, spread_before)
chk('AGC 之后各台跨度 ≤ 2 dB', spread_after <= 2.0, spread_after)
tgt_db = rms2db(TGT)
chk('最轻的台抬到目标 ±2 dB', abs(outs[3] - tgt_db) <= 2.0, (outs[3], tgt_db))
chk('最响的台压到目标 ±2 dB', abs(outs[0] - tgt_db) <= 2.0, (outs[0], tgt_db))
chk('增益不超过 AGC_MAX_GAIN', all(g <= MAXG + 1e-6 for g in gains), gains)
chk('量程覆盖 -8 ~ -35 dBFS', MAXG >= 4 and MING <= 0.3, (MAXG, MING))

# ---------------------------------------------------------------- ② 有声书
print('\n[2] 有声书：人声峰均比 16 dB，也要抬到 BOOK 目标')
books = [(-29.0, '轻的书'), (-26.0, '中等'), (-23.0, '响的书'), (-33.0, '特别轻')]
bouts, bgains = [], []
for db, nm in books:
    rms = db2rms(db)
    pk = rms * 6.31                        # 16 dB 峰均比（人声）
    a = run(rms, pk, book=True)
    out = rms2db(rms * a.gain)
    bouts.append(out)
    bgains.append(a.gain)
    peak_out = pk * a.gain
    print('    %-8s 源 %6.1f dBFS  增益 %.2f 倍  输出 %6.1f dBFS  峰值 %6.0f → 限幅后 %5.0f'
          % (nm, db, a.gain, out, peak_out, limiter(peak_out)))
btgt_db = rms2db(BTGT)
chk('每本书都收敛到 BOOK 目标 ±2 dB', all(abs(o - btgt_db) <= 2.0 for o in bouts), (bouts, btgt_db))
chk('书与书之间跨度 ≤ 2 dB（同一档音量听感一致）', max(bouts) - min(bouts) <= 2.0,
    max(bouts) - min(bouts))
chk('书比电台目标高 3~6 dB（人声同 RMS 听着更轻，要补回来）',
    3.0 <= btgt_db - tgt_db <= 6.0, btgt_db - tgt_db)
chk('书的增益上限够用（-33 dBFS 也抬得动）', BMAXG >= 10.0, BMAXG)

print('\n[2b] 根因复核：旧峰值上限能容多大的峰均比？')
# 目标 / 上限 = 允许的峰均比上限（线性）。超过它，增益就被钉死、抬不到目标。
max_crest_old = 42000.0 / 6400.0
max_crest_new = BCEIL / BTGT
print('    旧参数（上限 42000 / 目标 6400）：最多容 %.1f 倍峰均比 = %.1f dB'
      % (max_crest_old, 20 * math.log10(max_crest_old)))
print('    新参数（上限 %.0f / 目标 %.0f）：最多容 %.1f 倍 = %.1f dB'
      % (BCEIL, BTGT, max_crest_new, 20 * math.log10(max_crest_new)))
chk('新参数能容 20 dB 以上的峰均比（人声实测 18~22 dB）',
    20 * math.log10(max_crest_new) >= 20.0, 20 * math.log10(max_crest_new))
chk('旧参数只有 16 dB 出头 —— 峰均比一过就被钉死', 20 * math.log10(max_crest_old) <= 17.0,
    20 * math.log10(max_crest_old))

rows = []
for crest_db in (16.0, 18.0, 20.0, 22.0):
    rms = db2rms(-29.0)                    # 有声书常见源电平
    pk = rms * 10 ** (crest_db / 20.0)
    a_old = Agc(book=True, ceil=42000.0)
    a_old.tgt = 6400.0
    a_old.gmax = 9.0
    a_new = run(rms, pk, book=True)
    for _ in range(1200):
        a_old.block(rms, pk)
    o_old = rms2db(rms * a_old.gain)
    o_new = rms2db(rms * a_new.gain)
    rows.append((crest_db, o_old, o_new, a_old.gain, a_new.gain))
    print('    峰均比 %4.1f dB：旧 %.2f 倍 → %6.1f dBFS ｜ 新 %.2f 倍 → %6.1f dBFS ｜ 差 %+.1f dB'
          % (crest_db, a_old.gain, o_old, a_new.gain, o_new, o_new - o_old))

worst = [r for r in rows if r[0] >= 20.0]
chk('峰均比 ≥20 dB 的书：新参数比旧参数响 ≥ 3 dB',
    all(r[2] - r[1] >= 3.0 for r in worst), [(r[0], r[2] - r[1]) for r in worst])
chk('峰均比 16 dB 时两者差不多（没把好走的路也改坏）',
    abs(rows[0][2] - rows[0][1]) <= 1.5, (rows[0],))

# ---------------------------------------------------------------- ③ 安全 / 行为
print('\n[3] 安全与行为')
for db, nm in books:
    rms = db2rms(db)
    pk = rms * 6.31
    a = run(rms, pk, book=True)
    chk('%s：峰值 × 增益 ≤ 上限（不靠削顶变响）' % nm, pk * a.gain <= BCEIL + 1.0,
        (pk * a.gain, BCEIL))
    chk('%s：限幅后不超过 32600（不会硬削）' % nm, abs(limiter(pk * a.gain)) <= 32600)

# 留白/换气不许把增益抬上去
a = Agc(book=True, gain=2.0)
a.fresh = False
before = a.gain
for _ in range(200):
    a.block(180.0, 300.0)                 # 轻的底噪/留白：RMS 很小但峰值也小
chk('轻的块（峰值 < 上加门）不涨增益', abs(a.gain - before) < 1e-6, (before, a.gain))

# 覆盖：先遇留白再遇人声，增益仍然能涨（不能永久卡住）
a = Agc(book=True)
for _ in range(40):
    a.block(180.0, 300.0)
g1 = a.gain
for _ in range(300):
    a.block(db2rms(-29.0), db2rms(-29.0) * 6.31)
chk('留白之后遇到人声仍然能涨（不是永久卡住）', a.gain > g1 + 1.0, (g1, a.gain))

# 换台头一块：快速接入 + 不超 3 倍
a = Agc(book=True)
a.block(db2rms(-29.0), db2rms(-29.0) * 6.31)
chk('换台头一块就接到 3 倍（不是从 1.0 慢慢爬）', abs(a.gain - FAST) < 1e-6, a.gain)
chk('快速接入不超过 AGC_FAST_GAIN', a.gain <= FAST + 1e-6, a.gain)


def blocks_to(db_in, book=False):
    rms = db2rms(db_in)
    pk = rms * (6.31 if book else 3.16)
    a = Agc(book=book)
    tg = BTGT if book else TGT
    for b in range(1, 3000):
        a.block(rms, pk)
        if abs(rms2db(rms * a.gain) - rms2db(tg)) <= 1.0:
            return b
    return 3000


print('\n[4] 收敛速度（不能"抽气"，也不能慢到用户察觉）')
for db, nm, bk in [(-20.0, '轻台', False), (-8.2, '响台', False), (-33.0, '特别轻的书', True)]:
    b = blocks_to(db, bk)
    print('    %-10s 源 %5.1f dBFS → %d 块（≈%.1f 秒）进入目标 ±1 dB' % (nm, db, b, b * BLK_S))
    chk('%s 8 秒内到位' % nm, b * BLK_S <= 8.0, b * BLK_S)
chk('下降比上升快（压下来要快，抬上去别抽气）', STEP_DN > STEP_UP * 1.5, (STEP_UP, STEP_DN))

# ---------------------------------------------------------------- ⑤ 静音
print('\n[5] 静音 → 补噪声')
sil, triggered = 0, None
for i in range(200):
    pk = 9000 if i < 30 else 0
    sil = sil + 1 if pk < SILPK else 0
    if sil >= SILBLK and triggered is None:
        triggered = i
print('    第 %s 块开始补噪声（阈值 %d 块）' % (triggered, SILBLK))
chk('静音持续满阈值才补噪声', triggered is not None and triggered - 30 >= SILBLK - 1)
chk('阈值对应约 1 秒（20~60 块）', 20 <= SILBLK <= 60, SILBLK)
chk('真声音回来立刻停止补噪声', (0 if 9000 >= SILPK else SILBLK) == 0)

# ---------------------------------------------------------------- ⑥ 源码守卫
print('\n[6] 源码守卫（改坏就红）')
A = src('src/audio.cpp')
H = src('src/audio.h')
N = src('src/net.cpp')

chk('PCM 回调里先量原始电平、再补噪声', A.find('healthSamples(buff, len)') < A.find('nzFill(buff, n)'))
chk('静音流补噪声走 nzFill', 'if (s_silBlocks >= NOISE_SIL_BLOCKS) {' in A)
chk('AGC 只在有声块上收敛', 'if (pk >= AGC_SILENT_PK && !s_noiseOn && !s_voiceOn && !s_swOn)' in A)
chk('软限幅保护', 'static inline int16_t nzLim' in A)

chk('有声书用专属峰值上限', 'isBook ? AGC_PEAK_CEIL_BOOK : AGC_PEAK_CEIL' in A)
chk('峰值上限按包络算、不按单块峰值', 'const uint32_t pe = s_pkEnv ? s_pkEnv : pk;' in A)
chk('包络每块缓降', 's_pkEnv = (pk > env) ? pk : env;' in A)
chk('轻的块不许加增益（AGC_UP_GATE_PK）', 'if (want > cur && pk < AGC_UP_GATE_PK) want = cur;' in A)
chk('上升/下降分速', 'AGC_STEP_UP' in A and 'AGC_STEP_DN' in A)
chk('换台头一块快速接入', 'AGC_FAST_GAIN' in A and 's_agcFresh' in A)

chk('每台的增益缓存', 'static int16_t  s_gainCache[AGC_GAIN_CACHE_N]' in A)
chk('换台时按缓存接上增益', 's_gainCache[idx] > 0) s_agcQ10 = s_gainCache[idx];' in A)
chk('噪声回来后也接回缓存', 's_agcRst' in A and A.count('s_agcRst') >= 3)
chk('缓存写回只在 AGC 分支里', A.count('s_gainCache[A.cur] = (int16_t)s_agcQ10') == 1)
chk('缓存只放 RAM（没写进 flash）', 's_gainCache' not in src('src/store.cpp'))

chk('/api/diag 暴露 arms', 'audioAgcRms()' in N)
chk('/api/diag 暴露 apk', 'audioAgcPeak()' in N)
chk('audio.h 声明两个新接口', 'audioAgcRms()' in H and 'audioAgcPeak()' in H)
_fwm = re.search(r'#define\s+FW_VER\s+"([^"]+)"', src('src/config.h'))
chk('版本不低于 1.46.30（AGC 重写那一版）',
    bool(_fwm) and _fwm.group(1) >= '1.46.30', _fwm.group(1) if _fwm else None)

# 温度降载那半边（"一会大一会小"的另一个根因）
print('\n[7] 温度降载（同一条听感问题的另一半）')
chk('一档降载不再压音量', cfg('PWR_T1_VOL_PCT') == 100, cfg('PWR_T1_VOL_PCT'))
chk('一档温度提到 56℃', cfg('PWR_T1_C') == 56, cfg('PWR_T1_C'))
chk('二档仍然压音量（安全没丢）', cfg('PWR_T2_VOL_PCT') == 50, cfg('PWR_T2_VOL_PCT'))
chk('过热暂停档没动', cfg('PWR_T3_C') == 68, cfg('PWR_T3_C'))

print('\n==== 失败 %d 项 ====' % len(FAIL))
for f in FAIL:
    print('  -', f)
sys.exit(1 if FAIL else 0)
