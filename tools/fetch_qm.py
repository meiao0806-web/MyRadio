#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""我听评书网 / 全民听书（qmtsw.com）-> tools/有声书源-评书.json

站内结构（2026-11-01 实测，纯静态，不用登录、不用签名、不用浏览器）：
    /book/<分类>.html / /book/<分类>-<页>.html   分类列表
    /show/<书号>.html                            书籍详情，**章节列表一次性给全**（不用翻页）
                                                链接形如 /play/<书号>-0-<i>.html，i 从 0 开始
    /play/<书号>-0-<i>.html                      单集，页面里 `var now="<音频直链>"`

⚠️ 音频域名有两代：
    · 新书  → aod.cos.tx.xmcdn.com  …… 实测 206 + audio/mp4，能下
    · 老书  → audio.xmcdn.com       …… 字节 CDN 一律 403（连真浏览器也 403），是死链
    所以逐集要校验域名，死链直接跳过并计数，别把 403 地址写进台表。

搜索 search.php 有「系统安全验证」拦着，找书要么用 --sid 直接给书号，
要么先跑 tools/_q3.py 建分类索引再本地匹配。

用法：
    python tools/fetch_qm.py --sid=8945 --group=悬疑 --name=鬼吹灯
    python tools/fetch_qm.py --sid=8945 --plan      # 只报集数和域名分布
"""
import sys, os, re, json, time, ssl, urllib.request, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, '有声书源-评书.json')

UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')
CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE
PH = {'http': os.environ.get('http_proxy', ''), 'https': os.environ.get('https_proxy', '')}
OP = urllib.request.build_opener(urllib.request.ProxyHandler(PH),
                                 urllib.request.HTTPSHandler(context=CTX))
BASE = 'https://www.qmtsw.com'
GAP = float(os.environ.get('GAP', 0.30))
DEAD_HOST = 'audio.xmcdn.com'      # 这批地址整体 403，别用


def get(u, referer=None, tries=3):
    h = {'User-Agent': UA, 'Accept-Encoding': 'identity'}
    if referer:
        h['Referer'] = referer
    for k in range(tries):
        try:
            r = OP.open(urllib.request.Request(u, headers=h), timeout=25)
            return r.read().decode('utf-8', 'replace')
        except Exception:
            time.sleep(1.0 * (k + 1))
    return ''


def chapters(sid, show_html):
    """从详情页取 [(集号, 标题)]，去重保序"""
    out, seen = [], set()
    for m in re.finditer(r'href="/play/%s-0-(\d+)\.html"[^>]*>([^<]{1,30})</a>' % sid, show_html or ''):
        i = int(m.group(1)); t = m.group(2).strip()
        if i in seen or t == '开始收听':
            continue
        seen.add(i)
        out.append((i, t))
    return out


def audio_of(sid, i):
    h = get('%s/play/%s-0-%d.html' % (BASE, sid, i), referer='%s/show/%s.html' % (BASE, sid))
    m = re.search(r'var now="([^"]+)"', h or '')
    t = re.search(r'<h1[^>]*>(.*?)</h1>', h or '', re.S)
    title = re.sub(r'<[^>]+>', '', t.group(1)).strip() if t else ''
    return (m.group(1) if m else ''), title


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1); kv[k] = v
        elif a.startswith('--'):
            kv[a[2:]] = True
    sid = str(kv.get('sid') or '').strip()
    if not sid:
        print('用法：python tools/fetch_qm.py --sid=8945 [--group=悬疑] [--name=鬼吹灯] [--plan]')
        return
    group = kv.get('group') or '评书'
    name = kv.get('name') or ''
    plan = bool(kv.get('plan'))

    show = get('%s/show/%s.html' % (BASE, sid))
    if not show:
        print('详情页取不到（书号 %s）' % sid)
        return
    if not name:
        m = re.search(r'<title>(.*?)(?:有声小说|_听书)', show, re.S)
        t = m.group(1) if m else ''
        t2 = re.search(r'<h1[^>]*>(.*?)</h1>', show, re.S)
        name = (re.sub(r'<[^>]+>', '', t2.group(1)).strip() if t2 else t).strip()
    chs = chapters(sid, show)
    print('《%s》书号 %s：%d 集' % (name, sid, len(chs)))

    cache = os.path.join(HERE, '.qm_cache_%s.json' % sid)
    done = {}
    if os.path.exists(cache):
        try:
            done = json.load(open(cache, encoding='utf-8'))
        except Exception:
            done = {}

    items, dead, fail = [], 0, 0
    hosts = collections.Counter()
    t0 = time.time()
    for n, (i, t) in enumerate(chs, 1):
        rec = done.get(str(i)) or {}
        if not rec.get('url'):
            url, tt = audio_of(sid, i)
            if url:
                rec = {'url': url, 'title': tt or t}
                done[str(i)] = rec
            time.sleep(GAP)
        u = rec.get('url') or ''
        if u:
            hosts[re.sub(r'^https?://([^/]+)/.*$', r'\1', u)] += 1
            if DEAD_HOST in u:
                dead += 1
            else:
                items.append({'title': rec.get('title') or t, 'url': u})
        else:
            fail += 1
        if n % 25 == 0 or n == len(chs):
            el = time.time() - t0
            print('  %d/%d  可用 %d 死链 %d 取不到 %d  (%.2fs/集)'
                  % (n, len(chs), len(items), dead, fail, el / n))
            json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

    json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('\n域名分布:', dict(hosts))
    print('可用 %d / %d（死链 %d，取不到 %d）' % (len(items), len(chs), dead, fail))
    if plan:
        return
    if not items:
        print('没有可用集，未写文件')
        return

    old = []
    if os.path.exists(OUT):
        try:
            old = [b for b in json.load(open(OUT, encoding='utf-8')) if str(b.get('sid')) != sid]
        except Exception:
            old = []
    old.append({'group': group, 'name': name, 'sid': sid, 'source': 'qmtsw', 'items': items})
    json.dump(old, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('写出 %s：%d 部书 / 本书 %d 集'
          % (os.path.relpath(OUT, ROOT), len(old), len(items)))


if __name__ == '__main__':
    main()
