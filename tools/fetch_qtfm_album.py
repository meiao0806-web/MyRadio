#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""蜻蜓 FM「点播专辑」（一个 /channels/<id> 链接）-> tools/有声书源-蜻蜓.json

和 fetch_qtfm.py 的区别：
    fetch_qtfm.py        抓的是**直播电台**（31 个地区的 FM 台），台号给台表，地址是
                         http://lhttp.qtfm.cn/live/<台号>/64k.mp3
    fetch_qtfm_album.py  抓的是**点播专辑**（评书 / 有声小说 / 播客），一个专辑 = 一部书，
                         每集一条音频直链，写进集表（preset_episodes.h）

用法：
    python tools/fetch_qtfm_album.py --channel=140696 --group=科普 --name=原来是这样？！
    python tools/fetch_qtfm_album.py --channel=140696 --plan      # 只列集数，不写文件

接口（2026-11-01 实测）：
    ① 目录：https://www.qtfm.cn/channels/<id>[/<页>]  —— 服务端渲染，每页 40 条，
       总集数写在 class="total"(N个) 里。翻到 total 页为止。
    ② 音频：https://audio.qtfm.cn/audiostream/redirect/<专辑号>/<节目号>?sign=MD5-HMAC
       sign = HMAC_MD5(key="7l8CZ)SgZgM_bkrw", "/audiostream/redirect/<专辑号>/<节目号>")
       （key 抄自官网 main.<hash>.js，公开在页面里；实测**不带任何参数也能过**，
         t/device_id 那些只影响 CDN 节点，所以地址就写成 ...?sign=<32位>，96 字节，
         集表里 600 多集也塞得下。）
       这个地址会 302 到 https://tchwod-sign.qtfm.cn/m4a/xxxx_24.m4a?auth_key=…，
       auth_key 是**每次现签的**，所以固件里只能存跳转地址、不能存 CDN 地址。

⚠️ 三个坑（改这个脚本前先看）：
    1. 音频是 **m4a（AAC in MP4）**，但 CDN 回的 Content-Type 是 `audio/mpeg`。
       ESP32-audioI2S 只按 Content-Type 判格式 ⇒ 会当 mp3 解、同步字找不到、整集没声音。
       1.16.0 起仓库自带打过补丁的 lib/ESP32-audioI2S（见 platformio.ini），靠 URL 里的
       ".m4a" 把格式纠回来；**升级音频库时这两处补丁要重新贴**。
    2. 每集要「真去看一眼」：接口 302 才算在架。付费集/下架集直接 403/404，
       写进台表就是「这个台没有声音」（机器上会 20 秒后自动跳过）。
    3. 列表顺序各专辑不一样：播客（原来是这样？！）是**倒序**（第 1 页是最后一集），
       评书是正序。所以能抠出集号就按集号升序排，抠不出就按页面顺序。
