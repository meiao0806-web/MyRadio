#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
台表-精选.md -> src/preset_stations.h（编译进固件的出厂台表）

用法：python tools/gen_preset_curated.py

与 gen_preset.py 的区别：
    gen_preset.py         抓全量 31 个地区 1038 个台（含音乐台、广告台）
    gen_preset_curated.py 只保留人工精选的台，并按「内容」重新分组

分组规则（2026-11-01 起）：
    台表-精选.md 里的台一律进「广播」组 ——
    不再按台名关键词拆到新闻/评书/历史/广播剧。
    其余组只放**有声书**，组名由 tools/有声书源*.json 里每部书的 group 字段决定。
    这样「广播」= 所有直播电台（268 台），「新闻」组因为没有书、没有台，生成时自动跳过。

    （旧规则：按台名关键词把"XX新闻广播"分到新闻组、"XX戏曲广播"分到评书组 ——
      1.13.0 及以前的做法，已废弃。用户要的是"收音机就该有一个电台组"。）

1.14.0 起「广播」组的最前面是**中央台**（总台/央广九个，公开 HLS 直链）：
    广播组顺序 = 中央台 → 陕西 → 四川 → 重庆 → 兵团 → 其余 → 苏州
    开机默认停在第一个台，也就是「中国之声」—— 老人开机就有声音，不用转旋钮。
    中央台那 9 行的「台号」栏填的是完整 http 地址，不是蜻蜓台号（parse_md 两种都认）。

1.8.0 起「陕西」不再单独成组，而是并进「广播」组的最前面：
    （四川/重庆/兵团 只取本来就落在广播组的台。）

苏州本地的台一律放到「广播」组的最后（8 个），这样转旋钮不会一进组就是家门口那几个。

1.9.0 起有声书**不再一集一个台**：一部书在台表里只占**一个台**（台名就是书名），
各集的 URL 单独写进 src/preset_episodes.h。旋钮一格 = 一部书，播完一集固件自己接下一集，
最后一集播完回到第一集；再转到这部书时，从上次听的那一集接着放。

1.10.0 起分组多了三类，且「军事历史」紧跟在「广播」后面（老人点一下旋钮就到）：
    广播 → 军事历史 → 历史 → 新闻 → 评书 → 广播剧 → 悬疑 → 都市
军事历史 = 二战战争史 / 抗战韩疯子；悬疑 = 鬼吹灯 / 血脉觉醒 / 第九特区；
都市 = 首席医官 / 一路高升 / 为民无悔 / 人间悲喜。

