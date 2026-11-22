#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""云听（radio.cn）栏目/专辑 -> tools/有声书源-云听.json

为什么这个源最好（2026-11-01 实测）：
    · 音频是 **标准 mp3**（audio/mpeg，ID3 头），落在 ytmedia.radio.cn 的固定路径上，
      不带任何签名参数、**不需要 Referer**、与出口 IP 无关 —— 直接写进固件就能长期播。
    · 一集一文件（20MB 上下），不像 B 站那种 DASH 分片（moof/trun）解码库认不了。
    · 接口 `pageByAlbum` 一次给 20 条（pageSize 参数被服务端钉死 20，但不限页数），
      83 集的栏目 5 次请求就取全，比听书网一集一页快得多。
    · `feeType` 全 0 —— 云听这些党的理论/纪实类音频基本都免费。

接口签名（网页 api.js 里 key 是明文）：
    signText = "<参数按键名排序 a=1&b=2>" + "&timestamp=<ms>&key=f0fc4c668392f9f9a447e48584c214ee"
    sign     = MD5(signText).upper()      （见 tools/rcn_api.py）

用法：
    python tools/fetch_rcn.py --column=16631244851730 --group=历史 --name=精神的力量
    python tools/fetch_rcn.py --column=16631244851730 --plan     # 只看集数与可播性，不写文件
"""
import sys, os, json, time
import rcn_api

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, '有声书源-云听.json')


def probe(u):
    """真拉 2KB：audio/* 才算可播"""
    import urllib.request
    try:
        r = rcn_api.OP.open(urllib.request.Request(
            u, headers={'Range': 'bytes=0-2047'}), timeout=20)
        return r.status in (200, 206) and 'audio' in (r.headers.get('Content-Type') or '')
    except Exception:
        return False


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1); kv[k] = v
        elif a.startswith('--'):
            kv[a[2:]] = True
    column = str(kv.get('column') or '').strip()
    if not column:
        print('用法：python tools/fetch_rcn.py --column=<columnId> [--group=历史] [--name=书名] [--plan]')
        return
    group = kv.get('group') or '历史'
    name = kv.get('name') or ''
    plan = bool(kv.get('plan'))

    items, page, total = [], 0, 0
    while True:
        j = rcn_api.call('/web/appSingle/pageByAlbum',
                         {'albumId': column, 'pageNo': page, 'pageSize': 20})
        d = j.get('data') or {}
        L = d.get('data') or []
        if not L:
            if page == 0:
                print('取不到曲目（column=%s）：%s' % (column, json.dumps(j, ensure_ascii=False)[:200]))
                return
            break
        items += L
        total = d.get('totalNum') or total
        print('  第 %d 页 +%d 集（已 %d / %s）' % (page + 1, len(L), len(items), total))
        if page + 1 >= (d.get('totalPage') or 0):
            break
        page += 1
        time.sleep(0.35)

    print('栏目 %s：共 %d 集' % (column, len(items)))
    if not name:
        name = '云听%s' % column

    keep, bad = [], 0
    for n, x in enumerate(items, 1):
        u = x.get('playUrlHigh') or x.get('playUrlLow') or ''
        if not u or x.get('feeType'):
            bad += 1
            continue
        # 只抽检（每 10 集验 1 集 + 首末集），全量逐集拉 20MB 头太慢
        if n in (1, len(items)) or n % 10 == 0:
            if not probe(u):
                print('  [跳过] 第 %d 集拉不动：%s' % (n, u[:70]))
                bad += 1
                continue
        keep.append({'title': x.get('name') or '', 'url': u})

    print('\n《%s》可用 %d 集 / 共 %d 集（跳过 %d）' % (name, len(keep), len(items), bad))
    if keep:
        print('  第 1 集：%s' % keep[0]['title'][:44])
        print('  末一集：%s' % keep[-1]['title'][:44])
    if plan or not keep:
        return

    old = []
    if os.path.exists(OUT):
        try:
            old = [b for b in json.load(open(OUT, encoding='utf-8'))
                   if str(b.get('column')) != column]
        except Exception:
            old = []
    old.append({'group': group, 'name': name, 'column': column,
                'total': total, 'items': keep})
    json.dump(old, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('写出 %s：%d 部书 / 本书 %d 集'
          % (os.path.relpath(OUT, ROOT), len(old), len(keep)))


if __name__ == '__main__':
    main()
