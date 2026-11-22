#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""逐个切到「会重启」的有声书台，看设备有没有复位（up 归零 / reset 变）。

用法：python tools/probe_book_crash.py
输出：每台一行的 JSON + 结论表。
"""
import io, json, os, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOK = io.open(os.path.join(ROOT, '_tok.txt'), encoding='utf-8').read().strip()
BASE = os.environ.get('MYRADIO_IP', 'http://10.240.152.129')

# 台号 -> 书名（台号来自 src/preset_stations.h 的 kPreset 下标）
TARGETS = [
    (62,  '上下五千年'),
    (86,  '宇宙真相大探索'),
    (99,  '中国绝密档案'),
    (135, '三国演义'),
    (148, '官场现形记'),
    (221, '鬼吹灯'),
    (246, '如临大敌'),
    (282, '一路高升'),
]

WATCH = float(os.environ.get('WATCH_SEC', '18'))
STEP = 1.5


def _url(path):
    sep = '&' if '?' in path else '?'
    return BASE + path + sep + 't=' + TOK


def get(path):
    with urllib.request.urlopen(_url(path), timeout=6) as r:
        return json.loads(r.read().decode('utf-8'))


def post(path):
    req = urllib.request.Request(_url(path), data=b'')
    with urllib.request.urlopen(req, timeout=6) as r:
        return r.read().decode('utf-8')


def probe(idx, name):
    d0 = get('/api/diag')
    up0 = d0['up']
    post('/api/play?id=%d' % idx)
    samples = []
    t0 = time.time()
    died = False
    while time.time() - t0 < WATCH:
        time.sleep(STEP)
        try:
            d = get('/api/diag')
            a = d['audio']
            samples.append((d['up'], a['dec'], a['sec'], a['sil'], a['kbps'], a['url'][-28:]))
            if d['up'] < up0:
                died = True
                break
        except Exception as e:
            samples.append(('ERR', type(e).__name__, str(e)[:40], '', '', ''))
            died = True
            break
    last = samples[-1] if samples else (0, 0, 0, 0, 0, '')
    first = samples[0] if samples else (0, 0, 0, 0, 0, '')
    res = {
        'idx': idx, 'name': name, 'up0': up0, 'died': died,
        'n': len(samples),
        'dec_delta': (last[1] - first[1]) if isinstance(last[1], int) and isinstance(first[1], int) else None,
        'sec_last': last[2], 'last_url': last[5],
    }
    return res


def main():
    print('设备 %s' % BASE)
    d = get('/api/diag')
    print('固件 %s  已运行 %ss  上次复位=%s' % (d['fw'], d['up'], d['reset']))
    rows = []
    for idx, name in TARGETS:
        print('\n>>> %d %s' % (idx, name), flush=True)
        try:
            r = probe(idx, name)
        except Exception as e:
            r = {'idx': idx, 'name': name, 'died': True, 'err': '%s %s' % (type(e).__name__, e)}
        rows.append(r)
        print('    ' + json.dumps(r, ensure_ascii=False), flush=True)

    print('\n=== 结论 ===')
    for r in rows:
        status = '重启/失联' if r.get('died') else '存活'
        print('%-14s %-8s decΔ=%-6s sec=%s' % (r['name'], status, r.get('dec_delta'), r.get('sec_last')))
    bad = [r['idx'] for r in rows if r.get('died')]
    print('复现重启的台：', bad)
    if bad:
        # 复位原因
        d = get('/api/diag')
        print('复位原因：', d['reset'])


if __name__ == '__main__':
    main()
