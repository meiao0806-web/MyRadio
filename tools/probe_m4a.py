# -*- coding: utf-8 -*-
"""查 m4a 能不能从中间续播：看 mdat 里有没有 ADTS 同步字 / 能不能靠 stsz 定位。"""
import io, re, os, sys, json, urllib.request

P = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'preset_episodes.h')
s = io.open(P, encoding='utf-8').read()
eb = s[s.find('kEpis[]'):]
eb = eb[:eb.find('\n};')]
epis = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([^"]+)"\s*\}', eb)
blk = s[s.find('kEpisBook[]'):]
blk = blk[:blk.find('};')]
books = [(int(a), int(b), n) for n, a, b in
         re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', blk)]
UA = {'User-Agent': 'okhttp/3.12'}


def fetch(u, a, b):
    rq = urllib.request.Request(u, headers=dict(UA, Range='bytes=%d-%d' % (a, b)))
    r = urllib.request.urlopen(rq, timeout=30)
    return r.read(), r.geturl(), r.headers.get('Content-Range', '')


def be32(d, i):
    return int.from_bytes(d[i:i + 4], 'big')


def parse_head(u):
    """取前 64KB，顺着 box 找 moov/mdat/stsz"""
    d, fin, cr = fetch(u, 0, 65535)
    tot = re.search(r'/(\d+)$', cr)
    total = int(tot.group(1)) if tot else 0
    boxes = []
    i = 0
    while i + 8 <= len(d):
        sz = be32(d, i)
        typ = d[i + 4:i + 8].decode('latin1')
        if sz < 8:
            break
        boxes.append((typ, i, sz))
        if typ in ('moov', 'mdat') and sz > 0:
            pass
        i += sz
    info = {'total': total, 'boxes': [(t, o, z) for t, o, z in boxes]}
    # 找 stsz
    j = d.find(b'stsz')
    if j > 0:
        ver = be32(d, j + 4)
        ssz = be32(d, j + 8)
        cnt = be32(d, j + 12)
        info['stsz'] = {'ver': ver, 'sample_size': ssz, 'count': cnt}
        if ssz == 0:
            info['stsz']['first'] = [be32(d, j + 16 + 4 * k) for k in range(min(5, cnt))]
    # 找 mdhd 时间基准
    k = d.find(b'mdhd')
    if k > 0:
        ver = d[k + 4]
        off = k + 4 + 4
        if ver == 1:
            info['timescale'] = be32(d, off + 16)
        else:
            info['timescale'] = be32(d, off + 8)
    return info, d, fin


def check_mid(u, total, moov_end):
    """mdat 里取中间一小段，看是不是 0xFFF 同步字开头"""
    if total <= 0:
        return None
    mid = total // 2
    d, _, _ = fetch(u, mid, mid + 63)
    head = d[:8].hex()
    sync = [k for k in range(min(4, len(d) - 1)) if d[k] == 0xFF and (d[k + 1] & 0xF0) == 0xF0]
    return {'mid': mid, 'hex': head, 'sync_offsets': sync}


targets = []
for nm in ('三国演义', '水浒传', '官场现形记', '原来是这样？！', '如临大敌', '赵亮：白眉大侠外传'):
    st, n, _ = [x for x in books if x[2] == nm][0]
    targets.append((nm, st, epis[st][1]))

for nm, st, u in targets:
    print('=' * 12, nm)
    try:
        info, d, fin = parse_head(u)
        print('  最终: %s' % fin[:110])
        print('  总长: %d B (%.1f MB)' % (info['total'], info['total'] / 1048576.0))
        print('  box: %s' % [(t, 'len=%d' % z) for t, o, z in info['boxes'][:6]])
        print('  stsz: %s' % info.get('stsz'))
        print('  timescale: %s' % info.get('timescale'))
        mdat = [b for b in info['boxes'] if b[0] == 'mdat']
        r = check_mid(fin, info['total'], 0)
        print('  中段首字节: %s  同步字位置=%s  %s'
              % (r['hex'], r['sync_offsets'],
                 '<- 有 ADTS 头，可从中间扫' if r['sync_offsets'] else '<- 裸块，必须靠 stsz'))
        print('  mdat 偏移: %s' % (mdat[0][1] + 8 if mdat else '不在前 64KB'))
    except Exception as e:
        print('  ERR %s %s' % (type(e).__name__, e))