"""
import hashlib
import hmac
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, '有声书源-蜻蜓.json')

UA = {'User-Agent': ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
                     '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')}
SIGN_KEY = b'7l8CZ)SgZgM_bkrw'       # 官网前端明文，见 main.<hash>.js
AUDIO_HOST = 'https://audio.qtfm.cn'
PER_PAGE = 40
GAP = float(os.environ.get('GAP', 0.18))


def get(url, tries=3, allow_redirect=False):
    """抓一页 HTML。默认**不跟随 302**（探测用）。"""
    last = None
    for k in range(tries):
        try:
            req = urllib.request.Request(url, headers=UA)
            if allow_redirect:
                with urllib.request.urlopen(req, timeout=25) as r:
                    return r.read().decode('utf-8', 'replace')
            opener = urllib.request.build_opener(NoRedirect)
            with opener.open(req, timeout=25) as r:
                return r.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            return e                        # 交给调用方看状态码
        except Exception as e:
            last = e
            time.sleep(1.0 * (k + 1))
    raise last


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, hdrs, newurl):
        return None


def sign_path(path):
    return hmac.new(SIGN_KEY, path.encode(), hashlib.md5).hexdigest()


def ep_url(cid, pid):
    path = '/audiostream/redirect/%s/%s' % (cid, pid)
    return AUDIO_HOST + path + '?sign=' + sign_path(path)


def alive(url):
    """302 = 在架可播（后面那一跳是 CDN）；403/404 = 收费或下架。"""
    try:
        opener = urllib.request.build_opener(NoRedirect)
        r = opener.open(urllib.request.Request(url, headers=UA), timeout=20)
        return r.status == 200 or r.status == 302
    except urllib.error.HTTPError as e:
        return e.code in (301, 302)
    except Exception:
        return False


def fetch_index(cid):
    """翻完所有目录页 -> (总集数, [(节目号, 标题), ...])"""
    rows, total = [], 0
    page = 1
    while page <= 40:
        url = 'https://www.qtfm.cn/channels/%s' % cid + ('' if page == 1 else '/%d' % page)
        h = get(url, allow_redirect=True)
        if not isinstance(h, str):
            print('  [页 %d] HTTP %s，停' % (page, h.code))
            break
        if page == 1:
            m = re.search(r'class="total">\(<!-- -->(\d+)<!-- -->个\)', h)
            total = int(m.group(1)) if m else 0
        got = re.findall(r'href="/channels/%s/programs/(\d+)"><p[^>]*title="([^"]*)"' % cid, h)
        if not got:
            break
        before = len(rows)
        seen = set(p for p, _ in rows)
        for pid, title in got:
            if pid in seen:
                continue
            seen.add(pid)
            rows.append((pid, title.strip()))
        print('  [页 %d] +%d 条（累计 %d / 共 %d）' % (page, len(rows) - before, len(rows), total or 0))
        if total and len(rows) >= total:
            break
        if len(rows) == before:
            break
        page += 1
        time.sleep(GAP)
    return total or len(rows), rows


NUM_PATTERNS = [r'第\s*(\d{1,4})\s*[回集章话期篇]', r'^\s*(\d{1,4})\s*[：:．.、]', r'(\d{3})\s*$']


def ep_key(title):
    for pat in NUM_PATTERNS:
        m = re.search(pat, title)
        if m:
            return int(m.group(1))
    if '序言' in title or '序章' in title:
        return 0
    return None


def order_rows(rows):
    """目录是正序还是倒序，各专辑不一样（播客「原来是这样？！」是倒序、评书是正序）。

    判据：能抠出集号的那部分，第一个比最后一个大 ⇒ 整份目录是倒序，**整份反转**就行。
    不能按集号重排 —— 57 条【特刊】【番外】没有集号，重排会把它们全挤到一头，
    反转才能让它们在时间线上待在原来的位置。
    """
    keys = [ep_key(t) for _, t in rows]
    got = [k for k in keys if k is not None]
    if len(got) >= max(3, len(rows) // 2) and got[0] > got[-1]:
        return list(reversed(rows)), True
    return rows, False


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1)
            kv[k] = v
        elif a.startswith('--'):
            kv[a[2:]] = True
    cid = str(kv.get('channel') or '').strip()
    if not cid:
        print('用法：python tools/fetch_qtfm_album.py --channel=140696 [--group=科普] [--name=书名] [--plan]')
        return
    group = kv.get('group') or '评书'
    name = kv.get('name') or ('蜻蜓专辑%s' % cid)
    plan = bool(kv.get('plan'))

    print('专辑 %s，抓目录…' % cid)
    total, rows = fetch_index(cid)
    rows, sorted_ok = order_rows(rows)
    print('目录 %d 条（站点写 %d）%s' % (len(rows), total, '，已按集号升序' if sorted_ok else '，保持页面顺序'))
    if not rows:
        print('一条都没抓到')
        return
    print('  首条：%s' % rows[0][1][:40])
    print('  末条：%s' % rows[-1][1][:40])
    if plan:
        return

    cache = os.path.join(HERE, '.qtfm_cache_%s.json' % cid)
    done = {}
    if os.path.exists(cache):
        try:
            done = json.load(open(cache, encoding='utf-8'))
        except Exception:
            done = {}

    items, dead, t0 = [], [], time.time()
    for n, (pid, title) in enumerate(rows, 1):
        rec = done.get(pid)
        if rec is None:
            u = ep_url(cid, pid)
            ok = alive(u)
            rec = {'url': u, 'title': title, 'ok': ok}
            done[pid] = rec
            time.sleep(GAP)
        if rec['ok']:
            items.append({'title': rec['title'], 'url': rec['url']})
        else:
            dead.append(rec['title'])
        if n % 40 == 0 or n == len(rows):
            print('  %d/%d  死链 %d  (%.2fs/集)' % (n, len(rows), len(dead), (time.time() - t0) / n))
            json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

    json.dump(done, open(cache, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('\n《%s》可用 %d 集 / 目录 %d' % (name, len(items), len(rows)))
    if dead:
        print('  跳过 %d 集（收费/下架）：%s' % (len(dead), '、'.join(t[:12] for t in dead[:5])))
    if not items:
        print('没有可用集，未写文件')
        return

    old = []
    if os.path.exists(OUT):
        try:
            old = [b for b in json.load(open(OUT, encoding='utf-8'))
                   if str(b.get('channel')) != cid]
        except Exception:
            old = []
    old.append({'group': group, 'name': name, 'channel': cid, 'total': total, 'items': items})
    json.dump(old, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('写出 %s：%d 部书 / 本书 %d 集'
          % (os.path.relpath(OUT, ROOT), len(old), len(items)))


if __name__ == '__main__':
    main()
