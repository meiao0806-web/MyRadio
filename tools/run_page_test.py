# -*- coding: utf-8 -*-
"""在无头 Chrome 里跑一个 Node 版页面回归脚本（CDP 直连，零第三方依赖）。

用法：
    python tools/run_page_test.py verify_wifi_slots.js [另一个.js ...]

为什么不用 --dump-dom：这些回归要点击、要等 setInterval、要读 iframe 里的 DOM，
虚拟时间推不动 rAF/定时器，只能真开一个浏览器。

⚠️ 必须给独立 --user-data-dir：用户自己开着 Chrome 时，新进程会把请求转交给已有实例，
   --window-size 直接失效（踩过）。
"""
import os, sys, subprocess, tempfile, time, json, shutil, urllib.request, socket

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NODE = r'C:\Users\lenovo\.workbuddy\binaries\node\versions\22.22.2-3\node.exe'
CHROME = r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'
PORT = 9333


def free_port(p):
    s = socket.socket()
    try:
        s.bind(('127.0.0.1', p))
        return True
    except OSError:
        return False
    finally:
        s.close()


def wait_cdp(t=25):
    for _ in range(int(t / 0.4)):
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/json/version' % PORT, timeout=1) as f:
                json.loads(f.read().decode())
                return True
        except Exception:
            time.sleep(0.4)
    return False


def main():
    scripts = [a for a in sys.argv[1:] if a.endswith('.js')]
    if not scripts:
        print('用法: python tools/run_page_test.py <脚本.js> [...]')
        return 2

    if not free_port(PORT):
        print('端口 %d 被占用，先关掉那个 Chrome' % PORT)
        return 2

    prof = tempfile.mkdtemp(prefix='cdp_prof_')
    args = [CHROME, '--headless=new', '--disable-gpu', '--no-first-run',
            '--no-default-browser-check', '--disable-extensions',
            '--allow-file-access-from-files',
            '--remote-debugging-port=%d' % PORT,
            '--user-data-dir=' + prof,
            '--window-size=430,900',
            'about:blank']
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc = 1
    try:
        if not wait_cdp():
            print('Chrome 的 CDP 没起来')
            return 1
        env = dict(os.environ, CDP_PORT=str(PORT))
        for sc in scripts:
            path = os.path.join(ROOT, 'tools', sc)
            print('\n================ %s ================' % sc)
            r = subprocess.run([NODE, path], cwd=ROOT, env=env,
                               capture_output=True, text=True, errors='replace')
            print(r.stdout.rstrip())
            if r.stderr.strip():
                print('--- stderr ---')
                print(r.stderr.rstrip()[-1600:])
            if '0 / ' not in r.stdout and '通过' in r.stdout:
                pass
            rc = 0 if r.returncode == 0 else 1
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        time.sleep(0.6)
        shutil.rmtree(prof, ignore_errors=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
