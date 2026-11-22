#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""m3u / m3u8 / IPTV 列表 -> 这台机器真能放的台（探活筛）

    python tools/probe_m3u.py --src=https://raw.githubusercontent.com/xxx/TVradio --out=ok.tsv
    python tools/probe_m3u.py --src=列表.m3u --follow --limit=200

为什么需要筛：
    这类合集动辄上千条，但形态混杂 ——
      .m3u8   HLS 切片，2.0.6 的 audioI2S 没有解复用，**一定放不了**
      .php    电视中转（live.cooltv.top 那种），放不了
      [2409:] IPv6 直连，机器没配 IPv6，放不了
      .flv    放不了
    真正能放的只有「http(s) 直连、回 audio/* 或 ID3/帧同步头」的那一小撮。

判据（跟 fetch_rcn.py 的 probe 一个路子）：
    发 Range: bytes=0-2047 拉 2KB —— 200/206 且 content-type 含 audio，
    或者头 3 字节是 ID3 / 前 2 字节是 MP3 帧同步(FFFx)，才算「能放」。
"""
import sys, re, os, json, argparse, collections
import urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

UA = 'VLC/3.0.20 LibVLC/3.0.20'


def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode('utf-8', 'replace')


def parse(text):
    """EXTINF -> {name, g, url}；也吃纯 URL 列表（每行一个）"""
    items, cur = [], None
    for l in text.splitlines():
        l = l.strip()
        if not l:
            continue
        if l.startswith('#EXTINF'):
            m = re.search(r'group-title="([^"]*)"', l)
            cur = {'name': l.rsplit(',', 1)[-1].strip() or '(无名)',
                   'g': m.group(1) if m else ''}
        elif l.startswith('#'):
            continue
        elif l.startswith('http') and cur is not None:
            cur['url'] = l
            items.append(cur)
            cur = None
        elif l.startswith('http') and cur is None:
            items.append({'name': l.rsplit('/', 1)[-1][:30], 'g': '', 'url': l})
    return items


def kind(u):
    p = u.split('?')[0]
    tail = p.rsplit('/', 1)[-1]
    if '.m3u8' in p:
        return 'm3u8'
    if tail.endswith('.flv'):
        return 'flv'
    if tail.endswith('.php'):
        return 'php'
    if re.match(r'^[a-z]+://\[', u):
        return 'ipv6'
    return 'direct'


def probe(u, follow=False, timeout=6):
    """返回 (ok, 说明)"""
    req = urllib.request.Request(u, headers={'User-Agent': UA, 'Range': 'bytes=0-2047'})
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if not follow or e.code not in (301, 302, 303, 307, 308):
            return False, 'HTTP %d' % e.code
        loc = e.headers.get('Location')
        if not loc:
            return False, 'HTTP %d 无 Location' % e.code
        try:
            r = urllib.request.urlopen(
                urllib.request.Request(loc, headers={'User-Agent': UA, 'Range': 'bytes=0-2047'}),
                timeout=timeout)
        except Exception as e2:
            return False, type(e2).__name__
    except Exception as e:
        return False, type(e).__name__
    try:
        ct = (r.headers.get('Content-Type') or '').lower()
        head = r.read(16)
        r.close()
    except Exception as e:
        return False, type(e).__name__
    audio = ('audio' in ct and 'mpegurl' not in ct) or head[:3] == b'ID3' or \
            head[:2] in (b'\xff\xfb', b'\xff\xf3', b'\xff\xf2', b'\xff\xf1')
    if not audio:
        return False, ct or 'no content-type'
    return True, ct or 'ok'


def clip_utf8(s, n):
    b = s.encode('utf-8')
    return s if len(b) <= n else b[:n].decode('utf-8', 'ignore')


def push(items, host, group, code='', delay=0.15):
    """把筛出来的直接写进机器分组（跟 import_local.py 一套接口）"""
    import time, urllib.parse

    def api(url, data=None, token=None):
        body = urllib.parse.urlencode(data).encode() if data is not None else None
        req = urllib.request.Request(url, data=body)
        if token:
            req.add_header('X-Token', token)
        if body is not None:
            req.add_header('Content-Type', 'application/x-www-form-urlencoded')
        op = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # 内网直连，别走代理
        with op.open(req, timeout=12) as r:
            return json.loads(r.read().decode('utf-8', 'replace'))

    dev = 'http://' + host
    try:
        r = api(dev + '/api/auth' + (('?code=' + code) if code else ''))
    except Exception as e:
        print('连不上机器：%s' % e)
        return 1
    if r.get('locked') and not r.get('token'):
        print('机器开了配对码，屏上有 6 位数字，用 --code= 传进来。')
        return 1
    tok = r.get('token') or ''
    groups = api(dev + '/api/stations', token=tok).get('groups') or []
    g = next((x['i'] for x in groups if x.get('n') == group), None)
    if g is None:
        api(dev + '/api/groupop', {'op': 'add', 'n': group}, token=tok)
        groups = api(dev + '/api/stations', token=tok).get('groups') or []
        g = next((x['i'] for x in groups if x.get('n') == group), None)
        print('已新建分组「%s」' % group)
    if g is None:
        print('建分组失败')
        return 1

    ok = fail = 0
    for k, i in enumerate(items):
        name = clip_utf8(i['name'], 39)
        try:
            r = api(dev + '/api/add', {'name': name, 'url': i['url'], 'g': g}, token=tok)
            if r.get('err'):
                fail += 1
                print('[%d] %s 失败：%s' % (k + 1, name, r['err']))
            else:
                ok += 1
        except Exception as e:
            fail += 1
            print('[%d] %s 失败：%s' % (k + 1, name, e))
        time.sleep(delay)
    print('导入完成：成功 %d，失败 %d，进的是「%s」。' % (ok, fail, group))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True, help='本地文件或 http 地址')
    ap.add_argument('--out', default='', help='可用清单写到这里（tsv：名字/分组/地址）')
    ap.add_argument('--follow', action='store_true', help='跟 302（部分电台靠跳转发 token）')
    ap.add_argument('--limit', type=int, default=0, help='最多探多少条，0=全部')
    ap.add_argument('--jobs', type=int, default=10)
    ap.add_argument('--timeout', type=float, default=6)
    ap.add_argument('--import-to', dest='imp', default='', help='直接写进机器，填 host[:port]')
    ap.add_argument('--group', default='网络电台', help='配合 --import-to 用，默认「网络电台」')
    ap.add_argument('--code', default='', help='配对码')
    a = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    text = fetch(a.src) if a.src.startswith('http') else open(a.src, encoding='utf-8', errors='replace').read()
    items = parse(text)
    if not items:
        print('没解析出条目 —— 这不像 m3u。')
        return 1

    kinds = collections.Counter(kind(i['url']) for i in items)
    print('条目 %d：' % len(items) + '  '.join('%s=%d' % kv for kv in kinds.most_common()))
    dead = kinds.get('m3u8', 0) + kinds.get('flv', 0) + kinds.get('php', 0) + kinds.get('ipv6', 0)
    print('其中 %d 条是 m3u8/flv/php/IPv6 —— 这台机器结构上就放不了，跳过。' % dead)

    cand = [i for i in items if kind(i['url']) == 'direct']
    if a.limit:
        cand = cand[:a.limit]
    if not cand:
        print('没有可探的直链。')
        return 1
    print('探 %d 条直链（%d 并发）…' % (len(cand), a.jobs))

    ok = []
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(probe, i['url'], a.follow, a.timeout): i for i in cand}
        done = 0
        for f, i in futs.items():
            good, why = f.result()
            done += 1
            if good:
                ok.append(i)
                i['ct'] = why
            sys.stdout.write('\r  已探 %d/%d，可用 %d' % (done, len(cand), len(ok)))
            sys.stdout.flush()
    print()

    if not ok:
        print('一条都没有。')
        return 0
    print('\n可用 %d 条：' % len(ok))
    for i in ok:
        print('  %-24s %-10s %-12s %s' % (i['name'][:24], i['g'][:10], i.get('ct', '')[:12], i['url'][:70]))

    if a.out:
        with open(a.out, 'w', encoding='utf-8', newline='') as f:
            for i in ok:
                f.write('%s\t%s\t%s\n' % (i['name'], i['g'], i['url']))
        print('\n写到 %s（%d 行）' % (a.out, len(ok)))

    if a.imp:
        print('')
        return push(ok, a.imp, a.group, a.code)
    return 0


if __name__ == '__main__':
    sys.exit(main())
