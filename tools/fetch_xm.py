#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""喜马拉雅专辑 -> tools/有声书源-喜马.json（能拿多少集拿多少集）

两条实测可用的接口（2026-11-01）：
    ① 曲目表：https://mobile.ximalaya.com/mobile/v1/album?albumId=<album>&pageSize=100&isAsc=true|false
       返回 data.tracks.list[]（trackId / orderNo / title），data.tracks.totalCount 是专辑总集数。
    ② 音频：https://mobile.ximalaya.com/mobile/v1/track/<trackId>
       返回 playUrl64（mp3，实测 206 + audio/mpeg，可直接 Range 拉）。

⚠️ 这个接口最多只给 100 条，且**分页参数全都不生效**（pageId / page / pageNum / pageSize>100 /
   offset / start / 游标 lastTrackId … 逐个试过，首条永远是同一条）。
   唯一有效的是 isAsc：true 给 orderNo 1~100，false 给 orderNo (N-99)~N。
   所以一本 526 集的专辑只能拿到首尾各 100 集（共 200 集），中间那段拿不到。
   想拿全量必须登录态：revision/album/v1/getTracksList 无登录回 401「用户未登录」/
   407「webtk缺失」，浏览器里页面自己调也是 401。

为什么不用别的：
    · album/<id>.xml（RSS）只有上传者主动开了 RSS 才有，这本回 204；
    · revision/play/album 回 ret=200 但 tracksAudioPlay 是空数组（未登录）；
    · mobile/v1/album/track（老接口）回 924「该内容因故已下架」；
    · www.ximalaya.com/tracks/<id>.json 的 play_path 对非免费节目是空串；
    · 专辑页 / sound 页都是 SPA 空壳，裸 HTTP 拿不到曲目链接。

用法：
    python tools/fetch_xm.py --album=53688396 --group=评书 --name=乱世枭雄
    python tools/fetch_xm.py --album=53688396 --plan        # 只报能拿多少集
"""
import sys
import os
import json
import time
import ssl
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, '有声书源-喜马.json')

UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE
PH = {k: v for k, v in (('http', os.environ.get('http_proxy', '')),
      ('https', os.environ.get('https_proxy', ''))) if v}   # 空字符串会让 ProxyHandler 崩掉
OP = urllib.request.build_opener(urllib.request.ProxyHandler(PH),
                                 urllib.request.HTTPSHandler(context=CTX))

GAP = float(os.environ.get('GAP', 0.28))
MAXP = int(os.environ.get('MAXP', 100))        # 接口硬上限，别改


def get_json(url, referer='https://www.ximalaya.com/', tries=3):
    H = {'User-Agent': UA, 'Accept': 'application/json, text/plain, */*',
         'Accept-Encoding': 'identity', 'Referer': referer}
    for k in range(tries):
        try:
            r = OP.open(urllib.request.Request(url, headers=H), timeout=25)
            return json.loads(r.read().decode('utf-8', 'replace'))
        except Exception:
            time.sleep(1.5 * (k + 1))
    return None


def album_slice(album, asc):
    u = ('https://mobile.ximalaya.com/mobile/v1/album?albumId=%s&pageSize=%d&isAsc=%s'
         % (album, MAXP, 'true' if asc else 'false'))
    j = get_json(u, referer='https://www.ximalaya.com/album/%s' % album)
    if not j or j.get('ret') != 0:
        return [], 0
    t = (j.get('data') or {}).get('tracks') or {}
    return list(t.get('list') or []), int(t.get('totalCount') or 0)


def track_audio(tid):
    j = get_json('https://mobile.ximalaya.com/mobile/v1/track/%s' % tid,
                 referer='https://www.ximalaya.com/')
    if not j or j.get('ret') not in (0, None):
        return ''
    for k in ('playUrl64', 'playUrl32', 'playPathAacv164', 'playPathAacv224'):
        v = j.get(k)
        if v and str(v).startswith('http'):
            return v
    return ''


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1)
            kv[k] = v
        elif a.startswith('--'):
            kv[a[2:]] = True
    album = str(kv.get('album') or '').strip()
    if not album:
        print('用法：python tools/fetch_xm.py --album=53688396 [--group=评书] [--name=书名] [--plan]')
        return
    group = kv.get('group') or '评书'
    name = kv.get('name') or ''
    plan = bool(kv.get('plan'))

    # ---- ① 首尾两批曲目 ----
    head, total = album_slice(album, True)
    tail, _ = album_slice(album, False)
    print('专辑 %s：共 %d 集；接口给头 %d / 尾 %d' % (album, total, len(head), len(tail)))

    seen = set()
    rows = []
    for x in head + tail:
        tid = str(x.get('trackId') or '')
        if not tid or tid in seen:
            continue
        seen.add(tid)
        rows.append(x)
    rows.sort(key=lambda x: (x.get('orderNo') or 0))
    print('合并去重 %d 集（第 %s～%s 集）'
          % (len(rows), rows[0].get('orderNo') if rows else '-', rows[-1].get('orderNo') if rows else '-'))
    if total and len(rows) < total:
        miss = total - len(rows)
        print('⚠️ 还差 %d 集拿不到：喜马这个接口不分页，中间段必须登录才能取。' % miss)
    if plan:
        return

    if not name:
        name = kv.get('name') or ('喜马专辑%s' % album)

    # ---- ② 逐集音频（带缓存，断了能续） ----
    cache = os.path.join(HERE, '.xm_cache_%s.json' % album)
    done = {}
    if os.path.exists(cache):
        try:
            done = json.load(open(cache, encoding='utf-8'))
        except Exception:
            done = {}

    items, fail = [], 0
    t0 = time.time()
    for n, x in enumerate(rows, 1):
        tid = str(x['trackId'])
        rec = done.get(tid) or {}
        if not rec.get('url'):
            url = track_audio(tid)
            if url:
                rec = {'url': url, 'title': x.get('title') or '', 'orderNo': x.get('orderNo') or 0}
                done[tid] = rec
            time.sleep(GAP)
        if rec.get('url'):
            items.append({'title': rec.get('title') or x.get('title') or '', 'url': rec['url']})
        else:
            fail += 1
        if n % 25 == 0 or n == len(rows):
            el = time.time() - t0
            print('  %d/%d  失败 %d  (%.2fs/集)' % (n, len(rows), fail, el / n))
            json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

    json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    if not items:
        print('一集都没拿到')
        return

    # ---- ③ 合并进总清单（同一专辑覆盖） ----
    old = []
    if os.path.exists(OUT):
        try:
            old = [b for b in json.load(open(OUT, encoding='utf-8'))
                   if str(b.get('album')) != album]
        except Exception:
            old = []
    old.append({'group': group, 'name': name, 'album': album,
                'total': total, 'items': items})
    json.dump(old, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('\n写出 %s：%d 部书 / 本书 %d 集（目标 %d，失败 %d）'
          % (os.path.relpath(OUT, ROOT), len(old), len(items), total, fail))


if __name__ == '__main__':
    main()
