#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""B 站视频 -> 单集 mp3（只取音轨）

用法：
    python tools/bili_audio.py --bvid=BV1BzxE6UE55 --p=3 --out=audio/hlm/002.mp3

为什么需要这个脚本：
    B 站音频是 DASH 分片 MP4（moof/trun），而且 URL 2 小时过期 + 必须带 Referer，
    固件里那套 ESP32-audioI2S 直接放不了（见 CHANGELOG 1.13.0）。
    所以只能在电脑上「先落成普通 mp3，再挂到一个能长期访问的地址」。
"""
import sys, os, json, subprocess, urllib.request, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
def _find_ffmpeg():
    """优先环境变量，其次仓库内 _tools/ffmpeg.exe，最后 PATH 里的 ffmpeg"""
    for c in (os.environ.get('FFMPEG'),
              os.path.join(ROOT, '_tools', 'ffmpeg.exe'),
              os.path.join(ROOT, '_tools', 'ffmpeg')):
        if c and os.path.isfile(c):
            return c
    return 'ffmpeg'


FFMPEG = _find_ffmpeg()

UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')
REF = 'https://www.bilibili.com/'


def api(url):
    req = urllib.request.Request(url, headers={'User-Agent': UA, 'Referer': REF})
    return json.loads(urllib.request.urlopen(req, timeout=25).read().decode('utf-8'))


def main():
    kv = {}
    for a in sys.argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1)
            kv[k] = v
    bvid = kv.get('bvid')
    if not bvid:
        print('用法：python tools/bili_audio.py --bvid=BVxxx --p=3 --out=x.mp3')
        return 1
    page = int(kv.get('p', '1'))
    out = kv.get('out') or os.path.join(ROOT, 'audio', '%s_p%d.mp3' % (bvid, page))
    os.makedirs(os.path.dirname(out), exist_ok=True)

    view = api('https://api.bilibili.com/x/web-interface/view?bvid=%s' % bvid)
    if view.get('code') != 0:
        print('取视频信息失败：', view.get('message'))
        return 1
    d = view['data']
    pages = d.get('pages') or []
    if page < 1 or page > len(pages):
        print('p 越界，这个视频只有 %d 个分 P' % len(pages))
        return 1
    pg = pages[page - 1]
    title = (pg.get('part') or d.get('title') or '').strip()
    print('分 P %d/%d：%s  %d 秒' % (page, len(pages), title, pg.get('duration', 0)))

    pu = api('https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%s&fnval=16&fnver=0&fourk=1'
             % (bvid, pg['cid']))
    if pu.get('code') != 0:
        print('取播放地址失败：', pu.get('message'))
        return 1
    audios = ((pu.get('data') or {}).get('dash') or {}).get('audio') or []
    if not audios:
        print('这个分 P 没有独立音轨（可能是纯视频/互动视频）')
        return 1
    # 30232 = 128k，30216 = 64k，30280 = 192k；取中间那档
    audios.sort(key=lambda a: a.get('bandwidth', 0))
    a = audios[min(1, len(audios) - 1)]
    print('音轨 id=%s %s %dkbps' % (a.get('id'), a.get('codecs'), a.get('bandwidth', 0) // 1000))

    m4s = os.path.join(os.path.dirname(out), '.tmp_%s.m4s' % os.path.basename(out))
    req = urllib.request.Request(a['baseUrl'], headers={'User-Agent': UA, 'Referer': REF})
    with urllib.request.urlopen(req, timeout=60) as r, open(m4s, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        got = 0
        while True:
            b = r.read(1 << 16)
            if not b:
                break
            f.write(b)
            got += len(b)
            if total:
                sys.stdout.write('\r下载 %5.1f%%' % (got * 100.0 / total))
                sys.stdout.flush()
    print('\n分片 %.1f MB' % (os.path.getsize(m4s) / 1048576.0))

    cmd = [FFMPEG, '-y', '-hide_banner', '-loglevel', 'error', '-i', m4s,
           '-vn', '-ac', '1', '-c:a', 'libmp3lame', '-b:a', '64k',
           '-id3v2_version', '3', '-metadata', 'title=%s' % title, out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.remove(m4s)
    if r.returncode != 0:
        print('转码失败：', r.stderr[-500:])
        return 1
    print('完成：%s（%.1f MB）' % (out, os.path.getsize(out) / 1048576.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
