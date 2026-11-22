# -*- coding: utf-8 -*-
"""store.cpp 的 Python 复刻 + 断言 —— 用**真出厂台表**跑。

    跑法：python tools/store_sim.py

1.15.0 起台表模型整个换了，这个脚本也跟着重写：
  · **不再分组**：文件顺序就是播放顺序，旋钮从第 0 台走到最后一台
  · 第一栏降级成**标签**（只给 App 筛选），不要求同标签连续
  · 整表读进内存（MAX_STATIONS=500），所以「增删 / 排序 / 改标签」全都只动内存，
    最后整份重写一次（先写 .tmp 再改名）
  · 标签单独存 /tags.txt（顺序 = App 里 chips 的顺序，且允许暂时没有台）

下面每个函数都是照 src/store.cpp 逐行翻译的，不引入新规则。
重点是那些「一按就丢数据」的路径：删标签、删台、改标签、表版本升级。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src', 'preset_stations.h')

# ---- 与 config.h 保持一致 ----
MAX_STATIONS = 500
MAX_GROUPS = 48
GROUP_NAME_LEN = 16

FAILS = []


def check(cond, msg, extra=None):
    if cond:
        print("  ✓ " + msg + ("" if extra is None else "   " + str(extra)))
    else:
        print("  ✗ " + msg + ("" if extra is None else "   " + str(extra)))
        FAILS.append(msg)


# ============================================================
#  store.cpp 的复刻
# ============================================================
def is_qtfm_id(u):
    """对应 isQtfmId()：非空、全是数字、不超过 9 位"""
    if not u:
        return False
    return all('0' <= c <= '9' for c in u) and len(u) <= 9


def clip(s, n):
    """strncpy + 结尾 0"""
    return s[:n - 1]


class Store:
    def __init__(self):
        self.st = []          # [{'url':..., 'name':...}]
        self.tagOf = []       # 每台的标签下标
        self.tags = []        # 标签名（顺序 = chips 顺序）
        self.cur = -1
        self.saves = 0

    # ---------- 标签 ----------
    def find_tag(self, n):
        for i, t in enumerate(self.tags):
            if t == n:
                return i
        return -1

    def ensure_tag(self, n):
        """有就返回下标，没有就追加；标签太多 / 名字非法 → -1"""
        if not n:
            return -1
        i = self.find_tag(n)
        if i >= 0:
            return i
        if len(self.tags) >= MAX_GROUPS:
            return -1
        self.tags.append(clip(n, GROUP_NAME_LEN))
        return len(self.tags) - 1

    def tag_count(self, t):
        if t < 0 or t >= len(self.tags):
            return 0
        return sum(1 for x in self.tagOf if x == t)

    def tag_name(self, i):
        if i < 0 or i >= len(self.st):
            return ""
        t = self.tagOf[i]
        return self.tags[t] if 0 <= t < len(self.tags) else ""

    # ---------- 载入整表（loadAll）----------
    def load(self, tsv, tags_txt=None):
        self.st, self.tagOf, self.tags, self.cur = [], [], [], -1
        # loadTags()
        if tags_txt:
            for ln in tags_txt.split('\n'):
                ln = ln.strip()
                if ln and len(ln) < GROUP_NAME_LEN:
                    self.ensure_tag(ln)

        for raw in tsv.split('\n'):
            line = raw.rstrip('\r\n ')
            if not line or line[0] == '#':
                continue
            parts = line.split('\t')
            if len(parts) < 3:
                continue                      # 少一栏 → 跳过（不崩）
            tag, url, name = parts[0], parts[1], parts[2]
            if not url:
                continue                      # 空地址 = 空标签的占位行
            if len(self.st) >= MAX_STATIONS:
                break
            url = clip(url, 320).rstrip('\r\n ')
            name = clip(name, 40).rstrip('\r\n ')
            if not url:
                continue
            ti = self.ensure_tag(tag)
            self.st.append({'url': url, 'name': name})
            self.tagOf.append(ti if ti >= 0 else 0)

        self.cur = 0 if self.st else -1
        return len(self.st) > 0

    # ---------- 整份重写（storeSave）----------
    def dump(self):
        out = []
        for i, s in enumerate(self.st):
            t = self.tagOf[i]
            tag = self.tags[t] if 0 <= t < len(self.tags) else ""
            out.append("%s\t%s\t%s" % (tag, s['url'], s['name']))
        return "\n".join(out) + ("\n" if out else "")

    def dump_tags(self):
        return "".join(t + "\n" for t in self.tags)

    def save(self):
        self.saves += 1
        return self.dump(), self.dump_tags()

    # ---------- 台 ----------
    def add(self, name, url, tag):
        """对应 storeAdd()：tag<0 或越界 → 落到第 0 个标签"""
        if len(self.st) >= MAX_STATIONS:
            return -1
        if not url or (not is_qtfm_id(url) and not url.startswith('http')):
            return -1
        nm = clip(name if name else "未命名", 40)
        nm = nm.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')
        self.st.append({'url': clip(url, 320), 'name': nm})
        self.tagOf.append(tag if (0 <= tag < len(self.tags)) else 0)
        self.save()
        return len(self.st) - 1

    def delete(self, i):
        if i < 0 or i >= len(self.st):
            return False
        del self.st[i]
        del self.tagOf[i]
        if self.cur > i:
            self.cur -= 1
        elif self.cur == i and self.cur >= len(self.st):
            self.cur = len(self.st) - 1 if self.st else -1
        self.save()
        return True

    def move(self, i, d):
        """上下移一位"""
        j = i + d
        if i < 0 or i >= len(self.st) or j < 0 or j >= len(self.st):
            return False
        self.st[i], self.st[j] = self.st[j], self.st[i]
        self.tagOf[i], self.tagOf[j] = self.tagOf[j], self.tagOf[i]
        if self.cur == i:
            self.cur = j
        elif self.cur == j:
            self.cur = i
        self.save()
        return True

    def move_to(self, i, to):
        """拖到第 to 位（0 基），抽出再插入"""
        if i < 0 or i >= len(self.st):
            return False
        to = max(0, min(to, len(self.st) - 1))
        if to == i:
            return True
        s = self.st.pop(i)
        t = self.tagOf.pop(i)
        self.st.insert(to, s)
        self.tagOf.insert(to, t)
        if self.cur == i:
            self.cur = to
        elif i < self.cur <= to:
            self.cur -= 1
        elif to <= self.cur < i:
            self.cur += 1
        self.save()
        return True

    def edit(self, i, name, url):
        if i < 0 or i >= len(self.st):
            return False
        if url:
            if not is_qtfm_id(url) and not url.startswith('http'):
                return False
            self.st[i]['url'] = clip(url, 320)
        if name:
            self.st[i]['name'] = clip(name, 40).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')
        self.save()
        return True

    # ---------- 标签管理 ----------
    def move_to_tag(self, i, g_to, new_name=None):
        """对应 storeMoveToGroup()：1.15.0 起**只改标签、不动位置**"""
        if i < 0 or i >= len(self.st):
            return False
        t = g_to
        if g_to < 0:
            if not new_name:
                return False
            t = self.ensure_tag(new_name)
            if t < 0:
                return False
        if t < 0 or t >= len(self.tags):
            return False
        if self.tagOf[i] == t:
            return True
        self.tagOf[i] = t
        self.save()
        return True

    def group_add(self, name):
        if not name or len(name) >= GROUP_NAME_LEN:
            return False
        if self.find_tag(name) >= 0:
            return False
        if self.ensure_tag(name) < 0:
            return False
        self.save()
        return True

    def group_rename(self, g, name):
        if g < 0 or g >= len(self.tags) or not name or len(name) >= GROUP_NAME_LEN:
            return False
        if self.tags[g] == name:
            return False
        if self.find_tag(name) >= 0:
            return False
        self.tags[g] = name
        self.save()
        return True

    def group_del(self, g):
        """删标签**不删台**：这些台退回第 0 个标签。第 0 个标签不许删。"""
        if g <= 0 or g >= len(self.tags) or len(self.tags) <= 1:
            return False
        for i in range(len(self.tagOf)):
            if self.tagOf[i] == g:
                self.tagOf[i] = 0
            elif self.tagOf[i] > g:
                self.tagOf[i] -= 1
        del self.tags[g]
        self.save()
        return True

    def group_move(self, g, d):
        j = g + d
        if g < 0 or g >= len(self.tags) or j < 0 or j >= len(self.tags):
            return False
        self.tags[g], self.tags[j] = self.tags[j], self.tags[g]
        for i in range(len(self.tagOf)):
            if self.tagOf[i] == g:
                self.tagOf[i] = j
            elif self.tagOf[i] == j:
                self.tagOf[i] = g
        self.save()
        return True


# ============================================================
#  从真出厂表造一份 /stations.tsv（= storeSeedIfEmpty 写出来的样子）
# ============================================================
def load_preset():
    s = open(SRC, encoding='utf-8').read()

    def arr(decl):
        m = re.search(decl + r'\[\] = \{(.*?)\n\};', s, re.S)
        if not m:
            raise SystemExit('preset_stations.h 里找不到 %s[]' % decl)
        return m.group(1)

    st = arr('static const PresetSt kPreset')
    rows = [{'u': u.replace('\\"', '"'), 'n': n}
            for u, n in re.findall(r'\{\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', st)]
    tags = [n for n, _ in re.findall(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*\}',
                                     arr('static const PresetTag kPresetTag'))]
    tag_of = [int(x) for x in re.findall(r'\b(\d+)\b',
                                         arr('static const uint8_t kPresetTagOf'))]
    ver = re.search(r'#define\s+PRESET_VER\s+(\d+)', s)
    return rows, tags, tag_of, (int(ver.group(1)) if ver else 0)


def seed_tsv(rows, tags, tag_of):
    return "".join("%s\t%s\t%s\n" % (tags[tag_of[i]] if tag_of[i] < len(tags) else "",
                                     r['u'], r['n'])
                   for i, r in enumerate(rows))


def seed_tags(tags):
    return "".join(t + "\n" for t in tags)


def main():
    rows, tags, tag_of, ver = load_preset()
    TSV = seed_tsv(rows, tags, tag_of)
    TG = seed_tags(tags)

    print("[1] 出厂表整表读入（%d 台 / %d 个标签，台表版本 %d）" % (len(rows), len(tags), ver))
    s = Store()
    s.load(TSV, TG)
    check(len(s.st) == len(rows), "读到 %d 台（出厂 %d 台）" % (len(s.st), len(rows)))
    check(len(s.tags) == len(tags), "读到 %d 个标签" % len(s.tags))
    check(s.tags == tags, "标签顺序与 kPresetTag 一致", s.tags)
    check(s.cur == 0, "开机停在第一个台（中国之声）", s.st[0]['name'])
    check(s.st[0]['name'] == '中国之声', "第 1 台是中国之声", s.st[0]['name'])
    check(all(0 <= t < len(s.tags) for t in s.tagOf), "每个台的标签下标都在范围内")
    for ti, tn in enumerate(tags):
        real = sum(1 for x in s.tagOf if x == ti)
        check(real == s.tag_count(ti), "标签「%s」%d 台，与逐台统计一致" % (tn, real))
    # 顺序必须与头文件逐条一致（文件顺序 = 播放顺序，错一个位置就是转错台）
    check([x['name'] for x in s.st] == [r['n'] for r in rows], "整表顺序与 kPreset 逐条一致")
    check([x['url'] for x in s.st] == [r['u'] for r in rows], "地址与 kPreset 逐条一致")

    print("\n[2] 往返：写出去再读回来必须一模一样")
    tsv, tgs = s.save()
    s2 = Store()
    s2.load(tsv, tgs)
    check([x['name'] for x in s2.st] == [x['name'] for x in s.st], "台名顺序稳定")
    check([x['url'] for x in s2.st] == [x['url'] for x in s.st], "地址稳定")
    check(s2.tagOf == s.tagOf, "标签下标稳定")
    check(s2.tags == s.tags, "标签顺序稳定")
    check(s2.dump() == s.dump(), "第二次 dump 与第一次逐字节相同")

    print("\n[3] 脏行 / 老格式：不许崩，也不许把表读歪")
    dirty = ("# 这是注释行\n"
             "\n"
             "广播\t1001\t正常台\n"
             "广播\t\t空地址占位行\n"
             "缺栏的一行\n"
             "广播\t1002\t台名里有\t制表符\n"
             "广播\t1003\tCRLF 结尾台\r\n"
             "广播\t1004\t末行没换行")
    d = Store()
    d.load(dirty)
    check(len(d.st) == 4, "4 个有效台（注释 / 空行 / 占位行 / 缺栏都被跳过）", len(d.st))
    check(d.st[0]['name'] == '正常台', "第一台正常", d.st[0]['name'])
    check(d.st[1]['name'] == '台名里有', "多一栏时只取前三栏", repr(d.st[1]['name']))
    check(d.st[2]['name'] == 'CRLF 结尾台', "\\r 被 trim 掉", repr(d.st[2]['name']))
    check(d.st[3]['name'] == '末行没换行', "末行没换行也能读", repr(d.st[3]['name']))
    # 空地址占位行不会变成「空地址的台」—— 否则播放时会去连一个空 URL
    check(all(x['url'] for x in d.st), "没有空地址的台")

    print("\n[4] 新增台")
    s = Store(); s.load(TSV, TG)
    n0 = len(s.st)
    i = s.add("我家测试台", "123456", -1)
    check(i == n0, "返回新台下标 %d" % i)
    check(len(s.st) == n0 + 1, "台数 +1")
    check(s.tagOf[i] == 0, "tag<0 → 落到第 0 个标签（广播）", s.tags[s.tagOf[i]])
    i2 = s.add("评书测试", "654321", s.find_tag('评书'))
    check(s.tagOf[i2] == s.find_tag('评书'), "指定标签生效", s.tags[s.tagOf[i2]])
    check(s.add("坏地址", "ftp://x", 0) == -1, "非数字且非 http 的地址被拒")
    check(s.add("空地址", "", 0) == -1, "空地址被拒")
    check(s.add("", "111222", 0) >= 0 and s.st[-1]['name'] == "未命名", "空台名 → 「未命名」")
    check(s.add("带\t制表符", "111333", 0) >= 0 and '\t' not in s.st[-1]['name'],
          "台名里的制表符换成空格（否则会把 tsv 写坏）")
    # 写出去再读回来，新加的台必须还在、标签还对
    t2, g2 = s.save()
    s3 = Store(); s3.load(t2, g2)
    check(len(s3.st) == len(s.st), "往返后新加的台没丢（%d 台）" % len(s3.st))
    check(s3.tagOf == s.tagOf, "往返后标签下标一致")

    print("\n[5] 删台")
    s = Store(); s.load(TSV, TG)
    nm1 = s.st[1]['name']
    s.cur = 5
    check(s.delete(0) is True, "删第 1 台成功")
    check(len(s.st) == n0 - 1, "台数 -1")
    check(s.st[0]['name'] == nm1, "后面的台顶上来：%s" % s.st[0]['name'])
    check(s.cur == 4, "当前台跟着前移（5 → 4）", s.cur)
    check(s.delete(999) is False, "越界删除返回 False")
    # 删当前台且它在末尾 → cur 夹回最后一台
    s.cur = len(s.st) - 1
    s.delete(s.cur)
    check(s.cur == len(s.st) - 1, "删掉末尾的当前台，cur 夹回最后一台", s.cur)
    # 删光 → cur = -1，不崩
    while s.st:
        s.delete(0)
    check(s.cur == -1 and not s.st, "删光后 cur = -1、表为空")

    print("\n[6] 排序：只动位置，标签跟着台走")
    s = Store(); s.load(TSV, TG)
    a, b = s.st[3]['name'], s.st[4]['name']
    ta, tb = s.tagOf[3], s.tagOf[4]
    check(s.move(3, +1) is True, "下移一位")
    check(s.st[4]['name'] == a and s.st[3]['name'] == b, "两台交换：%s / %s" % (s.st[3]['name'], s.st[4]['name']))
    check(s.tagOf[3] == tb and s.tagOf[4] == ta, "标签跟着台一起换位置（没留在原地）")
    check(s.move(0, -1) is False, "第 1 台上移越界 → False")
    check(s.move(len(s.st) - 1, +1) is False, "最后一台下移越界 → False")

    s = Store(); s.load(TSV, TG)
    first = s.st[0]['name']
    t_first = s.tagOf[0]
    check(s.move_to(0, 10) is True, "拖第 1 台到第 11 位")
    check(s.st[10]['name'] == first, "它落在第 11 位")
    check(s.tagOf[10] == t_first, "标签跟着走")
    check(s.st[0]['name'] != first, "原位被后面的台补上")
    check(len(s.st) == n0, "台数不变（%d）" % len(s.st))
    check(s.move_to(0, 999) is True and s.st[-1]['name'] == s.st[-1]['name'], "拖到超大下标被夹到末尾")
    check(s.move_to(-1, 0) is False, "非法下标返回 False")

    print("\n[7] 改标签（mvgroup）：只改标签、不动位置")
    s = Store(); s.load(TSV, TG)
    nm5 = s.st[5]['name']
    tgt = s.find_tag('评书')
    n_before = s.tag_count(tgt)
    check(s.move_to_tag(5, tgt) is True, "第 6 台改成「评书」")
    check(s.st[5]['name'] == nm5, "位置没动（还在第 6 台）", s.st[5]['name'])
    check(s.tagOf[5] == tgt, "标签已改", s.tags[s.tagOf[5]])
    check(s.tag_count(tgt) == n_before + 1,
          "评书从 %d 台变 %d 台" % (n_before, n_before + 1), s.tag_count(tgt))
    check(s.move_to_tag(5, tgt) is True, "重复改成同一个标签是 no-op（不报错）")
    # 用新名字建标签
    n_tag = len(s.tags)
    check(s.move_to_tag(6, -1, "常听") is True, "用新名字「常听」建标签并移过去")
    check(len(s.tags) == n_tag + 1 and s.tags[-1] == '常听', "标签表多出「常听」")
    check(s.move_to_tag(6, -1, "") is False, "新名字为空 → False")
    check(s.move_to_tag(999, 0) is False, "越界下标 → False")

    print("\n[8] 标签管理")
    s = Store(); s.load(TSV, TG)
    check(s.group_add("夜里听") is True, "新建空标签")
    check(s.tags[-1] == '夜里听' and s.tag_count(len(s.tags) - 1) == 0, "空标签 0 台（也得留着显示）")
    check(s.group_add("夜里听") is False, "重名不再追加")
    check(s.group_add("广播") is False, "与已有标签重名 → False")
    check(s.group_add("x" * GROUP_NAME_LEN) is False, "名字超长 → False")

    # 改名：台只存下标，所以自动跟着走
    g = s.find_tag('评书')
    cnt_before = s.tag_count(g)
    idx_before = [i for i, t in enumerate(s.tagOf) if t == g]
    check(s.group_rename(g, "评书连播") is True, "「评书」改名成「评书连播」")
    check(s.tags[g] == '评书连播', "标签名已改")
    check(s.tag_count(g) == cnt_before, "台数没变（%d）" % s.tag_count(g))
    check([i for i, t in enumerate(s.tagOf) if t == g] == idx_before, "原来那几台还挂在它下面（下标没串）")
    check(s.tag_name(idx_before[0]) == '评书连播', "台读出来的标签名跟着变")
    check(s.group_rename(g, "广播") is False, "改成已有标签名 → False（不合并）")
    check(s.group_rename(g, "评书连播") is False, "改成同名 → False")

    # 删标签：不删台，退回第 0 个标签
    s = Store(); s.load(TSV, TG)
    g = s.find_tag('评书')
    keep = [i for i, t in enumerate(s.tagOf) if t == g]
    n_tags = len(s.tags)
    check(s.group_del(g) is True, "删「评书」标签")
    check(len(s.tags) == n_tags - 1, "标签数 -1（%d）" % len(s.tags))
    check(len(s.st) == n0, "**台数没变**（%d 台一个都没删）" % len(s.st))
    check(all(s.tagOf[i] == 0 for i in keep),
          "原来那 %d 台退回第 0 个标签「广播」" % len(keep))
    check(all(0 <= t < len(s.tags) for t in s.tagOf), "所有台的标签下标重新对上了（没越界）")
    # 后面的标签下标要整体前移，否则会指到别人身上
    for tn in ('广播剧', '悬疑', '都市'):
        check(s.find_tag(tn) >= 0, "「%s」还在，下标 %d" % (tn, s.find_tag(tn)))
    check(s.group_del(0) is False, "第 0 个标签不许删（它是退回目标）")
    check(s.group_del(999) is False, "越界删除 → False")

    # 排序：只动 chips 先后，不动台顺序
    s = Store(); s.load(TSV, TG)
    names_before = [x['name'] for x in s.st]
    t0, t1 = s.tags[0], s.tags[1]
    check(s.group_move(0, +1) is True, "第 1 个标签往下挪")
    check(s.tags[0] == t1 and s.tags[1] == t0, "两个标签换位：%s / %s" % (s.tags[0], s.tags[1]))
    check([x['name'] for x in s.st] == names_before, "**台顺序一台都没动**")
    check(all(0 <= t < len(s.tags) for t in s.tagOf), "标签下标跟着换（没越界）")
    check(s.group_move(0, -1) is False, "第一个再上移 → False")

    print("\n[9] 容量上限")
    s = Store(); s.load(TSV, TG)
    room = MAX_STATIONS - len(s.st)
    ok_cnt = 0
    for k in range(room):
        if s.add("填充%d" % k, str(100000 + k), 0) >= 0:
            ok_cnt += 1
    check(ok_cnt == room, "能一直加到 %d 台（加了 %d 台）" % (MAX_STATIONS, ok_cnt))
    check(s.add("再多一个", "999999", 0) == -1, "第 %d 台被拒（不越界写内存）" % (MAX_STATIONS + 1))
    # 标签上限
    s2 = Store(); s2.load(TSV, TG)
    made = 0
    for k in range(MAX_GROUPS + 10):
        if s2.group_add("T%d" % k):
            made += 1
    check(len(s2.tags) <= MAX_GROUPS, "标签数不超过 MAX_GROUPS（%d ≤ %d）" % (len(s2.tags), MAX_GROUPS))

    print("\n[10] 台表版本升级：变了要重灌，且先备份旧表")
    # 对应 storeSeedIfEmpty()：stamp != PRESET_VER 就重灌；重灌前把旧表拷到 /stations.bak
    def seed_if_empty(fs, preset_ver):
        have = int(fs.get('/stamp.txt', '-1').strip() or -1)
        if '/stations.tsv' in fs and have == preset_ver:
            return 'skip'
        if '/stations.tsv' in fs:
            fs['/stations.bak'] = fs['/stations.tsv']      # 备份
        fs['/tags.txt'] = TG
        fs['/stations.tsv'] = TSV
        fs['/stamp.txt'] = str(preset_ver)
        return 'reseed'

    fs = {}
    check(seed_if_empty(fs, ver) == 'reseed', "空机器：第一次开机灌入出厂表")
    check(fs['/stations.tsv'] == TSV, "写进去的就是出厂表")
    check(fs['/stamp.txt'] == str(ver), "版本号写入 %d" % ver)
    check(seed_if_empty(fs, ver) == 'skip', "同版本再开机：不重灌（用户的改动保住了）")
    fs['/stations.tsv'] = TSV + "广播\t777777\t我加的台\n"
    check(seed_if_empty(fs, ver + 1) == 'reseed', "固件升到 %d：重灌" % (ver + 1))
    check('/stations.bak' in fs and '我加的台' in fs['/stations.bak'],
          "重灌前把用户改过的表备份进 /stations.bak（没一句话不说就覆盖）")
    check(fs['/stations.tsv'] == TSV, "重灌后台表回到出厂那版")

    print("\n结果：" + ("全部通过 ✓" if not FAILS else "%d 项失败" % len(FAILS)))
    if FAILS:
        for f in FAILS:
            print("  ✗ " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
