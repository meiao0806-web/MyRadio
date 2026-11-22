# -*- coding: utf-8 -*-
"""verify_offair.py —— 1.46.6「停播台自动跳过」的回归（不需要设备）

两部分：
  A. 逻辑复刻：把 offair.cpp 的判据（退避/跳过/到期复查/全表兜底/导航）
     逐行照抄成 Python，喂真实台表跑断言。
  B. 源码级断言：宏从 config.h 读真值，检查「判定 → 跳过 → 复活」这条链
     在每个文件里都接上了（这类事编译器一声不响，只能靠这里守）。

用法：python tools/verify_offair.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def P(*a): return os.path.join(ROOT, *a)
def rd(*a): return io.open(P(*a), encoding='utf-8', errors='replace').read()

PASS = FAIL = 0
def ok(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print('  OK   ' + msg)
    else:
        FAIL += 1
        print('  FAIL ' + msg)

# ---------------------------------------------------------------- 读真值
cfg = rd('src', 'config.h')
def macro(name, cast=int):
    m = re.search(r'#define\s+%s\s+([0-9]+)' % name, cfg)
    assert m, 'config.h 里没有 %s' % name
    return cast(m.group(1))

OFF_ON      = macro('OFFAIR_ON')
OFF_MAXN    = macro('OFFAIR_MAX_N')
BASE_MIN    = macro('OFFAIR_BASE_MIN')
MAX_MIN     = macro('OFFAIR_MAX_MIN')
SIL_BLOCKS  = macro('OFFAIR_SIL_BLOCKS')
PROBE_MS    = macro('OFFAIR_PROBE_MS')
SAVE_MIN    = macro('OFFAIR_SAVE_MIN')

station_h = rd('src', 'preset_stations.h')
names = re.findall(r'\{ "([^"]*)", "([^"]*)" \}', station_h)
print('台表 %d 台；OFFAIR: on=%d maxn=%d base=%d cap=%d sil=%d probe=%dms save=%dmin'
      % (len(names), OFF_ON, OFF_MAXN, BASE_MIN, MAX_MIN, SIL_BLOCKS, PROBE_MS, SAVE_MIN))

# ---------------------------------------------------------------- A. 逻辑复刻
def backoff_min(miss):
    if miss <= 1: return BASE_MIN
    if miss == 2: return BASE_MIN * 2
    if miss == 3: return BASE_MIN * 3
    return MAX_MIN

class Ledger:
    """offair.cpp 的最小复刻：key=地址串，等价于 healthKeyOf()"""
    def __init__(self, st):
        self.st = st            # [(name, url)]
        self.sl = {}            # url -> [endMs, miss]
        self.on = bool(OFF_ON)
        self.now = 0
    def alive(self, s):
        e, miss = self.sl[s]
        return miss > 0 and e - self.now > 0
    def skip(self, i):
        if not self.on or not (0 <= i < len(self.st)): return False
        url = self.st[i][1]
        return url in self.sl and self.alive(url)
    def probe(self, i):
        if not self.on or not (0 <= i < len(self.st)): return False
        if self.st[i][1] not in self.sl: return False
        e, miss = self.sl[self.st[i][1]]
        return miss > 0 and e - self.now <= 0
    def mark(self, i, is_book=False):
        if not self.on or not (0 <= i < len(self.st)): return False
        if is_book: return False          # 有声书永远不进台账
        url = self.st[i][1]
        cur = self.sl.get(url, [0, 0])
        miss = min(cur[1] + 1, 6)
        self.sl[url] = [self.now + backoff_min(miss) * 60000, miss]
        return True
    def ok(self, i):
        url = self.st[i][1]
        if url in self.sl: self.sl[url] = [0, 0]
    def hidden(self):
        return sum(1 for s in self.sl if self.alive(s))
    def step(self, i, d):
        n = len(self.st)
        if n <= 0: return i
        if not self.on: return (i + d) % n
        j = i
        for _ in range(n):
            j = (j + d) % n
            if not self.skip(j): return j
        return (i + d) % n               # 整表都被跳过 → 退回普通一格
    def first_audible(self, i):
        if not self.on or not self.skip(i): return i
        j = i
        for _ in range(len(self.st)):
            j = (j + 1) % len(self.st)
            if not self.skip(j): return j
        return i

st = names[:12]
L = Ledger(st)

print('\n[A1] 判定一次就跳过，到点自动回来')
L.mark(3)
ok(L.skip(3) is True, '判停播后立刻被跳过')
ok(L.hidden() == 1, '屏上「跳过 N」= 1')
L.now += (BASE_MIN * 60000) - 1
ok(L.skip(3) is True, '差 1ms 没到期 → 仍然跳过')
L.now += 2
ok(L.skip(3) is False, '到点 → 回到列表')
ok(L.probe(3) is True, '到点的第一次被标成「复查」（9 秒短超时）')

print('\n[A2] 退避：连着判会越藏越久，但封顶')
mins = []
L2 = Ledger(st)
for k in range(6):
    L2.now = 0
    L2.mark(4)
    mins.append(round((L2.sl[st[4][1]][0] - L2.now) / 60000))
ok(mins[:4] == [BASE_MIN, BASE_MIN * 2, BASE_MIN * 3, MAX_MIN],
   '退避梯度 %s = 15/30/45/60' % mins[:4])
ok(all(m <= MAX_MIN for m in mins), '封顶不超过 %d 分钟（%s）' % (MAX_MIN, mins))

print('\n[A3] 出声就放回')
L3 = Ledger(st)
L3.mark(5)
ok(L3.skip(5), '先藏起来')
L3.ok(5)
ok(L3.skip(5) is False and L3.probe(5) is False, '出声后既不跳过也不再复查')
ok(L3.hidden() == 0, '「跳过 N」回到 0')

print('\n[A4] 有声书不进台账')
L4 = Ledger(st)
ok(L4.mark(6, is_book=True) is False, 'mark 直接拒绝（返回 False）')
ok(L4.skip(6) is False and L4.hidden() == 0, '书不会被藏起来')

print('\n[A5] 导航：转身越过停播台，绕一圈能回到原处')
L5 = Ledger(st)
for i in (2, 5, 8):
    L5.mark(i)
seen = []
j = 0
for _ in range(len(st) - 3):
    j = L5.step(j, +1)
    seen.append(j)
ok(all(not L5.skip(i) for i in seen), '连续转 %d 格全部落在能听的台上 %s' % (len(seen), seen))
ok(len(set(seen)) == len(st) - 3, '正好走遍剩下的 %d 台' % (len(st) - 3))
j = 0
for _ in range(len(st) - 3):
    j = L5.step(j, +1)
ok(j == 0, '绕一圈回到起点（不会漂）')
back = [L5.step(0, -1)]
ok(back[0] not in (2, 5, 8), '反向一格也跳过停播台（落在 #%d）' % back[0])

print('\n[A6] 整表都被跳过 → 退回普通换台，不能把旋钮卡死')
L6 = Ledger(st)
for i in range(len(st)):
    L6.mark(i)
ok(L6.step(0, +1) == 1, '全被跳过时仍能走到下一台（不是原地不动）')
L6.on = False
ok(L6.skip(0) is False, '总开关关掉 → 一台都不跳')

print('\n[A7] 开机跳过：上次那个停播了，直接落到能听的台')
L7 = Ledger(st)
L7.mark(0)
ok(L7.first_audible(0) == 1, '从 #0 起找到 #1')
for i in (1, 2, 3):
    L7.mark(i)
ok(L7.first_audible(0) == 4, '连着 4 台停播 → 落到 #4')
L8 = Ledger(st)
L8.mark(7)
ok(L8.first_audible(7) == 8, '从停播台自己开始找 → 往后第一个能听的')
ok(L8.first_audible(6) == 6, '本来就是能听的台 → 原地返回')

# ---------------------------------------------------------------- B. 源码级
print('\n[B] 源码接线')
src = {}
for f in os.listdir(P('src')):
    if f.endswith(('.cpp', '.h')):
        src[f] = rd('src', f)
off_h = src.get('offair.h', '')
off_c = src.get('offair.cpp', '')
ok(bool(off_h) and bool(off_c), 'offair.h / offair.cpp 都在')
ok('bool offairMark(int idx)' in off_h and 'int  offairStep(int idx, int dir)' in off_h,
   '对外接口齐全（mark / step / firstAudible / hidden / clear）')
for k in ('offairBegin', 'offairTick', 'offairClearAll', 'offairSetEnabled', 'offairJson',
          'offairProbe', 'offairFirstAudible', 'offairOk', 'offairHidden'):
    ok(k in off_h, 'offair.h 声明了 %s' % k)

ok('healthKeyOf' in src.get('health.h', '') and 'uint32_t healthKeyOf(int idx)' in src.get('health.cpp', ''),
   '地址指纹复用 healthKeyOf（两边不会各写一份 FNV 漂掉）')

a = src['audio.cpp']
ok('s_probe ? OFFAIR_PROBE_MS' in a, '到期复查用 9 秒短超时（OFFAIR_PROBE_MS）')
ok('offairProbe(idx)' in a, 'startIndex 里算了这台是不是「到期回来复查」')
ok('s_probe || s_noiseTries > 0' in a,
   '两回连不上才判停播（第一回仍补沙沙，和 1.46.0 的规矩一致）')
ok('offairMark(A.cur)' in a, 'audio 里真的会记台账')
ok('s_silBlocks >= OFFAIR_SIL_BLOCKS' in a and 's_silHit = true' in a,
   '静音流只置标志，不在解码回调里换台')
ok('if (s_silHit)' in a, '任务循环里处理静音流（换台发生在安全处）')
ok('offairOk(A.cur)' in a, '出声后从台账放回')
ok('i = offairStep(i, dir >= 0 ? 1 : -1)' in a, 'advance()（自动换台）也跳过停播台')
ok('offairStep(A.cur, (dir >= 0) ? 1 : -1)' in a, 'audioNext()（App 上一台/下一台）也跳过')
ok(a.index('s_silHit = true') > a.index('int16_t* buff'), '静音判据在 PCM 回调里（拿得到样本）')

i = src['input.cpp']
ok(i.count('offairStep(idx') == 2, '旋钮 stepUnit 正反两向都走 offairStep')
ok('albumStep(idx' not in i.split('static int stepUnit')[1].split('static void commitPending')[0],
   'stepUnit 里不再直接调 albumStep（漏掉一处就等于没跳过）')

m = src['main.cpp']
ok('offairBegin();' in m, 'setup 里挂台账')
ok('offairTick();' in m, 'loop 里跑倒计时')
ok('offairSkip(A.cur)' in m and 'offairFirstAudible(A.cur)' in m,
   '开机时上次那个台停播 → 直接改播能听的')

n = src['net.cpp']
ok('server.on("/api/offair",      HTTP_GET,  hOffair);' in n, 'GET /api/offair')
ok('server.on("/api/offair",      HTTP_POST, hOffair);' in n, 'POST /api/offair')
ok('offairClearAll()' in n, 'clear=1 能立刻全部放回（误判的逃生口）')

u = src['ui.cpp']
ok('offairHidden()' in u and '跳过%d' in u, '屏上右下角有「跳过 N」')
ok('l_offN != offairHidden()' in u, 'N 变了只重画台名区（不整屏闪）')

ap = src['api.cpp']
ok('offairSkip(i)' in ap and r'\"off\":1' in ap, '/api/stations 里标了 off')

# 版本号
v = re.search(r'#define\s+FW_VER\s+"([^"]+)"', cfg).group(1)
# 版本号别钉死：这条功能是 1.46.6 进来的，之后每版都会往上走。
#   钉死的结果是「每次改版本都要动回归」，而且是假 FAIL。
vt = tuple(int(x) for x in v.split('.'))
ok(vt >= (1, 46, 6), 'FW_VER = %s（≥ 1.46.6，停播跳过这一版起才有）' % v)
ok('OFFAIR_SIL_BLOCKS' in cfg and SIL_BLOCKS >= 160 and SIL_BLOCKS <= 800,
   '静音判据 %d 块 ≈ %d 秒（够长，不会被广告间隙误判）' % (SIL_BLOCKS, SIL_BLOCKS // 40))
ok(SAVE_MIN >= 1 and SAVE_MIN <= 10, '倒计时每 %d 分钟刷一次盘（断电不越藏越久）' % SAVE_MIN)

print('\n通过 %d / 失败 %d' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
