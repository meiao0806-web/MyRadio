#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""本地音频文件夹 -> 机器台表（整本有声书一键上架）

    python tools/import_local.py --dir="D:/百度网盘/红楼梦" --group=听书 --name=红楼梦
    python tools/import_local.py --dir=... --group=听书 --name=红楼梦 --dry     # 只看不写

它干三件事：
    1. 扫文件夹里的音频（mp3 / m4a / aac），按文件名里的数字自然排序；
    2. 在本机起一个静态 HTTP（**带 Range**，m4a 没有 Range 放不了），
       把文件映射成短 ASCII 地址 http://<本机IP>:8099/a/1.mp3 ——
       中文文件名、空格、长路径全在映射表里，机器那头看到的永远是干净短链；
    3. 调机器的 /api/add，把每一集加成「分组」里的一个台。

台名默认是「<name> 001 / 002 …」。这不是随便起的：
    album.cpp 认「尾部 空格 + 2~4 位数字」，同一前缀的台在组内会被当成**一个单元** ——
    旋钮转一格就是这本书（不是一集），一集播完自动接下一集，跟出厂那几部书手感一样。
    单组上限 MAX_STATIONS = 160 台，120 回放得下；超过要拆成两组。

注意：
    · 这个 HTTP 服务一关，机器就拉不到流了（台表还在，只是没声音）。
      长期用请把它放到 NAS / 常开的机器上，或改用云听那种永久直链。
    · 重复运行会重复加台（固件没有"按名字去重"的接口）。
