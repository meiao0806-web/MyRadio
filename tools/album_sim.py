# -*- coding: utf-8 -*-
"""album.cpp + epis.cpp 的 Python 复刻 + 断言：用真台表 / 真集表验这两层逻辑。
   跑法：python tools/album_sim.py

   1.9.0 起：一部书只占**一个台**（台名＝书名），集在 src/preset_episodes.h 里。
   所以 album 层退化成「每台自成一个单元」，真正的连播/续播逻辑在 epis 层。
   下面两层的判据都是逐行照抄 C++，不引入新规则。
"""
import re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, '..', 'src', 'preset_stations.h')
EPI  = os.path.join(HERE, '..', 'src', 'preset_episodes.h')

t = open(SRC, encoding='utf-8', errors='ignore').read()

rows = re.findall(r'\{ "([^"]*)", "([^"]*)" \}', t)

# 1.15.0：kPresetGrp（组名 / 起始下标 / 台数）已经不存在了。
# 换成 kPresetTag（标签名 / 台数）+ kPresetTagOf（每台一个标签下标）。
# 播放顺序**只由 kPreset 的先后决定**，标签不参与排序，也不要求同标签连续。
def _arr(decl):
    m = re.search(decl + r'\[\] = \{(.*?)\n\};', t, re.S)
    if not m:
        raise SystemExit('preset_stations.h 里找不到 %s[]' % decl)
    return m.group(1)

tags   = [(m[0], int(m[1])) for m in
          re.findall(r'\{ "([^"]+)", (\d+) \}', _arr('static const PresetTag kPresetTag'))]
tag_of = [int(x) for x in re.findall(r'\b(\d+)\b', _arr('static const uint8_t kPresetTagOf'))]

ALL = [{'id': i, 'name': n} for i, n in rows]

# ---------------------------------------------------------------- album 层
def album_split(name):
    """album.cpp: albumSplit 的逐行复刻。返回专辑名或 None。"""
    b = name.encode('utf-8')
    d = 0
    while d < 4 and (len(b) - 1 - d) >= 0 and 0x30 <= b[len(b) - 1 - d] <= 0x39:
        d += 1
    if d < 2:
        return None
    sp = len(b) - d - 1
    if sp < 2 or b[sp:sp + 1] != b' ':
        return None
    if sp > 39:
        sp = 39
    return b[:sp].decode('utf-8', 'ignore')


PROG = {}   # 专辑名 -> 组内索引


def unit_start(lst, i):
    if not (0 <= i < len(lst)):
        return -1
    a = album_split(lst[i]['name'])
    if a is None:
        return i
    s = i
    while s > 0 and album_split(lst[s - 1]['name']) == a:
        s -= 1
    return s


def unit_end(lst, i):
    if not (0 <= i < len(lst)):
        return -1
    a = album_split(lst[i]['name'])
    if a is None:
        return i
    e = i
    while e + 1 < len(lst) and album_split(lst[e + 1]['name']) == a:
        e += 1
    return e


def album_step(lst, i, d):
    n = len(lst)
    if n == 0:
        return -1
    if not (0 <= i < n):
        i = 0
    s0, e0 = unit_start(lst, i), unit_end(lst, i)
    j = (e0 + 1) % n if d >= 0 else (s0 - 1) % n
    return unit_start(lst, j)


def album_entry(lst, i):
    s0 = unit_start(lst, i)
    if s0 < 0:
        return -1
    a = album_split(lst[s0]['name'])
    if a is None:
        return s0
    rec = PROG.get(a)
    if rec is not None and s0 <= rec < len(lst) and album_split(lst[rec]['name']) == a:
        return rec
    return s0


def step_unit(lst, base, steps):
    n = len(lst)
    if n == 0:
        return -1
    if not (0 <= base < n):
        base = 0
    steps = max(-n, min(n, steps))
    i = base
    while steps > 0:
        i = album_step(lst, i, +1); steps -= 1
    while steps < 0:
        i = album_step(lst, i, -1); steps += 1
    return i


# ---------------------------------------------------------------- epis 层
e = open(EPI, encoding='utf-8', errors='ignore').read()
books = [(m[0], int(m[1]), int(m[2])) for m in
         re.findall(r'\{ "([^"]+)", (\d+), (\d+) \}',
                    e.split('static const EpisBook kEpisBook[]')[1].split('};')[0])]
items = re.findall(r'\{ "([^"]*)", "([^"]*)" \}',
                   e.split('static const EpisItem kEpis[]')[1])
EPIS_CUR = {}


def ep_count(b):
    return books[b][2] if 0 <= b < len(books) else 0


def ep_cur(b):
    n = ep_count(b)
    if n <= 0:
        return -1
    v = EPIS_CUR.get(b, 0)
    return 0 if (v < 0 or v >= n) else v        # epis.cpp: 越界夹到第 1 集


