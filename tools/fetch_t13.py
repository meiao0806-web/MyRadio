#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""13 听书网 -> tools/有声书源-听书网.json（全量集数，纯 Python 通道）

为什么不是「一集开一个页面」：
    站点的音频接口 POST /api/mapi/play 要两边东西：
      · nid / cid —— 两个 8 位 base62 的 token，只出现在播放页的 <meta name="_b">/_p 里；
      · sc / sp   —— 页面里 51.la SDK 现算的签名头。
    而实测这些 token 在同一本书里是**连号**的（…Gb, Gc, Gd, Ge … 逐集 +1，就是 base62 加一），
    所以：先开一页拿到第一个 token，之后本地做 +1 推算，直接打接口；
    接口回非 200 就说明推算跑偏（书与书之间会跳号），这时才回页面取一次真 token 纠偏。
    这样每集只花一次接口请求（~0.3s），而不是一次整页加载（~2s+，还会被限流）。

为什么不用浏览器抓页（1.10.0 的做法）：
    真 Chrome 连续开 4 个页面就被回「请求过于频繁」，而纯 Python 连续 20 页都没事 ——
    限流只针对浏览器特征。所以这里全部用 Python，浏览器只用来取一次 sc/sp 签名。

用法：
    python tools/fetch_t13.py                # 全量（断点续跑，缓存 tools/.t13cache）
    python tools/fetch_t13.py --only=24417   # 只抓一本
    python tools/fetch_t13.py --plan         # 只统计每本书多少集
    python tools/fetch_t13.py --force        # 忽略缓存重抓
