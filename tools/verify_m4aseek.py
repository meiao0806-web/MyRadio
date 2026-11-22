# -*- coding: utf-8 -*-
"""验 m4a 续播的定位算法（1.46.52）。

固件里的做法（src/m4aseek.cpp）：
  取文件头 256KB → moov → trak → mdia → mdhd(timescale) / minf → stbl → stsz
  帧号 = 秒 × timescale ÷ 1024
  字节 = mdat 负载起点 + Σ stsz[0..帧号-1]

这里做三件事：
  A. 把 C++ 那段逻辑**逐行搬过来**（包括「先只读表头、再累加」两步走），
     喂真文件的前 256KB，看算到哪。
  B. 另写一份**独立实现**（递归收集所有 box、把 stsz 整表读进 list，
     直接累加索引），两份结果必须一致 —— 一份写错另一份能兜住。
  C. 结构判据：
       · absPos 必须落在 mdat 里
       · 反推帧号（累加到 >= absPos 的第一帧）必须正好等于目标帧 ⇒ 确实对齐了边界
       · 帧号换算回秒 ≈ 请求的秒（误差 < 1 秒）
       · 抽样多个秒，absPos 必须单调递增
"""
import io, os, re, sys, urllib.request

P = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'preset_episodes.h')
CAP = 256 * 1024
UA = {'User-Agent': 'MyRadio'}
oks, fails = 0, 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails += 1
        print('  FAIL ' + msg + (('  << ' + str(extra)) if extra else ''))


def be32(b, i):
    return int.from_bytes(b[i:i + 4], 'big')


def fetch_head(url, cap=CAP):
    rq = urllib.request.Request(url, headers=dict(UA, Range='bytes=0-%d' % (cap - 1)))
    r = urllib.request.urlopen(rq, timeout=40)
    data = r.read()
    total = 0
    cr = r.headers.get('Content-Range') or ''
    m = re.search(r'/(\d+)$', cr)
    if m:
        total = int(m.group(1))
    elif r.headers.get('Content-Length'):
        total = int(r.headers['Content-Length'])
    return data, total, r.geturl()


# ---------------------------------------------------------------- A：搬 C++ 逻辑
def find_child(b, begin, end, name):
    p = begin
    while p + 8 <= end:
        sz = be32(b, p)
        if sz == 1:
            return None
        real = (end - p) if sz == 0 else sz
        if real < 8 or p + real > end:
            return None
        if b[p + 4:p + 8] == name.encode('ascii'):
            return (p, real, p + 8)
        p += real
    return None


def mdhd_timescale(b, box):
    off, sz, body = box
    if body + 4 > off + sz:
        return 0
    ver = b[body]
    t = body + 4 + (16 if ver == 1 else 8)
    if t + 4 > off + sz:
        return 0
    return be32(b, t)


def stsz_sum(b, stsz, frames):
    off, sz, body = stsz
    if body + 12 > off + sz:
        return None
    ss = be32(b, body + 4)
    cnt = be32(b, body + 8)
    if cnt == 0:
        return None
    if ss != 0:
        f = min(frames, cnt) if frames else 0
        return (ss * f, ss, cnt)
    ent = body + 12
    if frames and ent + frames * 4 > len(b):
        return None
    s = 0
    for i in range(min(frames, cnt)):
        s += be32(b, ent + i * 4)
    return (s, ss, cnt)