def ep_set_cur(b, v):
    n = ep_count(b)
    if n <= 0:
        return
    EPIS_CUR[b] = max(0, min(n - 1, v))          # epis.cpp: clamp


def ep_step(b, d):
    n = ep_count(b)
    if n <= 1:
        return 0
    return (ep_cur(b) + (1 if d >= 0 else -1)) % n   # 两端循环


def ep_title(b, i):
    if not (0 <= b < len(books)):
        return ''
    f, c = books[b][1], books[b][2]
    if not (0 <= i < c) or f + i >= len(items):
        return ''
    return items[f + i][0]


def ep_url(b, i):
    f, c = books[b][1], books[b][2]
    if not (0 <= i < c) or f + i >= len(items):
        return ''
    return items[f + i][1]


def book_of_name(nm):
    for b, (bn, _f, _c) in enumerate(books):
        if nm == bn or (nm and nm.startswith(bn) and len(nm) > len(bn) and nm[len(bn)] == ' '):
            return b
    return -1


# ---------------------------------------------------------------- 断言
fails = []


def ck(cond, msg):
    if not cond:
        fails.append(msg)


# 1. albumSplit 用例（含误判防护）—— 函数还在，判据不能变
cases = [
    ('三国演义 001', '三国演义'),
    ('上下五千年 015', '上下五千年'),
    ('每晚一个历史故事 003', '每晚一个历史故事'),
    ('红楼梦 007', '红楼梦'),
    ('西安新闻广播', None),
    ('FM998 舟山新闻综合广播', None),
    ('FM92.6 综合广播', None),
    ('中江综合广播994', None),          # 有数字但前面不是空格
    ('彬州之声', None),
    ('台 12', '台'),                    # 2 字节专辑名刚好够
    ('', None),
    ('990', None),                      # 纯数字
]
for name, want in cases:
    got = album_split(name)
    ck(got == want, 'albumSplit(%r) -> %r，期望 %r' % (name, got, want))

# 2. 台表结构：1.15.0 起**不再分组** —— 整张表就是一个列表，先后即播放顺序
ck(len(ALL) > 250, '台表应大于 250 台，实得 %d' % len(ALL))
ck(len(tags) >= 5, '至少该有 5 个标签，实得 %d' % len(tags))
ck(len(tag_of) == len(ALL),
   'kPresetTagOf 有 %d 项，kPreset 有 %d 台 —— 两边必须一一对应' % (len(tag_of), len(ALL)))
ck(all(0 <= x < len(tags) for x in tag_of),
   '标签下标越界：min=%d max=%d，标签只有 %d 个' % (min(tag_of), max(tag_of), len(tags)))
# 标签里报的台数必须与 tag_of 实际出现次数一致（否则 App 的 chips 会报假数）
for ti, (tn, tc) in enumerate(tags):
    real = tag_of.count(ti)
    ck(tc == real, '标签「%s」报 %d 台，实际 %d 台' % (tn, tc, real))
ck(sum(c for _, c in tags) == len(ALL),
   '各标签台数之和 %d ≠ 台表 %d（说明有台没打标签）' % (sum(c for _, c in tags), len(ALL)))
# 只校验几个「必须存在且个位数量级」的标签，具体台数交给上面的自洽校验
# 2026-11-01：电台全部并进「广播」，其余标签只放有声书。
# 「新闻」里 97 个"XX新闻广播"都挪进广播后，它既没台也没书，生成时被跳过。
ck(tags[0][0] == '广播', '第一个标签必须是「广播」（它是删标签的退回目标），实得 %s' % tags[0][0])
ck(not any(x[0] == '新闻' for x in tags), '「新闻」应已并入「广播」，不该再存在')
for g in ('军事历史', '历史', '评书', '广播剧', '悬疑', '都市'):
    ck(any(x[0] == g for x in tags), '缺少标签「%s」' % g)

# 3. 1.14.0：中央台九个必须顶在**整张表最前面**，开机默认台就是「中国之声」
CNR9 = ['中国之声', '经济之声', '文艺之声', '神州之声', '台海之声',
        '中国乡村之声', '环球资讯广播', '音乐之声', '中国交通广播']
head9 = [x['name'] for x in ALL[:len(CNR9)]]
ck(head9 == CNR9, '表头应是中央台九个，实得 %s' % '、'.join(head9))
for n in CNR9:
    hit = [x for x in ALL if x['name'] == n]
    ck(len(hit) == 1, '中央台「%s」应恰好 1 个，实得 %d' % (n, len(hit)))
    ck(hit[0]['id'].startswith('http'), '中央台「%s」的地址应是 http 直链，实得 %r'
       % (n, hit[0]['id']))