"""
import sys
import os
import re
import json
import time
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import _t13_py as T          # 取页 + 过 __51guid__ 挑战（已强制 IPv4）

SRC = os.path.join(HERE, '听书网源.json')
OUT = os.path.join(HERE, '有声书源-听书网.json')
CACHE = os.path.join(HERE, '.t13cache')
SIGF = os.path.join(HERE, '.t13sig.json')
NODE = os.environ.get('NODE') or r'C:\Users\lenovo\.workbuddy\binaries\node\versions\22.22.2-3\node.exe'
SIGJS = os.path.join(HERE, '_sig.js')
HOST = 'https://www.ting13.cc'

GAP_PAGE = float(os.environ.get('GAP_PAGE', 1.0))   # 章节页之间
GAP_EP = float(os.environ.get('GAP_EP', 0.30))      # 每集接口之间
B62 = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'

args = sys.argv[1:]
ONLY = ''
for a in args:
    if a.startswith('--only='):
        ONLY = a.split('=', 1)[1]
PLAN = '--plan' in args
FORCE = '--force' in args


# ---------------- base62 走一步 ----------------

def b62_inc(tok):
    """8 位 base62 加 1（进位），失败返回 ''。"""
    if not tok or len(tok) != 8:
        return ''
    d = [B62.index(c) if c in B62 else -1 for c in tok]
    if min(d) < 0:
        return ''
    i = 7
    while i >= 0:
        d[i] += 1
        if d[i] < 62:
            break
        d[i] = 0
        i -= 1
    if i < 0:
        return ''
    return ''.join(B62[x] for x in d)


# ---------------- 签名 ----------------

def load_sig():
    if not os.path.exists(SIGF):
        return None
    try:
        s = json.load(open(SIGF, encoding='utf-8'))
    except Exception:
        return None
    if not s.get('sc') or not s.get('sp'):
        return None
    return s


def refresh_sig(reason=''):
    print('  [签名] %s，重取一次…' % (reason or '过期/无效'))
    try:
        env = dict(os.environ)
        env.setdefault('CDP_PORT', '9511')
        r = subprocess.run([NODE, SIGJS], cwd=HERE, env=env, capture_output=True, timeout=180)
        out = (r.stdout or b'').decode('utf-8', 'replace').strip()
        if out:
            print('  [签名]', out.splitlines()[-1][:120])
    except Exception as e:
        print('  [签名] 重取失败：%s' % str(e)[:80])
    return load_sig()


# ---------------- 接口 ----------------

class Api:
    def __init__(self):
        self.sig = load_sig()
        self.bad = 0

    def cookie_header(self):
        c = {}
        if self.sig:
            for kv in self.sig.get('cookies') or []:
                if kv.get('name'):
                    c[kv['name']] = kv['value']
        c.update(T.COOKIE)
        return '; '.join('%s=%s' % (k, v) for k, v in c.items())

    def play(self, nid, tok, depth=0):
        """返回 (status, name, url)。status=0 表示网络异常。"""
        if not self.sig:
            self.sig = refresh_sig('没有签名文件')
            if not self.sig:
                return 0, '', ''
        H = {
            'User-Agent': self.sig.get('ua') or 'Mozilla/5.0',
            'sc': self.sig['sc'],
            'sp': self.sig['sp'],
            'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8',
            'Accept': 'application/json, text/javascript, */*; q=0.01',
            'X-Requested-With': 'XMLHttpRequest',
            'Referer': HOST + '/play/%s_1_1.html' % nid,
            'Cookie': self.cookie_header(),
        }
        import urllib.request, urllib.parse
        body = urllib.parse.urlencode({'nid': nid, 'cid': tok, 'sort': 'read'}).encode()
        req = urllib.request.Request(HOST + '/api/mapi/play', data=body, headers=H)
        try:
            r = T.OPENER.open(req, timeout=20)
            for sc in (r.headers.get_all('Set-Cookie') or []):
                m = re.match(r'([^=]+)=([^;]*)', sc)
                if m:
                    T.COOKIE[m.group(1)] = urllib.parse.unquote(m.group(2))
            txt = r.read().decode('utf-8', 'replace')
        except Exception as e:
            return 0, '', ''
        if 'var reversed = "' in txt and len(txt) < 6000 and depth < 3:
            T.solve(txt)
            time.sleep(0.3)
            return self.play(nid, tok, depth + 1)
        try:
            j = json.loads(txt)
        except Exception:
            return -2, '', ''
        st = j.get('status')
        url = j.get('url') or ''
        name = j.get('name') or ''
        # 405/404 时会塞一条随机 xncdm 兜底音频，绝不能当结果用
        if st == 200 and 'mp3hs.ting13.top' in url:
            self.bad = 0
            return 200, name, url
        return st or -3, name, ''


# ---------------- 章节 ----------------

def album_title(html):
    m = re.search(r'<title>(.*?)</title>', html, re.S)
    t = m.group(1) if m else ''
    t = re.sub(r'_免费听书.*$', '', t)
    t = re.sub(r'_演播[:：].*$', '', t)
    return t.replace('有声小说', '').strip()


def dirs_url(html):
    m = re.search(r'href="(/tingdirs/[^"?]+)', html or '')
    return HOST + m.group(1) if m else ''


def page_ranges(html):
    out = []
    for m in re.finditer(r'class="chapter-item[^"]*"[^>]*>\s*<a[^>]*>\s*(\d+)\s*~\s*(\d+)\s*</a>', html or ''):
        out.append((int(m.group(1)), int(m.group(2))))
    return out


def chapters_of(html, bid):
    blk = re.search(r'<div class="playlist"\s+id="playlist"[\s\S]*?</ul>', html or '')
    seg = blk.group(0) if blk else (html or '')
    out, seen = [], set()
    for m in re.finditer(r'<a[^>]*href="(/play/' + re.escape(bid) + r'_[^"]+)"[^>]*>([\s\S]*?)</a>', seg):
        t = re.sub(r'<[^>]+>', '', m.group(2))
        t = re.sub(r'\s+', ' ', t).strip()
        cid = (re.search(r'_1_(\d+)\.html', m.group(1)) or [None, ''])[1]
        if not t or not cid or cid in seen:
            continue
        seen.add(cid)
        out.append({'cid': cid, 't': t})
    return out


def ep_index(title):
    s = title or ''
    m = re.search(r'第\s*0*(\d{1,4})\s*[章集回话期]', s)
    if m:
        return int(m.group(1))
    m = re.match(r'\s*0*(\d{1,4})\s*[_、.．\s]', s)
    if m:
        return int(m.group(1))
    m = re.match(r'\s*0*(\d{1,4})\s*$', s)
    return int(m.group(1)) if m else 0


BLOCK = {'n': 0, 'until': 0.0}


def blocked():
    """连着几页都是 520，说明整条出口 IP 被站点挡了，只能等它自己放。"""
    BLOCK['n'] += 1
    if BLOCK['n'] < 3:
        return
    wait = min(900, 120 * (BLOCK['n'] - 2))
    print('  [封锁] 站点连 %d 页不通（520），等 %d 秒再试（这是按出口 IP 挡的）'
          % (BLOCK['n'], wait))
    time.sleep(wait)


def get_html(url, referer=None, tries=4):
    for k in range(tries):
        st, html = T.get(url, referer=referer)
        if st == 200 and len(html) > 3000:
            BLOCK['n'] = 0
            return html
        blocked()
        time.sleep(1.5 * (k + 1))
    return ''


def fetch_chapters(bk):
    album = get_html('%s/youshengxiaoshuo/%s/' % (HOST, bk['id']))
    if not album:
        return None
    du = dirs_url(album)
    if not du:
        return None
    p1 = get_html(du, referer=HOST + '/youshengxiaoshuo/%s/' % bk['id'])
    if not p1:
        return None
    rg = page_ranges(p1)
    total = rg[-1][1] if rg else 0
    pages = len(rg) or 1
    got = {}

    def add(html):
        for c in chapters_of(html, bk['id']):
            got.setdefault(c['cid'], c['t'])

    add(p1)
    for k in range(2, pages + 1):
        time.sleep(GAP_PAGE)
        h = get_html('%s?page=%d&sort=asc' % (du, k), referer=du)
        if not h:
            print('    第 %d/%d 页取不到' % (k, pages))
            continue
        add(h)
        if k % 5 == 0:
            print('    章节 %d/%d 页，累计 %d/%s' % (k, pages, len(got), total or '?'))
    lst = [{'cid': c, 't': t} for c, t in got.items()]
    if sum(1 for x in lst if ep_index(x['t']) > 0) >= len(lst) * 0.8:
        lst.sort(key=lambda x: ep_index(x['t']) or 9 ** 9)
    else:
        lst.sort(key=lambda x: int(x['cid']))
    return {'name': album_title(album) or bk['name'], 'total': total, 'chapters': lst}


# ---------------- 主流程 ----------------

def main():
    books = json.load(open(SRC, encoding='utf-8'))
    if ONLY:
        books = [b for b in books if b['id'] == ONLY]
    if not books:
        print('清单里没有匹配的书')
        return
    os.makedirs(CACHE, exist_ok=True)
    api = Api()
    if not api.sig:
        print('拿不到签名，先跑 tools/_sig.js')
        return

    plan = []
    for bk in books:
        cf = os.path.join(CACHE, bk['id'] + '.json')
        cache = {'chapters': [], 'eps': []}
        if os.path.exists(cf) and not FORCE:
            try:
                cache = json.load(open(cf, encoding='utf-8'))
            except Exception:
                pass
        cache.setdefault('chapters', [])
        cache.setdefault('eps', [])

        if not cache['chapters']:
            print('[%s] 抓章节表…' % bk['name'])
            r = fetch_chapters(bk)
            if not r:
                print('[%s] 章节表取不到，跳过' % bk['name'])
                continue
            cache['chapters'] = r['chapters']
            cache['name'] = r['name']
            cache['total'] = r['total']
            cache['eps'] = []
            json.dump(cache, open(cf, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
            print('[%s] 章节 %d 条（站点标注 %d 集）' % (bk['name'], len(r['chapters']), r['total']))

        plan.append((bk['name'], len(cache['chapters'])))
        if PLAN:
            continue

        # 每集：本地推算 token → 接口自检 → 不对就回页面取真 token
        chs = cache['chapters']
        eps = cache['eps']
        eps = (eps + [None] * len(chs))[:len(chs)]
        tok = ''
        for e in eps:                       # 找上一个已知 token 做起点
            if e and e.get('tok'):
                tok = e['tok']
        need_page = {}
        done = sum(1 for e in eps if e and e.get('url'))
        t0 = time.time()
        for i, ch in enumerate(chs):
            if eps[i] and eps[i].get('url'):
                tok = eps[i].get('tok') or tok
                continue

            guess = b62_inc(tok) if tok else ''
            rec = {'cid': ch['cid'], 't': ch['t'], 'tok': '', 'url': '', 'name': '', 'src': ''}
            if guess:
                st, nm, url = api.play(bk['id'], guess)
                if st == 200:
                    rec.update(tok=guess, url=url, name=nm, src='calc')
            if not rec['url']:
                # 推算没中：回页面拿真 token
                html = get_html('%s/play/%s_1_%s.html' % (HOST, bk['id'], ch['cid']),
                                referer=HOST + '/youshengxiaoshuo/%s/' % bk['id'])
                mp = re.search(r'<meta name="_p"\s*content="([^"]+)"', html or '')
                if not mp:
                    eps[i] = rec
                    json.dump(cache, open(cf, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
                    print('  FAIL %s %d/%d 页面取不到 token' % (bk['name'], i + 1, len(chs)))
                    continue
                real = mp.group(1)
                st, nm, url = api.play(bk['id'], real)
                rec.update(tok=real, url=url, name=nm, src='page' if url else 'fail')
                tok = real
            else:
                tok = rec['tok']

            eps[i] = rec
            if rec['url']:
                done += 1
            json.dump(cache, open(cf, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

            if (i + 1) % 25 == 0 or not rec['url']:
                el = time.time() - t0
                rate = el / max(1, i + 1)
                print('  %s %s %d/%d  已完成 %d  (%.2fs/集, 余 ~%.0f 分)'
                      % ('OK ' if rec['url'] else 'BAD', bk['name'], i + 1, len(chs), done,
                         rate, rate * (len(chs) - i - 1) / 60))
            time.sleep(GAP_EP)

        ok = sum(1 for e in eps if e and e.get('url'))
        print('[%s] 完成 %d/%d 集' % (bk['name'], ok, len(chs)))

    if PLAN:
        print('\n%-26s 集数' % '书名')
        s = 0
        for n, c in plan:
            print('%-26s %5d' % (n, c))
            s += c
        print('\n合计 %d 部 / %d 集' % (len(plan), s))
        return

    # 合成输出
    out = []
    for bk in json.load(open(SRC, encoding='utf-8')):
        cf = os.path.join(CACHE, bk['id'] + '.json')
        if not os.path.exists(cf):
            continue
        c = json.load(open(cf, encoding='utf-8'))
        items = [{'title': e.get('name') or e.get('t') or '', 'url': e['url']}
                 for e in (c.get('eps') or []) if e and e.get('url')]
        if items:
            out.append({'group': bk['group'], 'name': bk['name'], 'items': items})
    if out:
        json.dump(out, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        n = sum(len(b['items']) for b in out)
        print('\n写出 %s：%d 部书 / %d 集' % (os.path.relpath(OUT, ROOT), len(out), n))
    else:
        print('\n没有解析出任何一集，未写文件')


if __name__ == '__main__':
    main()