1.10.0 的 14 部听书网书**不是只取前 15 集**，而是把「全部章节」页翻完（每页 60 条，
页签写着 1~60 / 61~120 …），所以每部书的集数与站点一致（少的几十集，多的八百多集）。
代价是集表变大，固件里每集一条 URL（约 130 字节），整份集表几百 KB 级。
"""
import json
import os
import re
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "台表-精选.md")
OUT = os.path.join(ROOT, "src", "preset_stations.h")
EPIS_OUT = os.path.join(ROOT, "src", "preset_episodes.h")

MAX_NAME = 39                      # Station::name[40]，留 1 字节 0

# 「求订阅，求评论，求转发」这类拉票话术（见 ep_title 末尾的说明）
_JUNK_TITLE = re.compile(
    r"(?:[求请多来]?(?:订阅|关注|评论|转发|点赞|收藏|分享|打赏|月票|推荐|五星)"
    r"[\s，,、。！!~～.．·]*)+")

# 1.46.32：括号里是「分集序号」时不能当注解剥掉，否则同一回的上/中/下三集标题全一样。
#   只认 上/中/下/尾（可带数字）+ 单个中文数字，别的一律照旧剥。
_PART_SUFFIX = re.compile(
    r"[（(]\s*(?:上|中|下|尾|[上中下][0-9]{1,2}|[一二三四五六七八九十]{1,2})\s*[）)]\s*$")
PRESET_VER = 15                     # 台表版本；升固件时变了就自动重灌（store.cpp 比对）
                                   # 2026-11-04 → 15：删海南民生广播；加 8 部节目
                                   #   （喜马《光荣啊！铁道兵》144 / 蜻蜓《俗世奇人》75 /
                                   #    云听 6 个栏目 424 集）；并把用户手动加的 3 个台
                                   #   （安徽小说评书/阿基米德故事会/雨声电台）写进出厂表
                                   # 2026-11-04 → 14：《三国演义》从老 RSS 单播评书（145 集）
                                   #   换成喜马「百名CV大型多人精品历史剧」（album 53402173，
                                   #   付费墙前 22 集），老的那本用 DROP_SRC 挡掉
                                   # 1.10.0 → 4：灌进听书网 14 部书（全量集数），台表结构变了
                                   # 2026-11-01 → 5：所有电台并进「广播」组（8 组变 7 组）
                                   # 2026-11-01 → 6：**不再分组**，书穿插进电台，只留一条播放顺序；
                                   #   头文件结构也换了（kPresetGrp → kPresetTag + kPresetTagOf），
                                   #   必须变号，否则老机器 OTA 后还停在旧的 7 组表
                                   # 2026-11-01 → 7：加蜻蜓 3 部（原来是这样？！/白眉大侠外传/
                                   #   如临大敌）+ 喜马《战神出狱》，多一个「科普」标签
                                   # 2026-11-01 → 13：加用户点名的 13 部喜马书，且书改成
                                   #   「第 11 台之后」才开始穿插（前 11 台原样不动）
                                   # 2026-11-01 → 8：加原厂固件里的央视 17 个频道
                                   #   （央视单列一个标签，播放位置紧跟中央台九个之后）

HEAD_SECTIONS = ["中央台"]         # 广播组**最前**（总台/央广九个，排在陕西之前）
CCTV_SECTION = "央视频道"           # 这一节单独一个标签，播放位置紧跟中央台之后
CCTV_TAG = "央视"
SX = "陕西"                        # 这一节整组并进广播组（排在中央台后面）
FRONT_SECTIONS = ["四川", "重庆"]   # 广播组内提到前面的地区（按 md 小节认）
FRONT_KEYWORD = "兵团"             # 台名带「兵团」的也提到前面
BACK_SECTION = "苏州本地"           # 广播组里这些台挪到最后

# 1.46.31（用户要求）：「书从 11 台以后开始插入，前面的不动」——
#   前 BOOK_START_IDX 个台（0 ~ BOOK_START_IDX-1）原样保留，书只撒在这一段之后。
#   原来是「中央台 9 + 央视 12」固定不动、书从第 22 位才开始，
#   用户看到的是"前面二十几个台全是电台，一本都碰不到"。
#   ⚠️ 11 会切在「CCTV2 财经」和「CCTV3 综艺」之间，央视 12 个频道被书分开。
#      想让央视连成一片，把这个数改成 22（= 中央台 9 + 央视 12 + 1）即可。
BOOK_START_IDX = 11

# 已在蜻蜓下架的台（2026-10-31 用 Range 请求逐个验过，返回 404）。
# 留着的话旋钮转过去会卡住重连 5 次再跳台，体验很差，所以生成时剔掉。
# 以后官方恢复了，从这张表里删掉对应台号即可。
DEAD = {
    "5022397",   # FM100.7咸阳人民广播电台
    "5021543",   # 呼和浩特综合广播
    "5022648",   # 赣北之声FM94.2
    "1282",      # 江门人民广播电台综合广播
    "5022142",   # 济源新闻综合广播
    "5022622",   # 甘肃新闻综合广播
    "20211585",  # 901综合文艺广播
}

# 1.46.20：用户点名不要的书。直接从抓取数据里删会让「重跑抓取脚本」又长回来，
#   所以在生成这一层挡掉 —— 想加回来，把书名从这张表里删掉即可。
DROP_BOOKS = {
    "每晚一个历史故事",
}

# 1.46.32：「只挡某个源里的某一本」，同名书仍允许从别的源进来。
#   《三国演义》老的是 有声书源.json 里的 RSS 单播评书（小熊有声，145 集），
#   用户要换成喜马「百名CV大型多人精品历史剧」（album 53402173）。
#   新专辑落在 有声书源-喜马.json，所以**不能**整本写进 DROP_BOOKS —— 那会把新的也挡掉。
#   ⚠️ 同名去重是「留集数多的」：新专辑只有 22 集可播（其余付费墙）< 老 145 集，
#   不显式挡老的就一定被老的顶掉，所以这里必须挡。
DROP_SRC = {
    ("有声书源.json", "三国演义"),
}

OTHER = "广播"
# 分组顺序。只有「广播」收电台，其余组收有声书（组名来自 有声书源*.json 的 group 字段）。
# 「新闻」留在列表里只是保持顺序稳定：它既没有台也没有书，生成时会被跳过。
ORDER = ["广播", "军事历史", "历史", "科普", "新闻", "评书", "广播剧", "悬疑", "都市"]


def fit(s, limit=MAX_NAME):
    """按 UTF-8 边界截断，不留半个汉字"""
    b = s.encode("utf-8")
    if len(b) <= limit:
        return s
    cut = limit
    while cut > 0 and (b[cut] & 0xC0) == 0x80:
        cut -= 1
    return b[:cut].decode("utf-8", "ignore")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def interleave(radios, books):
    """把书均匀穿插进电台，返回合并后的列表。

    1.15.0 用户要求「将其他分组里的台穿插着和广播组合并」——
    不是「电台一大段 + 书一大段」，而是**均匀撒开**，否则 18 部书全落在
    270 位之后，老人转旋钮根本够不着。

    插在第 j 本书之前（j 从 0 起）用比例算，不写死间隔：
        pos_j = round((j+1) * n / (m+1))       # 插在「第 pos_j 个电台之后」
    268 台 / 18 本 → 每约 15 台一本，第一本在第 14 位之后。
    """
    n, m = len(radios), len(books)
    if m == 0:
        return list(radios)
    if n == 0:
        return list(books)

    pos = []
    for j in range(m):
        p = int(round((j + 1) * n / (m + 1.0)))
        p = max(1, min(n, p))
        if pos and p <= pos[-1]:        # 书比台还多时保序、不重叠
            p = pos[-1] + 1
        pos.append(p)

    out, bi = [], 0
    for i, r in enumerate(radios):
        out.append(r)
        if bi < m and pos[bi] == i + 1:
            out.append(books[bi])
            bi += 1
    out.extend(books[bi:])              # 兜底：还有没插完的（正常不会发生）
    return out


def shrink(name):
    """把超长台名压到 MAX_NAME 以内，尽量保住可读性。

    顺序：去 FM/AM 频率前缀 → 压缩冗长后缀 → 去掉括号注解 → 最后才硬截。
    只对超长的动手，短名字原样保留。
    """
    n = name.strip()
    if len(n.encode("utf-8")) <= MAX_NAME:
        return n

    # ① 去掉开头的「FM96.0」「AM1161」这类频率前缀（老人在屏上认的是台名，不是频率）
    n = re.sub(r"^(?:FM|AM)\s*[\d.]+\s*", "", n, flags=re.I)
    n = re.sub(r"\s+", "", n)
    if len(n.encode("utf-8")) <= MAX_NAME:
        return n

    # ② 压缩常见冗长后缀
    for a, b in (("人民广播电台", "电台"), ("广播电视台", "台"), ("广播电台", "电台"),
                 ("融媒体中心", "融媒"), ("广播电视", "广电")):
        n = n.replace(a, b)
    if len(n.encode("utf-8")) <= MAX_NAME:
        return n

    # ③ 还超长就去掉括号注解（括号外的才是台名，括号里是补充说明）
    m = re.match(r"^(.+?)[（(].*$", n)
    if m and len(m.group(1)) >= 3:
        n = m.group(1).strip()

    # ④ 硬截之后把尾巴上的虚词/分隔符去掉（避免出现「…国际旅游岛之」这种）
    n = fit(n)
    n = re.sub(r"[之的与和及·•\-—_]+$", "", n)
    return n or fit(name)


def ep_title(book, raw):
    """单集标题 → 只留「这一集讲什么」，屏上 / App 里都短得下。

    先砍掉书名前缀和集号，再去掉后面的括号注解（「（小熊有声）」这类跟内容无关）。
    只剩一个集号的（「第1章」「01集」「0007」）没有信息量，直接留空，
    App 那边就只显示「第 N 集」，不在真机屏上重复一遍「第3集 第3章」。
    """
    t = (raw or "").strip()
    # 书名前缀有几种写法：直接接、「《书名》」、书名后跟 - 或 : ，都要剥掉
    for pre in (book, "《" + book + "》", "《" + book, book + "》"):
        if pre and t.startswith(pre):
            t = t[len(pre):]
            break
    else:
        # 书名也可能出现在结尾（「xx 鬼吹灯」不常见，跳过尾部的处理）
        pass
    t = t.lstrip(" -—_:：、|·")
    # 开头的方括号包装（「【三国演义】 030 …」）：书名在里面、集号跟在后面，
    # 不先扒掉这层，后面的「去集号」就削不动它
    t = re.sub(r"^[【\[][^】\]]*[】\]]\s*", "", t)
    t = re.sub(r"^\s*0*\d{1,4}\s*[集章回话期]?\s*[-—_:：、]?\s*", "", t)   # 开头的集号（001_ / 01集 / 0041 ）
    # 开头的「第100集」「第一百六十六集」「第12章：」这类，前面大字已经写了集号，别再重复
    t = re.sub(r"^\s*第\s*[0-9一二三四五六七八九十百千零两]{1,8}\s*[集章回话期篇]\s*[：:、_\s-]*", "", t)
    # 尾巴上的括号注解：「（小熊有声）」这类跟内容无关的剥掉，
    # 但「（上）/（中）/（下）/（一）…」是**分集序号**，剥掉后同一回的几集标题会一模一样
    # （三国演义 百名CV 版一回分上中下 3 集 / 5 集），必须留着 —— 1.46.32 加的这个例外。
    if not _PART_SUFFIX.search(t):
        t = re.sub(r"[（(][^）)]*[）)]\s*$", "", t)             # 尾巴上的括号注解
    t = t.strip(" -—_·、|,，")
    if re.match(r"^第?\s*[0-9一二三四五六七八九十百千零两]{1,6}\s*[章节回集话期篇]?$", t):
        return ""
    # 上传者把某些集的标题直接改成「求订阅，求评论，求转发」这种拉票话术 ——
    # 一点内容信息都没有，留在选集里就是满屏「求订阅」（三国演义 145 集里 45 集这么写）。
    # 判据：把订阅/关注/评论这类词和标点全去掉之后什么都不剩。
    if _JUNK_TITLE.fullmatch(t):
        return ""
    # 90 字节 = 30 个汉字：App 的选集列表够放，真机屏那行由 ui.cpp 的 cutTo()
    # 按字宽自己截（它会补「…」）。这里只防一条超长标题把 .h 撑爆。
    t2 = fit(t, 90)
    if t2 != t:
        t2 = fit(t, 87) + "…"
    return t2


def load_books():
    """有声书：三个源合起来

    ① tools/有声书源.json       喜马拉雅专辑 RSS（fetch_books.py，老 5 部）
    ② tools/有声书源-听书网.json 13 听书网逐集解析（fetch_t13.py，1.10.0 新增 14 部）
    ③ tools/有声书源-喜马.json   喜马拉雅单专辑抓取（fetch_xm.py，1.11.0 新增；
                                走 mobile/v1/album + mobile/v1/track，能拿多少集拿多少集）
    ④ tools/有声书源-云听.json   云听 radio.cn 栏目/专辑（fetch_rcn.py，1.13.0 新增；
                                音频是 ytmedia.radio.cn 上的**标准 mp3**，无签名、免 Referer、
                                与出口 IP 无关，是目前最稳的一路）
    ⑤ tools/有声书源-蜻蜓.json   蜻蜓 FM 点播专辑（fetch_qtfm_album.py，1.16.0 新增；
                                音频是 m4a，CDN 却标成 audio/mpeg ⇒ 必须配仓库里那份
                                打过补丁的 lib/ESP32-audioI2S 才能出声，见 platformio.ini）

    返回值：OrderedDict  group -> [ {name, items:[(url, title), ...]}, ... ]
    一部书在台表里只出现**一次**（台名＝书名），集是这本书自己的一张表。
    整本书的标题都一样（如「三国演义 001/002 经典评书三国」）时，标题没意义，
    统一留空 —— App 那边就只显示「第 N 集」。
    """
    by = collections.OrderedDict()
    by_name = {}          # 1.46.31：书名 -> 已收的那本（同名去重用）
    for fn, who in (("有声书源.json", "喜马"),
                    ("有声书源-听书网.json", "听书网"),
                    ("有声书源-喜马.json", "喜马单辑"),
                    ("有声书源-云听.json", "云听"),
                    ("有声书源-蜻蜓.json", "蜻蜓")):
        path = os.path.join(HERE, fn)
        if not os.path.exists(path):
            if fn == "有声书源.json":
                print("  [提示] 没有 %s，跳过这批（先跑 tools/fetch_books.py）" % fn)
            continue
        for b in json.load(open(path, encoding="utf-8")):
            name = b["name"]
            if name in DROP_BOOKS:
                print("  有声书[%s] 跳过 %s（DROP_BOOKS）" % (who, name))
                continue
            if (fn, name) in DROP_SRC:
                print("  有声书[%s] 跳过 %s（DROP_SRC：已换成别的源）" % (who, name))
                continue
            items = [(it["url"], ep_title(name, it.get("title", ""))) for it in b["items"]]
            # 全一样 → 没信息量，整本留空（App 只显示「第 N 集」）。
            # 1.18.0：判的是「有内容的标题剩几种」—— 整本集数从 15 涨到 145 以后，
            # 里面混进了几集被上传者写成「求订阅」的，清掉那些之后还剩 1 种，也得算「一样」。
            titles = set(t for _, t in items if t)
            # 「没信息量」的两种情形：
            #   ① 只剩一种有内容的标题 —— 全本都叫这个，等于没有；
            #   ② 种类极少（≤5 种）且远少于集数（不到十分之一）—— 三国演义 145 集里
            #      只有「经典评书三国」「经典评书系列」两种，混在里面的 45 集是拉票话术，
            #      这种情况整本留空，App 只显示「第 N 集」才干净。
            if len(titles) <= 1 or (len(titles) <= 5 and len(titles) * 10 <= len(items)):
                items = [(u, "") for u, _ in items]
            # 1.46.31：同名书只留一份。固件的集表是**按书名**查的（kEpisBook.name），
            #   两份同名会让「这台书的集表」指向错的那一份。
            #   留集数多的：老 RSS 源《红楼梦》只有 15 集，用户点名的喜马版有 109 集。
            prev = by_name.get(name)
            if prev is not None:
                if len(prev["items"]) >= len(items):
                    print("  有声书[%s][%s] %s 跳过（同名已收 %d 集，这版只有 %d 集）"
                          % (who, b["group"], name, len(prev["items"]), len(items)))
                    continue
                for lst in by.values():
                    if prev in lst:
                        lst.remove(prev)
                print("  有声书[%s][%s] %s 用 %d 集覆盖同名的 %d 集"
                      % (who, b["group"], name, len(items), len(prev["items"])))
            rec = {"name": name, "items": items}
            by_name[name] = rec
            by.setdefault(b["group"], []).append(rec)
            print("  有声书[%s][%s] %s %d 集" % (who, b["group"], name, len(items)))
    return by


def parse_md(path):
    """返回 [(小节标题, [(台号, 台名), ...]), ...]，保持文件顺序"""
    text = open(path, encoding="utf-8").read()
    secs, cur = [], None
    for line in text.split("\n"):
        m = re.match(r"^##\s+(.+?)\s*$", line)
        if m:
            title = re.sub(r"[（(].*?[）)]", "", m.group(1)).strip()   # 「北京（10 → 6）」→「北京」
            title = title.replace("★", "").strip()
            cur = (title, [])
            secs.append(cur)
            continue
        # 第三栏既可以是蜻蜓台号（纯数字），也可以是完整 http 地址（中央台那批 HLS）
        m = re.match(r"^\|\s*\d+\s*\|\s*([^|]+?)\s*\|\s*(\d+|https?://[^\s|]+)\s*\|", line)
        if m and cur is not None:
            cur[1].append((m.group(2), m.group(1).strip()))
    return secs


def main():
    secs = parse_md(SRC)

    # 先按台号去重（「苏州本地」与「江苏」两节有重复），保留第一次出现的
    seen, uniq = {}, []
    for title, rows in secs:
        for sid, name in rows:
            if sid in seen or sid in DEAD:
                continue
            seen[sid] = name
            uniq.append((title, sid, name))

    buckets = collections.OrderedDict()
    for label in ORDER:
        buckets[label] = []

    cctv_list = []                             # 央视：单列，不被书打断
    home = []
    back = collections.OrderedDict()           # 分组 -> 苏州的台（各自放组末）
    front = collections.OrderedDict()          # 小节/关键词 -> 台
    for title, sid, name in uniq:
        if title == SX:                        # 陕西整节并进广播组
            home.append((sid, name))
            continue
        if title == CCTV_SECTION:              # 央视：单独一桶，后面插在中央台之后
            cctv_list.append((sid, name))
            continue
        # 台表里的台全是蜻蜓电台 → 一律进「广播」组（2026-11-01 起不再按名字拆组）
        label = OTHER
        # 苏州的台：一律记成「广播组的尾巴」
        if title == BACK_SECTION:
            back.setdefault(label, []).append((sid, name))
            continue
        if label == OTHER:                     # 广播组的前置小节/关键词
            if title in HEAD_SECTIONS:         # 中央台：整组顶到最前面
                front.setdefault(title, []).append((sid, name))
                continue
            if title in FRONT_SECTIONS:
                front.setdefault(title, []).append((sid, name))
                continue
            if FRONT_KEYWORD in name:
                front.setdefault(FRONT_KEYWORD, []).append((sid, name))
                continue
        buckets[label].append((sid, name))

    # 广播组：中央台 → 陕西 → 四川 → 重庆 → 兵团 → 其余（保持原顺序）→ 苏州（下面统一追加）
    head_list = []
    for k in HEAD_SECTIONS:
        head_list += front.get(k, [])
    front_list = []
    for k in FRONT_SECTIONS + [FRONT_KEYWORD]:
        front_list += front.get(k, [])
    rest = len(buckets[OTHER])
    buckets[OTHER] = head_list + home + front_list + buckets[OTHER]
    for label, items in back.items():
        if label in buckets:
            buckets[label] = buckets[label] + items
        else:
            print("  [跳过] 苏州 %d 台要放的组「%s」不存在" % (len(items), label))
    print("  央视 %d 台（插在中央台之后）" % len(cctv_list))
    print("  广播组重排：中央台 %d + 陕西 %d + 川渝兵团 %d + 其余 %d + 苏州 %d"
          % (len(head_list), len(home), len(front_list), rest, len(back.get(OTHER, []))))

    books = load_books()

    # ---------------- 1.15.0：不再分组，只有一条播放顺序 ----------------
    # ① 电台：全部在 buckets[OTHER]，顺序 = 中央台 → 陕西 → 川渝兵团 → 其余 → 苏州
    radios = [(OTHER, sid, name) for sid, name in buckets[OTHER]]

    # ② 有声书：按 ORDER 里标签的先后（军事历史 / 历史 / 评书 / 广播剧 / 悬疑 / 都市）
    book_rows = []
    for label in ORDER:
        if label == OTHER:
            continue
        for bk in books.get(label, []):
            book_rows.append((label, bk["items"][0][0], bk["name"]))

    # ③ 书均匀穿插进电台（按台数 / 书数均分，不堆末尾、不邻接）
    #    ★ 1.46.31 起插入起点由 BOOK_START_IDX 决定（用户要求 11）：
    #      前 11 台原样不动，书撒在之后那一段里。
    #      ⚠️ 11 落在央视中间 —— CCTV1/CCTV2 在前 11 台里，CCTV3 往后会被书分开。
    #      想保央视连成一片，把 BOOK_START_IDX 改成 22。
    n_head = min(len(head_list), len(radios))
    cctv_rows = [(CCTV_TAG, sid, nm) for sid, nm in cctv_list]
    # 1.46.31：先把「中央台 → 央视 → 其余电台」拼成一条完整序列，
    #   再在第 BOOK_START_IDX 个台之后切开 —— 前半段原样不动，书只撒后半段。
    all_radios = radios[:n_head] + cctv_rows + radios[n_head:]
    split = max(1, min(BOOK_START_IDX, len(all_radios)))
    entries = (all_radios[:split]
               + interleave(all_radios[split:], book_rows))
    print("  书从第 %d 台之后开始穿插（前 %d 台原样不动）" % (split, split))

    # ④ 标签：只给 App 筛选用，不影响播放顺序
    tags, tag_of = [], []
    for label, _sid, _nm in entries:
        if label not in tags:
            tags.append(label)
        tag_of.append(tags.index(label))
    tag_cnt = [0] * len(tags)
    for k in tag_of:
        tag_cnt[k] += 1

    st_lines, renamed = [], []
    for _label, sid, name in entries:
        short = shrink(name)
        if short != name:
            renamed.append((name, short))
        st_lines.append('  { %s, %s },' % (cstr(sid), cstr(short)))

    # 集表：按书的顺序排，书号就是这里的下标（台表顺序变了也不影响，按书名查）
    epi_items, epi_books = [], []
    for label in ORDER:
        for bk in books.get(label, []):
            first = len(epi_items)
            for url, tt in bk["items"]:
                epi_items.append((tt, url))
            epi_books.append((shrink(bk["name"]), first, len(bk["items"])))

    hdr = (
        "// 自动生成，请勿手改。\n"
        "// 来源：tools/台表-精选.md（人工精选，去音乐台 / 广告台 / 非汉语台）\n"
        "// 有声书来源：tools/有声书源*.json（喜马 / 听书网 / 云听 / 蜻蜓，见本脚本顶部说明）\n"
        "// 重新生成：python tools/gen_preset_curated.py\n"
        "// 换回全量地区表：python tools/fetch_qtfm.py && python tools/gen_preset.py\n"
        "//\n"
        "// 1.15.0 起固件**不再分组**：kPreset 的先后就是唯一的播放顺序，\n"
        "// 旋钮从第 0 台一路转到最后一台（%d 台），没有「组」这一维。\n"
        "// %d 部有声书已**均匀穿插**在电台之间（约每 %d 台一本），不会全堆在末尾。\n"
        "//\n"
        "// 「标签」只给手机 App 做筛选 / 管理用，不影响播放顺序：\n"
        "//   kPresetTagOf[i] = 第 i 台的标签下标（与 kPreset 一一对应）\n"
        "//   kPresetTag[]    = 标签名 + 台数（顺序 = 首次出现的先后）\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "\n"
        "struct PresetSt  { const char* id; const char* name; };\n"
        "struct PresetTag { const char* name; uint16_t count; };\n"
        "\n"
        "#define PRESET_ST_N   %d\n"
        "#define PRESET_TAG_N  %d\n"
        "#define PRESET_VER    %d\n"
        "\n"
        "static const PresetSt kPreset[] = {\n"
        % (len(entries), len(book_rows), max(1, round(len(entries) / max(1, len(book_rows)))),
           len(entries), len(tags), PRESET_VER)
    )
    body = "\n".join(st_lines) + "\n};\n\n"

    body += "// 每台所属标签（与 kPreset 一一对应；只给 App 筛选用）\n"
    body += "static const uint8_t kPresetTagOf[] = {\n"
    for i in range(0, len(tag_of), 24):
        body += "  " + ",".join(str(x) for x in tag_of[i:i + 24]) + ",\n"
    body += "};\n\n"

    body += "static const PresetTag kPresetTag[] = {\n"
    body += "\n".join('  { %s, %d },' % (cstr(n), c) for n, c in zip(tags, tag_cnt))
    body += "\n};\n"
    open(OUT, "w", encoding="utf-8", newline="\n").write(hdr + body)

    ehdr = (
        "// 自动生成，请勿手改。\n"
        "// 来源：tools/有声书源*.json（喜马 / 听书网 / 云听 / 蜻蜓；一部书 = 一张集表，与台表里的那个台同名）\n"
        "// 重新生成：python tools/gen_preset_curated.py\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "\n"
        "struct EpisItem { const char* t; const char* u; };   // t=单集短名（可为空） u=直链\n"
        "struct EpisBook { const char* name; uint16_t first; uint16_t count; };\n"
        "\n"
        "#define EPIS_ITEM_N   %d\n"
        "#define EPIS_BOOK_N   %d\n"
        "\n"
        "static const EpisItem kEpis[] = {\n" % (len(epi_items), len(epi_books))
    )
    ebody = "\n".join('  { %s, %s },' % (cstr(t), cstr(u)) for t, u in epi_items) + "\n};\n\n"
    ebody += "static const EpisBook kEpisBook[] = {\n"
    ebody += "\n".join('  { %s, %d, %d },' % (cstr(n), f, c) for n, f, c in epi_books)
    ebody += "\n};\n"
    open(EPIS_OUT, "w", encoding="utf-8", newline="\n").write(ehdr + ebody)

    print("写出 %s：%d 台 / %d 个标签（已剔除 %d 个下架台）"
          % (OUT, len(entries), len(tags), len(DEAD)))
    print("  电台 %d + 有声书 %d，书均匀穿插（约每 %d 台一本）"
          % (len(radios), len(book_rows),
             int(round(len(radios) / float(len(book_rows) + 1)))))
    print("写出 %s：%d 部书 / %d 集" % (EPIS_OUT, len(epi_books), len(epi_items)))
    for n, f, c in epi_books:
        print("  %-16s %2d 集" % (n, c))
    for n, c in zip(tags, tag_cnt):
        print("  标签 %-6s %3d 台" % (n, c))
    if renamed:
        print("  超长台名做了缩写（%d 个）：" % len(renamed))
        for a, b in renamed:
            print("    %s  ->  %s" % (a, b))


if __name__ == "__main__":
    main()