# 非汉语台（蒙语/朝鲜语/哈萨克语/维吾尔语）应已删干净
BAD = ('蒙语', '蒙古语', '朝鲜语', '哈萨克语', '维吾尔语')
left = [x['name'] for x in ALL if any(k in x['name'] for k in BAD)]
ck(not left, '还留着 %d 个非汉语台：%s' % (len(left), '、'.join(left)))

# 3. 1.9.0：没有任何台名再带「 001」后缀 → album 层退化成每台一个单元
suffixed = [x['name'] for x in ALL if album_split(x['name'])]
ck(not suffixed, '台表里不该再有「书名 001」这种台名，发现 %d 个：%s'
   % (len(suffixed), '、'.join(suffixed[:5])))

# 4. 1.15.0：有声书**均匀穿插**在电台之间，不堆在末尾
#    （判据：任意相邻两部的间隔都差不多；不再写死 10~20 ——
#      书从 18 部涨到 41 部、台表 257 台时，平均间隔本来就是 6 台，
#      写死区间会在「只是多加了书」时报假 FAIL。2026-11-04 踩到）
book_idx = [i for i, x in enumerate(ALL) if book_of_name(x['name']) >= 0]
ck(len(book_idx) == len(books),
   '表里应有 %d 部书，实得 %d 个' % (len(books), len(book_idx)))
gaps = [book_idx[k] - book_idx[k - 1] for k in range(1, len(book_idx))]
ck(min(gaps) >= 2 and max(gaps) - min(gaps) <= 2,
   '书之间应均匀（间隔差 ≤2 且 ≥2）：平均 %.1f，实得 %d~%d（说明穿插不均）'
   % (sum(gaps) / float(len(gaps)), min(gaps), max(gaps)))
ck(book_idx[0] > 5, '第一部书不该顶在开头（前面应先是中央台），实得第 %d 台' % (book_idx[0] + 1))
ck(book_idx[-1] < len(ALL) - 5,
   '最后一部书不该落在末尾，实得第 %d 台 / 共 %d 台' % (book_idx[-1] + 1, len(ALL)))
print('穿插：%d 部书，间隔 %d~%d 台，首 %d / 末 %d（共 %d 台）'
      % (len(book_idx), min(gaps), max(gaps), book_idx[0] + 1, book_idx[-1] + 1, len(ALL)))

# 5. 转一格 = 换一台（一部书也算一台），整表绕一圈回到原点、步数 = 台数
i, n_step = 0, 0
while True:
    i = album_step(ALL, i, +1)
    n_step += 1
    if i == 0 or n_step > len(ALL) + 5:
        break
ck(i == 0 and n_step == len(ALL),
   '整表绕圈异常：回到 %d，走了 %d 步（应 %d 步）' % (i, n_step, len(ALL)))
# 表里每一台都要被走到（不再按组切片，一次验完）
starts, j, seen = [], 0, set()
while j not in seen:
    seen.add(j)
    starts.append(j)
    j = album_step(ALL, j, +1)
ck(len(starts) == len(ALL),
   '整表节目数 %d ≠ 台数 %d（旋钮应逐台走）' % (len(starts), len(ALL)))
print('整表一圈 %d 步 = %d 台' % (n_step, len(ALL)))

# 5b. 书的前后都是普通台（书与书不相邻），且跨过书只算一格
for k in book_idx:
    if k + 1 < len(ALL):
        ck(book_of_name(ALL[k + 1]['name']) < 0,
           '第 %d 台（书）后面紧跟着另一部书，说明穿插没生效' % (k + 1))
    ck(album_step(ALL, k, +1) == k + 1, '从第 %d 台（书）转一格应到 %d' % (k + 1, k + 2))
    ck(album_step(ALL, k, -1) == k - 1, '从第 %d 台（书）反转一格应到 %d' % (k + 1, k))

PROG.clear()
ck(album_entry(ALL, 0) == 0, '无记忆时单元入口就是自己，实得 %d' % album_entry(ALL, 0))

# 6. 边界：单台表 / 空表不崩
ck(album_step([{'id': '1', 'name': '唯一台'}], 0, +1) == 0, '单台表 +1 应回到自己')
ck(album_step([{'id': '1', 'name': '三国演义 001'}], 0, +1) == 0, '单集专辑 +1 应回到自己')
ck(album_step([], 0, +1) == -1, '空表应返回 -1')
ck(step_unit(ALL, 0, 999) >= 0, '超大步数不应崩')

# 7. 全表：每台都能走通、不越界
for i in range(len(ALL)):
    j = album_step(ALL, i, +1)
    ck(0 <= j < len(ALL), '第 %d 台 +1 越界 -> %d' % (i + 1, j))
    k = album_step(ALL, i, -1)
    ck(0 <= k < len(ALL), '第 %d 台 -1 越界 -> %d' % (i + 1, k))

