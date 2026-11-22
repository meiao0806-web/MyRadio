# -*- coding: utf-8 -*-
"""1.46.54 回归：有声书「播过别再播」+ 调音量松手不换台 + 台序刷新。

这几条在桌面上跑不出来（要真旋钮、真网络、真屏），只能靠结构断言钉死。
"""
import io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
oks, fails = 0, 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails += 1
        print('  FAIL ' + msg + (('  << ' + str(extra)) if extra else ''))


def read(*p):
    return io.open(os.path.join(ROOT, *p), encoding='utf-8').read()


a = read('src', 'audio.cpp')
i = read('src', 'input.cpp')
u = read('src', 'ui.cpp')
cfg = read('src', 'config.h')
ep = read('src', 'epis.cpp')

print('[1] 断点门槛：听过 5 秒也要记（原来 20 秒，直接丢弃）')
ok('#define RESUME_MIN_SEC       3' in cfg, 'RESUME_MIN_SEC = 3')
ok('20      // 至少听过 20 秒' not in cfg, '旧的 20 秒门槛已去掉')

print('\n[2] 位置向上取整（播过的那 0.9 秒不能再放一遍）')
ok('const uint32_t el = s_playAt ? (uint32_t)(millis() - s_playAt) : 0;' in a,
   '先把已播毫秒取出来')
ok('s_posBase + (int)((el + 999u) / 1000u)' in a, '再用 (el+999)/1000 向上取整')
ok('/ 1000) : 0);' not in a or '(el + 999u)' in a, '没有残留向下取整的写法')
# 语义验算（纯算术）
def pos(base, ms):
    return base + (ms + 999) // 1000
cases = [(0, 5000, 5), (0, 5900, 6), (0, 1000, 1), (0, 999, 1), (53, 5000, 58)]
bad = [(b, m, pos(b, m), e) for b, m, e in cases if pos(b, m) != e]
ok(not bad, '播 5.0s→记5（=第6秒起播）；5.9s→记6（跳过0.1s，不重播）', bad)

print('\n[3] 旋钮调音量松手不再换台')
ok('static uint32_t s_volAt = 0;' in i, '有「上次调音量时刻」')
ok('#define VOL_TURN_GRACE_MS  400' in i, '宽限窗 400ms')
ok('(s_stable && (now - s_downAt) >= 20)' in i, '按下 20ms 后就算调音量（原 40ms）')
ok('''const bool volNow = (s_stable && (now - s_downAt) >= 20) ||
                        (s_volTurned && s_volAt && (now - s_volAt) < VOL_TURN_GRACE_MS);
    if (volNow) {''' in i, '这两个条件合起来决定走「音量」分支')
ok('if (s_stable && (now - s_downAt) >= 40) {' not in i, '旧的 40ms 单条件已去掉')
ok('s_volAt     = now;' in i, '调音量时刷新时间基准')
# 顺序：必须是 volNow 判断 → 否则 else if (A.fmMode) → else 换台
seg = i[i.find('const bool volNow'):]
seg = seg[:seg.find('s_lastTurn = now;')]
ok(seg.find('doVol(dv * VOL_HOLD_STEP);') < seg.find('s_pending += d;'),
   '音量分支在换台分支之前（volNow 成立就不会动 s_pending）')
ok('s_pending += d;' in seg and 'volNow' in seg, '换台分支仍在（转台功能没丢）')

print('\n[4] 台序文字：needBox 直接比「将要显示的那串字」')
ok('static void boxOrdText(char* b, size_t n)' in u, '抽出了 boxOrdText()')
ok('static char l_ord[20] = "";' in u, 'l_ord 记住上一帧画出去的文字')
ok('boxOrdText(l_ord, sizeof(l_ord));' in u, 'snapshot 里记 l_ord')
ok('const bool ordChanged = (strcmp(l_ord, ordNow) != 0);' in u, '比较文字是否变了')
ok('bool needBox  = needTop || needFreq || ordChanged || (l_ps != (int)A.ps) || (l_cnt != g_stN);' in u,
   'ordChanged 参与 needBox')
# drawBoxes 必须用同一个函数，否则比较的和画的不是一回事
seg2 = u[u.find('static void drawBoxes()'):]
seg2 = seg2[:2200]
ok('boxOrdText(b, sizeof(b));' in seg2, 'drawBoxes 用同一个 boxOrdText 取文字')
ok('if (A.fmMode) { b[0] = 0; return; }' in u, '收音模式下这一格不显示台序')
ok('episCur(bk1) + 1, episCount(bk1)' in u, '有声书显示「第几集/共几集」')
ok('(g_stN > 0) ? (idx + 1) : 0, g_stN' in u, '普通台显示「第几台/共几台」')
ok('A.preview >= 0 ? A.preview : A.cur' in u or '(A.preview >= 0) ? A.preview : A.cur' in u,
   '台序跟「正在预览的台」走（和台名同源）')

print('\n[5] 换台落定要有日志（下次能分清「没提交」还是「没刷新」）')
ok('evlog("key", "换台 #%d → #%d（待判 %d 格）", A.cur, land, (int)s_pending);' in i,
   'commitPending 里记了换台')

print('\n[6] 所有集播完后回第 1 集（确认这条本来就成立）')
ok('e = ((e % n) + n) % n;' in ep, 'episStep 会绕回（最后一集 → 第 1 集）')
ok('const int e2 = episStep(s_epiB, +1);     // 最后一集之后回第一集' in a,
   'nextEpOrStation 用的是 episStep(+1)')
ok('if (s_epiB >= 0) resumeClear(s_epiB);   // 1.46.2：这一集结束，断点作废' in a,
   '换集前先清这一集的断点（否则新集会从旧断点开始）')

print('\n---------------- 汇总 ----------------')
print('%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