def cpp_impl(b, sec):
    n = len(b)
    moov = mdat = None
    p = 0
    while p + 8 <= n:
        sz = be32(b, p)
        if sz == 1:
            break
        real = (n - p) if sz == 0 else sz
        if real < 8:
            break
        t = b[p + 4:p + 8]
        if t == b'moov':
            moov = (p, real, p + 8)
        if t == b'mdat':
            mdat = (p, real, p + 8)
            break
        if p + real > n:
            break
        p += real
    if not moov:
        return None, 'no moov in %dKB' % (n // 1024)

    m_end = min(moov[0] + moov[1], n)
    ts = 0
    frames = 0
    stsz = None
    tp = moov[2]
    while tp + 8 <= m_end:
        trak = find_child(b, tp, m_end, 'trak')
        if not trak:
            break
        mdia = find_child(b, trak[2], trak[0] + trak[1], 'mdia')
        if mdia:
            mdhd = find_child(b, mdia[2], mdia[0] + mdia[1], 'mdhd')
            if mdhd:
                ts = mdhd_timescale(b, mdhd)
            minf = find_child(b, mdia[2], mdia[0] + mdia[1], 'minf')
            if minf:
                stbl = find_child(b, minf[2], minf[0] + minf[1], 'stbl')
                if stbl:
                    s = find_child(b, stbl[2], stbl[0] + stbl[1], 'stsz')
                    if s:
                        r = stsz_sum(b, s, 0)
                        if r:
                            stsz, frames = s, r[2]
        if stsz:
            break
        tp = trak[0] + trak[1]

    if not stsz or not frames:
        return None, 'no stsz (ts=%d)' % ts
    if not ts:
        return None, 'no timescale'

    if not mdat:
        p = m_end
        while p + 8 <= n:
            sz = be32(b, p)
            if sz < 8:
                break
            if b[p + 4:p + 8] == b'mdat':
                mdat = (p, sz, p + 8)
                break
            if p + sz > n:
                break
            p += sz
    if not mdat:
        return None, 'no mdat within %dKB' % (n // 1024)

    target = (sec * ts) // 1024
    if target >= frames:
        target = frames - 1
    r = stsz_sum(b, stsz, target)
    if r is None:
        return None, 'stsz beyond %dKB' % (n // 1024)
    return {
        'absPos': mdat[2] + r[0],
        'dataStart': mdat[2],
        'timescale': ts,
        'frames': frames,
        'frame': target,
    }, None


# ---------------------------------------------------------------- B：独立实现
def indep_impl(b):
    n = len(b)
    boxes = []

    def walk(start, end, depth=0):
        p = start
        while p + 8 <= end:
            sz = be32(b, p)
            if sz == 1 or sz < 8:
                return
            real = sz
            typ = b[p + 4:p + 8].decode('latin1', 'replace')
            boxes.append((typ, p, real, depth))
            if typ in ('moov', 'trak', 'mdia', 'minf', 'stbl', 'udta', 'ilst'):
                walk(p + 8, min(p + real, end), depth + 1)
            if real == 0:
                return
            p += real

    walk(0, n)
    moov = [x for x in boxes if x[0] == 'moov']
    mdhd = [x for x in boxes if x[0] == 'mdhd']
    stsz = [x for x in boxes if x[0] == 'stsz']
    mdat = [x for x in boxes if x[0] == 'mdat']
    if not (moov and mdhd and stsz and mdat):
        return None
    m = mdhd[0]
    ver = b[m[1] + 8]
    ts_off = m[1] + 8 + 4 + (16 if ver == 1 else 8)
    ts = be32(b, ts_off)
    s = stsz[0]
    ss = be32(b, s[1] + 12)
    cnt = be32(b, s[1] + 16)
    lim = min(cnt, 400000)
    sizes = [ss] * lim if ss else [be32(b, s[1] + 20 + i * 4) for i in range(lim)]
    return {
        'dataStart': mdat[0][1] + 8,
        'timescale': ts,
        'frames': cnt,
        'sizes': sizes,
    }


def main():
    global fails
    s = io.open(P, encoding='utf-8').read()
    blk = s[s.find('kEpisBook[]'):]
    blk = blk[:blk.find('};')]
    books = {n: (int(a), int(b)) for n, a, b in
             re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', blk)}
    eb = s[s.find('kEpis[]'):]
    eb = eb[:eb.find('\n};')]
    epis = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([^"]+)"\s*\}', eb)

    for name in ('三国演义', '水浒传', '官场现形记', '原来是这样？！'):
        if name not in books:
            print('!! 书表里没有', name)
            continue
        st, n = books[name]
        url = epis[st][1]
        print('\n================ %s（%d 集）================' % (name, n))
        try:
            b, total, final = fetch_head(url)
        except Exception as e:
            print('  取头失败：%s %s' % (type(e).__name__, e))
            fails += 1
            continue
        print('  文件 %d B（%.1f MB），读到 %d KB' % (total, total / 1048576.0, len(b) // 1024))

        info, err = cpp_impl(b, 0)
        ind = indep_impl(b)
        if not info or not ind:
            print('  !! 解析失败：%s' % (err or '独立实现缺 box'))
            fails += 1
            continue

        print('  timescale=%d  frames=%d  dataStart=%d' %
              (info['timescale'], info['frames'], info['dataStart']))
        ok(info['timescale'] == ind['timescale'], '两份实现 timescale 一致：%d' % info['timescale'],
           '%d vs %d' % (info['timescale'], ind['timescale']))
        ok(info['frames'] == ind['frames'], '两份实现帧数一致：%d' % info['frames'],
           '%d vs %d' % (info['frames'], ind['frames']))
        ok(info['dataStart'] == ind['dataStart'], '两份实现 mdat 起点一致：%d' % info['dataStart'],
           '%d vs %d' % (info['dataStart'], ind['dataStart']))

        dur = ind['frames'] * 1024.0 / ind['timescale']
        print('  整集 ≈ %.1f 秒（%d 分 %d 秒）' % (dur, int(dur // 60), int(dur % 60)))

        secs = sorted(set(x for x in (0, 1, 5, 30, 60, 120,
                                      int(dur * 0.5), max(1, int(dur) - 20)) if 0 <= x < dur))
        prev = -1
        mono = True
        for sec in secs:
            r, e2 = cpp_impl(b, sec)
            if not r:
                ok(False, '第 %ds 算不出位置：%s' % (sec, e2))
                continue
            tgt = (sec * ind['timescale']) // 1024
            if tgt >= ind['frames']:
                tgt = ind['frames'] - 1
            exp = ind['dataStart'] + sum(ind['sizes'][:tgt])
            ok(r['absPos'] == exp,
               '第 %ds → 帧 %d → 字节 %d（与独立实现一致）' % (sec, r['frame'], r['absPos']),
               '差 %d' % (r['absPos'] - exp))
            back = r['frame'] * 1024.0 / ind['timescale']
            ok(abs(back - sec) < 1.0, '  帧号换回来 %.2fs ≈ %ds' % (back, sec))
            ok(ind['dataStart'] <= r['absPos'] < ind['dataStart'] + sum(ind['sizes']),
               '  落在 mdat 内')
            if prev >= 0 and r['absPos'] <= prev:
                mono = False
            prev = r['absPos']
        ok(mono, '字节位置随秒数单调递增')

        r, _ = cpp_impl(b, secs[-1])
        acc = ind['dataStart']
        hit = -1
        for i, szv in enumerate(ind['sizes']):
            if acc >= r['absPos']:
                hit = i
                break
            acc += szv
        ok(hit == r['frame'],
           '反推帧号正是目标帧（%d）—— 落在帧边界上' % hit,
           'hit=%d frame=%d' % (hit, r['frame']))

    print('\n---------------- 汇总 ----------------')
    print('%d / %d 通过' % (oks, oks + fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