# 8. 集表：每部书的区间自洽（**不写死集数**）
ck(len(books) >= 5, '至少该有 5 部书，实得 %d' % len(books))
ck(len(items) >= 75, '集表至少该有 75 集，实得 %d' % len(items))
acc = 0
for bn, f, c in books:
    ck(f == acc, '%s 起始下标应为 %d，实得 %d' % (bn, acc, f))
    acc += c
    print('书 %-12s %4d 集  第 1 集: %s' % (bn, c, ep_title(books.index((bn, f, c)), 0) or '（无标题）'))
ck(acc == len(items), '各部书集数之和 %d ≠ 集表 %d' % (acc, len(items)))

# 9. 每部书在台表里恰好一个台，且台名与书名逐字相同
for bn, f, c in books:
    hit = [x for x in ALL if x['name'] == bn]
    ck(len(hit) == 1, '「%s」在台表里应恰好 1 个台，实得 %d' % (bn, len(hit)))
    ck(book_of_name(bn) >= 0, 'bookOfName(%r) 应认得出' % bn)

# 10. 普通台不会被当成书
for nm in ('西安新闻广播', 'FM998 舟山新闻综合广播', '中江综合广播994', '经典FM1008'):
    ck(book_of_name(nm) == -1, '%r 不该被当成有声书' % nm)

# 11. epis 层：上下 / 循环 / 越界夹回
#   1.46.2：这里原来硬编码「三国演义 = 第 2 部、15 集」，集表一改就假 FAIL
#   （实测 1.46.2 里第 2 部是上下五千年 235 集）。改成按名字取书、按实际集数断言。
B = book_of_name('三国演义')
ck(B >= 0, '应能找到「三国演义」这部书，实得 %d' % B)
NB = ep_count(B)
ck(NB >= 2, '三国演义应有至少 2 集，实得 %d' % NB)
ck(ep_count(-1) == 0 and ep_count(99) == 0, '不存在的书应 0 集')
ep_set_cur(B, 0)
ck(ep_step(B, +1) == 1, '第 1 集 next 应到第 2 集，实得 %d' % ep_step(B, +1))
ck(ep_step(B, -1) == NB - 1, '第 1 集 prev 应绕到第 %d 集，实得 %d' % (NB, ep_step(B, -1)))
ep_set_cur(B, NB - 1)
ck(ep_step(B, +1) == 0, '末集 next 应回到第 1 集（播完从头再来），实得 %d' % ep_step(B, +1))
ep_set_cur(B, NB + 84)
ck(ep_cur(B) == NB - 1, '越界集号应夹到末集 %d，实得 %d' % (NB - 1, ep_cur(B)))
ep_set_cur(B, -5)
ck(ep_cur(B) == 0, '负数集号应夹到第 1 集，实得 %d' % ep_cur(B))
EPIS_CUR.clear()
ck(ep_cur(B) == 0, '没记录时应从第 1 集开始，实得 %d' % ep_cur(B))

# 12. 每部书各记各的
EPIS_CUR.clear()
ep_set_cur(2, 6)                             # 三国 第 7 集
ep_set_cur(3, 2)                             # 官场 第 3 集
ck(ep_cur(2) == 6 and ep_cur(3) == 2, '两部书的进度串了：三国 %d / 官场 %d' % (ep_cur(2), ep_cur(3)))
EPIS_CUR.clear()

# 13. 集地址 / 标题都取得到，且不串书
seen = set()
for b in range(len(books)):
    for i in range(ep_count(b)):
        u = ep_url(b, i)
        ck(u.startswith('http'), '%s 第 %d 集地址异常：%r' % (books[b][0], i + 1, u[:30]))
        ck(u not in seen, '%s 第 %d 集地址与前面重复' % (books[b][0], i + 1))
        seen.add(u)
ck(len(seen) == len(items), '集地址应各不相同：唯一 %d 个 ≠ 集表 %d 条' % (len(seen), len(items)))
_bk = book_of_name('上下五千年')
ck(_bk >= 0 and ep_title(_bk, 0) == '开天辟地的神话',
   '上下五千年 第 1 集标题应为「开天辟地的神话」，实得 %r' % (ep_title(_bk, 0) if _bk >= 0 else '找不到这本书'))
_bk2 = book_of_name('三国演义')
ck(_bk2 >= 0 and '宴桃园' in ep_title(_bk2, 0),
   '三国演义 第 1 集标题应含「宴桃园」（1.46.55 起换成喜马 22 集带标题版），实得 %r'
   % (ep_title(_bk2, 0) if _bk2 >= 0 else '找不到这本书'))

print()
if fails:
    print('失败 %d 条：' % len(fails))
    for f in fails:
        print('  ✗', f)
    sys.exit(1)
print('全部通过 ✓')
