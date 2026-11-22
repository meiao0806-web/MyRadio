# -*- coding: utf-8 -*-
"""
1.22.0 离线验证：WiFi 存 3 组（去重 / 置顶 / 截断 / 轮换）

逐字复刻 net.cpp 的 saveWifiFile / forgetWifiSlot / 轮换下标推进。
这几条是「用户加了一个网络，结果老网络被顶掉」这类事故的高发区。
"""
import sys

MAX = 3
fails = oks = 0

def chk(name, got, want):
    global fails, oks
    if got == want:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-50s got=%r want=%r' % (name, got, want))

def save(cur, ssid, pw):
    """复刻 saveWifiFile"""
    out = [(ssid, pw)]
    for s, p in cur:
        if len(out) >= MAX:
            break
        if s == ssid:
            continue
        out.append((s, p))
    return out[:MAX]

def forget(cur, ssid):
    return [(s, p) for s, p in cur if s != ssid]

print('=== 保存 / 去重 / 置顶 ===')
a = save([], 'jia', 'p1')
chk('空表存第一个', [s for s, _ in a], ['jia'])

b = save(a, 'bama', 'p2')
chk('再存一个 → 新的置顶', [s for s, _ in b], ['bama', 'jia'])

c = save(b, 'jia', 'p1new')
chk('重存已有的 → 提到首位且不重复', [s for s, _ in c], ['jia', 'bama'])
chk('重存时密码用新给的', dict(c)['jia'], 'p1new')
chk('重存不会把数量变多', len(c), 2)

d = save(c, 'gongsi', 'p3')
chk('存第三个', [s for s, _ in d], ['gongsi', 'jia', 'bama'])

e = save(d, 'lvyou', 'p4')
chk('存第四个 → 截断到 3，最老的掉出去', [s for s, _ in e], ['lvyou', 'gongsi', 'jia'])
chk('第四个不会超上限', len(e), MAX)
chk('被挤掉的 bama 确实没了', 'bama' in dict(e), False)

print('\n=== 忘记某个网络 ===')
f = forget(d, 'jia')
chk('忘掉中间的', [s for s, _ in f], ['gongsi', 'bama'])
chk('忘掉数量减一', len(f), 2)
g = forget(f, 'gongsi')
chk('忘掉第一个', [s for s, _ in g], ['bama'])
h = forget(g, 'bama')
chk('全忘光', h, [])
chk('忘不存在的网络不炸', forget(h, 'xxx'), [])

print('\n=== 轮换下标 ===')
def rotate(idx, n):
    return (idx + 1) % n

chk('3 组：0→1', rotate(0, 3), 1)
chk('3 组：1→2', rotate(1, 3), 2)
chk('3 组：2→0（绕回）', rotate(2, 3), 0)
chk('2 组：1→0', rotate(1, 2), 0)
chk('1 组：不轮换（0→0）', rotate(0, 1), 0)

# 轮换 n 次一定回到起点（不会卡死在某一组）
for n in (2, 3):
    i = 0
    for _ in range(n):
        i = rotate(i, n)
    chk('轮换 %d 次回起点' % n, i, 0)

print('\n=== 停机保护：连上过就不再乱换 ===')
# 复刻 netLoop 的条件： s_slotN > 1 && !s_everUp && 超时
def should_rotate(slotN, everUp, elapsed, try_ms=18000):
    return slotN > 1 and (not everUp) and elapsed > try_ms

chk('没连上过 + 超时 → 轮换', should_rotate(3, False, 19000), True)
chk('已连上过 + 超时 → 不轮换', should_rotate(3, True, 19000), False)
chk('只有 1 组 → 不轮换', should_rotate(1, False, 19000), False)
chk('还没超时 → 不轮换', should_rotate(3, False, 10000), False)
chk('刚好 18000 不轮换（要 >）', should_rotate(3, False, 18000), False)

print('\n=== 文件格式兼容 ===')
def strip_eol(s):
    # 对应 C++ 的 stripEol：只去行尾的 \r，不动 tab 和空格
    while s and s[-1] in ('\r', '\n'):
        s = s[:-1]
    return s

def parse(text):
    out = []
    for ln in text.split('\n'):
        ln = strip_eol(ln)
        if not ln:
            continue
        if '\t' not in ln:
            continue
        s, p = ln.split('\t', 1)
        if s:
            out.append((s, p))
    return out[:MAX]

def dump(v):
    return ''.join('%s\t%s\n' % (s, p) for s, p in v)

# 老固件写的单行文件：必须能读，读出来就是第 1 组
old = 'jia\tp1\n'
chk('读老格式（单行）', parse(old), [('jia', 'p1')])

# 新固件写三行
new = dump([('a', '1'), ('b', '2'), ('c', '3')])
chk('写再读往返一致', parse(new), [('a', '1'), ('b', '2'), ('c', '3')])
chk('往返后行数正确', len(new.strip().split('\n')), 3)

# 密码里带 \t 或空密码
chk('空密码能读', parse('x\t\n'), [('x', '')])
chk('坏行（没 tab）被跳过', parse('badline\nok\tpw\n'), [('ok', 'pw')])
chk('空行被跳过', parse('\n\nok\tpw\n\n'), [('ok', 'pw')])

# 超过 3 行的手工文件也不会炸
chk('文件里有 5 行只取前 3', len(parse('a\t1\nb\t2\nc\t3\nd\t4\ne\t5\n')), 3)

print('\n================ 通过 %d / 失败 %d ================' % (oks, fails))
sys.exit(1 if fails else 0)