"""
import sys, os, json, time, socket, argparse, threading, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

AUDIO_EXT = {'.mp3': 'audio/mpeg', '.m4a': 'audio/mp4', '.mp4': 'audio/mp4',
             '.aac': 'audio/aac', '.m4b': 'audio/mp4'}
SKIP_EXT = {'.flac', '.wav', '.ape', '.wma', '.ogg', '.opus'}

# 固件侧限制（src/config.h / state.h / net.cpp）
MAX_STATIONS = 160      # 单组台数上限
NAME_BYTES = 39         # Station.name[40]，含结尾 0
URL_MAX = 190           # net.cpp validUrl()


# ---------------------------------------------------------------- 工具
def out(*a):
    print(*a, flush=True)


def natural_key(s):
    """'第10集' 排在 '第9集' 后面"""
    import re
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]


def clip_utf8(s, n):
    """按字节截断，不切坏汉字"""
    b = s.encode('utf-8')
    if len(b) <= n:
        return s
    return b[:n].decode('utf-8', 'ignore')


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return '127.0.0.1'


def http_json(url, data=None, token=None, timeout=12):
    # 直连，不走系统代理 —— 机器的地址是内网 IP，
    # 一旦被 http_proxy 劫走就是 502（本机调试时踩过）
    body = None
    if data is not None:
        body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(url, data=body)
    if token:
        req.add_header('X-Token', token)
    if body is not None:
        req.add_header('Content-Type', 'application/x-www-form-urlencoded')
    op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with op.open(req, timeout=timeout) as r:
        return json.loads(r.read().decode('utf-8', 'replace'))


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    """把 /a/<id>.<ext> 映射到真实文件；必须支持 Range（audioI2S 取 m4a 的 moov 要靠它）"""
    table = {}          # '1.mp3' -> 绝对路径
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):      # 别把机器的每段请求都刷屏
        pass

    def _send(self, head_only=False):
        path = urllib.parse.urlparse(self.path).path
        key = path.rsplit('/', 1)[-1]
        fp = self.table.get(key)
        if not fp or not os.path.isfile(fp):
            self.send_error(404, 'not in table')
            return
        size = os.path.getsize(fp)
        ext = os.path.splitext(fp)[1].lower()
        ctype = AUDIO_EXT.get(ext, 'application/octet-stream')

        rng = self.headers.get('Range')
        start, end = 0, size - 1
        partial = False
        if rng and rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            try:
                if a:
                    start = int(a)
                    if b:
                        end = min(int(b), size - 1)
                elif b:                      # bytes=-500 尾部
                    start = max(0, size - int(b))
                    end = size - 1
                partial = True
            except ValueError:
                start, end, partial = 0, size - 1, False
        if start > end or start >= size:
            self.send_response(416)
            self.send_header('Content-Range', 'bytes */%d' % size)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return

        self.send_response(206 if partial else 200)
        self.send_header('Content-Type', ctype)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        if partial:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if head_only:
            return
        with open(fp, 'rb') as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                buf = f.read(min(65536, left))
                if not buf:
                    break
                try:
                    self.wfile.write(buf)
                except (BrokenPipeError, ConnectionResetError):
                    return                   # 机器换台了，正常
                left -= len(buf)

    def do_GET(self):
        self._send(False)

    def do_HEAD(self):
        self._send(True)


def serve(dirpath, port):
    srv = ThreadingHTTPServer(('0.0.0.0', port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--dir', required=True, help='音频文件夹（会递归扫）')
    ap.add_argument('--group', default='听书', help='加进哪个分组（不存在会新建）')
    ap.add_argument('--name', default='', help='台名前缀，默认取文件夹名')
    ap.add_argument('--host', default='myradio.local', help='机器地址，默认 myradio.local')
    ap.add_argument('--port', type=int, default=8099, help='本机 HTTP 端口，默认 8099')
    ap.add_argument('--bind', default='', help='对外报的 IP，默认自动探测')
    ap.add_argument('--code', default='', help='开了配对码时，屏上那 6 位')
    ap.add_argument('--start', type=int, default=1, help='起始序号，默认 1')
    ap.add_argument('--digits', type=int, default=3, help='序号位数，默认 3')
    ap.add_argument('--delay', type=float, default=0.2, help='每加一台的间隔秒数')
    ap.add_argument('--dry', action='store_true', help='只打印，不动机器')
    ap.add_argument('--no-serve', action='store_true', help='不起 HTTP（只加台）')
    a = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    root = os.path.abspath(a.dir)
    if not os.path.isdir(root):
        out('找不到文件夹：', root)
        return 1
    prefix = a.name or os.path.basename(root.rstrip('\\/'))

    files, skipped = [], []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            ext = os.path.splitext(fn)[1].lower()
            if ext in AUDIO_EXT:
                files.append(os.path.join(dp, fn))
            elif ext in SKIP_EXT:
                skipped.append(fn)
    files.sort(key=lambda p: natural_key(os.path.relpath(p, root)))

    if not files:
        out('这个文件夹里没有 mp3 / m4a / aac。')
        if skipped:
            out('  看到了 %d 个机器放不了的格式（%s）—— 得先转成 mp3。'
                % (len(skipped), ', '.join(sorted({os.path.splitext(x)[1] for x in skipped}))))
        return 1
    if len(files) > MAX_STATIONS:
        out('注意：%d 集 > 单组上限 %d 台，机器只会收下前 %d 集。'
            '（要全收得拆成两组，用 --start 分两次跑）' % (len(files), MAX_STATIONS, MAX_STATIONS))

    ip = a.bind or lan_ip()
    base = 'http://%s:%d/a/' % (ip, a.port)

    # 1) 建映射表
    plan = []
    for k, fp in enumerate(files):
        ext = os.path.splitext(fp)[1].lower()
        key = '%d%s' % (k + 1, ext)
        Handler.table[key] = fp
        idx = a.start + k
        title = clip_utf8('%s %0*d' % (prefix, a.digits, idx), NAME_BYTES)
        plan.append((title, base + key, os.path.basename(fp)))

    out('文件夹：%s' % root)
    out('识别到 %d 集（%s）%s' % (len(files),
        ', '.join(sorted({os.path.splitext(f)[1].lower() for f in files})),
        ('，跳过 %d 个放不了的格式' % len(skipped)) if skipped else ''))
    out('台名：%s … %s' % (plan[0][0], plan[-1][0]))
    if len(plan[0][1]) > URL_MAX:
        out('警告：地址超过固件上限 %d 字符，机器会拒收' % URL_MAX)
    out('')

    if a.dry:
        for t, u, f in plan[:8]:
            out('  %-16s %-40s <- %s' % (t, u, f))
        if len(plan) > 8:
            out('  … 共 %d 条' % len(plan))
        return 0

    if not a.no_serve:
        try:
            serve(root, a.port)
        except OSError as e:
            out('端口 %d 起不来（%s），换一个：--port=8100' % (a.port, e))
            return 1
        out('HTTP 已开：%s   （Ctrl-C 结束）' % base)

    # 2) 拿令牌
    dev = 'http://%s' % a.host
    try:
        q = '/api/auth' + (('?code=' + a.code) if a.code else '')
        r = http_json(dev + q)
    except Exception as e:
        out('连不上机器（%s）：%s' % (dev, e))
        out('  先在 App 里看一眼设备地址，用 --host=192.168.x.x 指定；或确认在同一 WiFi。')
        return 1
    if r.get('locked') and not r.get('token'):
        out('机器开了配对码，屏上会显示 6 位数字 —— 抄下来用 --code=xxxxxx 重跑。')
        return 1
    tok = r.get('token') or ''
    out('已连上机器，令牌 %s…' % tok[:4])

    # 3) 分组
    st = http_json(dev + '/api/stations', token=tok)
    groups = st.get('groups') or []
    g = next((x['i'] for x in groups if x.get('n') == a.group), None)
    if g is None:
        http_json(dev + '/api/groupop', {'op': 'add', 'n': a.group}, token=tok)
        groups = (http_json(dev + '/api/stations', token=tok).get('groups') or [])
        g = next((x['i'] for x in groups if x.get('n') == a.group), None)
        if g is None:
            out('建分组失败：%s' % a.group)
            return 1
        out('已新建分组「%s」' % a.group)
    else:
        out('分组「%s」已存在（第 %d 组）' % (a.group, g + 1))

    # 4) 逐条加
    ok = fail = 0
    for k, (title, url, fn) in enumerate(plan):
        try:
            r = http_json(dev + '/api/add', {'name': title, 'url': url, 'g': g}, token=tok)
            if r.get('err'):
                fail += 1
                out('[%d/%d] %s 失败：%s' % (k + 1, len(plan), title, r['err']))
            else:
                ok += 1
                sys.stdout.write('\r[%d/%d] %s' % (k + 1, len(plan), title))
                sys.stdout.flush()
        except Exception as e:
            fail += 1
            out('[%d/%d] %s 失败：%s' % (k + 1, len(plan), title, e))
        if a.delay:
            time.sleep(a.delay)
    out('')
    out('完成：成功 %d 条，失败 %d 条，已进分组「%s」。' % (ok, fail, a.group))

    if a.no_serve:
        return 0
    out('')
    out('这台电脑现在在给机器供流，**关掉这个窗口就没声了**。')
    out('想长期用：把文件夹放到 NAS / 常开的机器上，或者改走云听那种永久直链。')
    out('按 Ctrl-C 退出。')
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        out('已停止供流。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
