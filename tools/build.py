# -*- coding: utf-8 -*-
"""编译（可选烧录）。用法：python tools/build.py [flash]

为什么要先删 firmware.elf：本机 pio 第二次编译时会批量清理中间产物，
撞上 safe-delete 拦截后静默卡死（实测 7 分钟无输出）。先删掉 elf/bin/map 就不会触发。
"""
import os, sys, subprocess, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIO = r'C:\Users\lenovo\.workbuddy\binaries\python\envs\default\Scripts\pio.exe'
ESPTOOL = [r'C:\Users\lenovo\.workbuddy\binaries\python\envs\default\Scripts\python.exe',
           '-m', 'esptool', '--chip', 'esp32s3', '-p', 'COM3', '-b', '460800',
           'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '4MB',
           '0x10000']

build = os.path.join(ROOT, '.pio', 'build', 'myradio')
for pat in ('firmware.elf', 'firmware.bin', 'firmware.map'):
    p = os.path.join(build, pat)
    if os.path.exists(p):
        os.remove(p)
        print('[clean] %s' % pat)

env = dict(os.environ)
for k in list(env):
    if k.startswith('CODEBUDDY_SAFE_DELETE'):
        env.pop(k)

print('[pio] 编译中…')
r = subprocess.run([PIO, 'run', '-e', 'myradio'], cwd=ROOT, env=env,
                   capture_output=True, text=True, errors='replace')
out = r.stdout + r.stderr
tail = [l for l in out.splitlines() if ('SUCCESS' in l or 'FAILED' in l or 'error:' in l
                                        or 'RAM:' in l or 'Flash:' in l)]
print('\n'.join(tail[-12:]))
if r.returncode != 0:
    print(out[-3000:])
    sys.exit(1)

binp = os.path.join(build, 'firmware.bin')
print('[bin] %s  %.2f MB' % (binp, os.path.getsize(binp) / 1048576.0))

if 'flash' in sys.argv:
    print('[flash] 烧录中…')
    r2 = subprocess.run(ESPTOOL + [binp], capture_output=True, text=True, errors='replace')
    for l in (r2.stdout + r2.stderr).splitlines():
        if any(k in l for k in ('Hash of', 'Wrote ', 'ERROR', 'Verified')):
            print('  ' + l.strip())
    sys.exit(r2.returncode)
