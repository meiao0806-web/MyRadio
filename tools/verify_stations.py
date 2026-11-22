#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""台表回归：确认「用户点名删掉的台」真的没了、「加的台」真的在。

用法：python tools/verify_stations.py
断言读的是**编译进固件的那份** src/preset_stations.h，不是 md 源 ——
md 改了但没重新生成，是最容易犯的错。
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
H = os.path.join(ROOT, "src", "preset_stations.h")
MD = os.path.join(HERE, "台表-精选.md")

# 1.46.20/21：用户点名要删的台（md 里的原名）
DROP = [
    # 央视 5 个
    "CCTV5体育", "CCTV6电影", "CCTV8电视剧", "CCTV14少儿", "CCTV16奥林匹克",
    # 陕西
    "西安新闻广播", "安康综合广播", "彬州之声", "渭南广播FM102.6", "渭南广播FM90.9",
    # 四川
    "四川南部新闻综合广播", "成都新闻广播", "巴中新闻综合广播", "遂宁新闻综合台",
    "FM97.4广汉市广播电视台综合广播", "中江综合广播994",
    # 重庆
    "万盛融媒体中心综合广播", "綦江综合广播", "重庆之声", "重庆嘉陵之声FM88.7", "重庆巴渝之声",
    # 兵团 / 北京 / 河北
    "兵团882综合广播", "北京大兴人民广播电台FM986",
    "保定新闻广播", "邯郸新闻综合广播", "FM963景县融媒综合广播",
    # 上海
    "金山区广播电视台综合广播", "东上海之声FM106.5", "长三角之声", "上海经典947", "上海交通广播电台",
    # 山西
    "大同广播电视台新闻综合广播", "山西综合广播FM904", "平定综合广播FM101.1", "长治综合广播",
    # 内蒙古
    "内蒙古新闻综合广播", "FM94.9包头综合广播", "乌海综合广播", "赤峰综合广播", "阿拉善综合广播",
    # 辽宁
    "朝阳新闻综合广播", "瓦房店广播电视台新闻综合广播", "桓仁广播电视台综合广播",
    # 吉林
    "吉林新闻综合广播", "松原新闻综合广播", "长春新闻广播", "辉南综合广播",
]

# 1.46.22：用户第二批点名要删的台
DROP2 = [
    # 黑龙江
    "大兴安岭综合广播", "大庆综合广播", "牡丹江综合广播", "鸡西综合广播", "冰城融媒体电台",
    # 江苏
    "AM1161无锡新闻综合广播", "FM93.7无锡新闻综合广播", "南通新闻广播", "昆山人民广播电台",
    "苏州儿童广播", "苏州交通广播", "苏州戏曲广播",
    # 浙江
    "FM989宁海新闻综合广播", "FM998 舟山新闻综合广播",
    # 广东 / 广西 / 海南
    "潮州戏曲广播", "琼海人民广播电台", "畅听882（贺州广播电视台综合广播）",
    "南宁990新闻台", "钦州新闻综合广播", "广西970女主播电台",
    "海南旅游广播•国际旅游岛之声",
    # 甘宁
    "甘肃交通广播", "宁夏交通广播",
]

# 1.46.29：用户第三批点名要删的台
DROP3 = [
    "经典FM1008",       # 天津
    "沧州新闻广播",      # 河北
]

# 用户要的「一本不要的书」
DROP_BOOKS = ["每晚一个历史故事"]

# 用户要加的台
ADD = [("东广新闻", "275")]

ok_n = bad_n = 0


def chk(name, got, want):
    global ok_n, bad_n
    if got == want:
        ok_n += 1
        print("  OK   %s" % name)
    else:
        bad_n += 1
        print("  FAIL %s  期望 %r 实际 %r" % (name, want, got))


def main(verbose=False):
    h = io.open(H, encoding="utf-8").read()
    rows = re.findall(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}', h)
    names = [n for _, n in rows]
    urls = {n: u for u, n in rows}

    print("=== 1) 删掉的台 ===")
    for n in DROP:
        # 台表里存的是「缩写后」的名字（shrink() 会砍频率前缀等），所以按「包含」判定
        kw = n
        still = [x for x in names if x == kw]
        chk("已删除 %s" % n, still, [])

    print("=== 1b) 第二批删掉的台（1.46.22）===")
    for n in DROP2:
        chk("已删除 %s" % n, [x for x in names if x == n], [])

    print("=== 1c) 第三批删掉的台（1.46.29）===")
    for n in DROP3:
        chk("已删除 %s" % n, [x for x in names if x == n], [])

    print("=== 2) 删掉的书 ===")
    for n in DROP_BOOKS:
        chk("已删除 %s" % n, [x for x in names if x == n], [])

    print("=== 3) 新加的台 ===")
    for n, sid in ADD:
        chk("%s 在台表里" % n, n in urls, True)
        if n in urls:
            chk("%s 台号=%s" % (n, sid), urls[n], sid)

    print("=== 4) 该保留的别误删 ===")
    keep = ["中国之声", "经济之声", "上海新闻广播", "上海戏曲广播",
            "CCTV1综合", "CCTV13新闻", "CCTV17农业农村",
            "三国演义", "鬼吹灯", "一路高升", "宇宙真相大探索", "中国绝密档案",
            "上下五千年", "官场现形记", "如临大敌",
            "苏州新闻广播", "苏州生活广播", "常熟新闻广播",
            "延边新闻广播", "FM106.9海城综合广播",
            "哈尔滨广播电视台综合广播", "哈尔滨文艺广播", "龙广新闻台",
            "FM89.1吴江综合广播", "江苏故事广播"]
    for n in keep:
        chk("保留 %s" % n, n in names, True)

    print("=== 5) 台表版本号 ===")
    m = re.search(r"#define\s+PRESET_VER\s+(\d+)", h)
    ver = int(m.group(1)) if m else -1
    chk("PRESET_VER >= 12（改了台表就必须升，否则老机器不重灌）", ver >= 12, True)

    print("=== 6) 台数与去重 ===")
    n_st = int(re.search(r"#define\s+PRESET_ST_N\s+(\d+)", h).group(1))
    chk("PRESET_ST_N 与条目数一致", n_st, len(rows))
    dup = [x for x in set(names) if names.count(x) > 1]
    chk("台名无重复", dup, [])

    print("=== 7) md 源与固件头一致 ===")
    md = io.open(MD, encoding="utf-8").read()
    md_names = set(re.findall(r"^\|\s*\d+\s*\|\s*([^|]+?)\s*\|\s*(?:\d+|https?://[^\s|]+)\s*\|\s*$",
                              md, re.M))
    # md 里的名字经过 shrink() 可能变短，所以只查「md 里还有没有已删的台」
    left = [n for n in (DROP + DROP2 + DROP3) if n in md_names]
    chk("md 源里也没有这些台", left, [])
    chk("md 源里有东广新闻", "东广新闻" in md_names, True)

    print("\n%d 通过 / %d 失败" % (ok_n, bad_n))
    return 0 if bad_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main("-v" in sys.argv))
