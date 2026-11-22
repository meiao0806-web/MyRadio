#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""喜马拉雅专辑 -> tools/有声书源-喜马.json  （边抓边验，遇收费边界就停）

为什么要「验证」而不只是「取地址」：
    喜马很多是 VIP 专辑：前几十集免费，后面 playUrl64 仍然是**有值的**，
    但真去拉会被 403。所以每集必须 Range 试拉 1KB，只有 2xx/206 + audio/* 才算数。

为什么要「遇界就停」：
    收费集是**连续的一大段**（不是散在各处），实测都是「前 N 集免费、后面全收费」。
    连续 3 集拉不动就认定越过边界，直接收工，省掉几百次无用请求。

实测结论（2026-11-01，14 本书在喜马的候选）：
    · 标着「免费/永久免费」的专辑      → 整本可拿（为民无悔 / 春晚相声小品 / 鬼吹灯 / 枪毙刘汉臣）
    · 标着「VIP免费多人剧」的专辑      → 前 ~100 集免费，后面 403
    · 老牌付费专辑（首席医官阿陈版等）  → 只开放前几集
    接口一次只给 100 条且不分页，所以一本最多也只能拿首尾各 100 集（见 fetch_xm.py 注释）。

用法：
    python tools/fetch_xm2.py --album=105008938 --group=都市 --name=为民无悔
    python tools/fetch_xm2.py --album=105008938 --plan     # 只探可用集数，不写文件
"""
import sys, os, re, json, time, ssl, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, '有声书源-喜马.json')

UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')
CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE
PH = {k: v for k, v in (('http', os.environ.get('http_proxy', '')),
      ('https', os.environ.get('https_proxy', ''))) if v}   # 空字符串会让 ProxyHandler 崩掉
OP = urllib.request.build_opener(urllib.request.ProxyHandler(PH),
                                 urllib.request.HTTPSHandler(context=CTX))

GAP = float(os.environ.get('GAP', 0.22))
STOP_AFTER = int(os.environ.get('STOP_AFTER', 3))     # 连续几集拉不动就收工


def jget(u, ref='https://www.ximalaya.com/', tries=3):
    H = {'User-Agent': UA, 'Accept': 'application/json, text/plain, */*', 'Referer': ref}
    for k in range(tries):
        try:
            r = OP.open(urllib.request.Request(u, headers=H), timeout=25)
            return json.loads(r.read().decode('utf-8', 'replace'))
        except Exception:
            time.sleep(1.0 * (k + 1))
    return None


def head_tracks(album, n=100, asc=True):
    j = jget('https://mobile.ximalaya.com/mobile/v1/album?albumId=%s&pageSize=%d&isAsc=%s'
             % (album, n, 'true' if asc else 'false'))
    if not j or j.get('ret') != 0:
        return [], 0
    t = (j.get('data') or {}).get('tracks') or {}
    return list(t.get('list') or []), int(t.get('totalCount') or 0)


def tracks_all(album):
    """两种排序各取一批，按 orderNo 合并 —— 这是「≤200 集能拿全」唯一的路子。

    2026-11-04 实测（album 38217725，标称 144 集）：
        isAsc=true  → 第 45~145 集（最新的 100 集）
        isAsc=false → 第 1~100 集
        两批并集 = 144 集，一条不缺；单看任何一种都只有 100 集。
    接口不分页（pageId 无效），所以这是绕过 100 条硬顶的唯一办法。
    >200 集的专辑仍然拿不全，交付时要说清楚。
    """
    a, total = head_tracks(album, 100, True)
    if not a:
        return [], 0
    if total > len(a):
        b, _ = head_tracks(album, 100, False)
        seen = {str(x['trackId']) for x in a}
        a = a + [x for x in b if str(x['trackId']) not in seen]
    a.sort(key=lambda x: (int(x.get('orderNo') or 0), str(x.get('trackId'))))
    return a, total


def audio_of(tid):
    j = jget('https://mobile.ximalaya.com/mobile/v1/track/%s' % tid)
    if not j:
        return '', ''
    for k in ('playUrl64', 'playUrl32', 'playPathAacv164', 'playPathAacv224'):
        v = j.get(k)
        if v and str(v).startswith('http'):
            return v, (j.get('title') or '')
    return '', (j.get('title') or '')


def playable(u):
    """真去拉 1KB，只有 audio 才算可播"""
    try:
        r = OP.open(urllib.request.Request(u, headers={'User-Agent': UA, 'Range': 'bytes=0-1023'}), timeout=20)
        ct = (r.headers.get('Content-Type') or '')
        return r.status in (200, 206) and 'audio' in ct
    except Exception:
        return False


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1); kv[k] = v
        elif a.startswith('--'):
            kv[a[2:]] = True
    album = str(kv.get('album') or '').strip()
    if not album:
        print('用法：python tools/fetch_xm2.py --album=<id> [--group=都市] [--name=书名] [--plan]')
        return
    group = kv.get('group') or '评书'
    name = kv.get('name') or ''
    plan = bool(kv.get('plan'))

    tr, total = tracks_all(album)
    if not tr:
        print('曲目取不到（album=%s）' % album)
        return
    print('专辑 %s：标称 %d 集，两种排序合并后拿到 %d 集' % (album, total, len(tr)))

    cache = os.path.join(HERE, '.xm2_%s.json' % album)
    done = {}
    if os.path.exists(cache):
        try:
            done = json.load(open(cache, encoding='utf-8'))
        except Exception:
            done = {}

    items, miss = [], 0
    t0 = time.time()
    for n, x in enumerate(tr, 1):
        tid = str(x['trackId'])
        rec = done.get(tid)
        if rec is None:
            u, tt = audio_of(tid)
            ok = bool(u) and playable(u)
            rec = {'url': u if ok else '', 'title': tt or x.get('title') or '', 'ok': ok}
            done[tid] = rec
            time.sleep(GAP)
        if rec.get('ok') and rec.get('url'):
            items.append({'title': rec['title'], 'url': rec['url']})
            miss = 0
        else:
            miss += 1
            if miss >= STOP_AFTER:
                print('  连续 %d 集拉不动 → 认定越过收费边界，收工' % miss)
                break
        if n % 20 == 0:
            el = time.time() - t0
            print('  第 %d/%d 集，可用 %d  (%.2fs/集)' % (n, len(tr), len(items), el / n))

    json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('\n《%s》可用 %d 集 / 目标 %d' % (name or album, len(items), total))
    if items:
        print('  第 1 集：%s' % items[0]['title'][:40])
        print('  末一集：%s' % items[-1]['title'][:40])
    if plan:
        return
    if not items:
        print('没有可用集，未写文件')
        return

    old = []
    if os.path.exists(OUT):
        try:
            old = [b for b in json.load(open(OUT, encoding='utf-8')) if str(b.get('album')) != album]
        except Exception:
            old = []
    old.append({'group': group, 'name': name or ('喜马%s' % album),
                'album': album, 'total': total, 'items': items})
    json.dump(old, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('写出 %s：%d 部书 / 本书 %d 集'
          % (os.path.relpath(OUT, ROOT), len(old), len(items)))


if __name__ == '__main__':
    main()
