# -*- coding: utf-8 -*-
"""给 App 页面截三张验收图（首页收起 / 首页展开 / 设置页收音机卡）。

复用 verify_appctl.py 里的 extract() + MOCK，保证截的就是 src/webapp.h 那一份。
"""
import importlib.util
import io
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

spec = importlib.util.spec_from_file_location('v', os.path.join(HERE, 'verify_appctl.py'))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)

CH = v.CHROME
html0 = v.extract()
i = html0.find('<script>')
base = html0[:i] + v.MOCK + html0[i:]

CASES = [
    ('_app1_home_collapsed', 1500,
     "setTimeout(function(){go('home');window.scrollTo(0,0);},800);"),
    ('_app2_home_open', 1750,
     "setTimeout(function(){go('home');},800);"
     "setTimeout(function(){document.getElementById('btnCtl').click();},1400);"),
    ('_app3_fmcard', 1750,
     "setTimeout(function(){go('settings');},800);"
     "setTimeout(function(){var c=document.getElementById('fmCard');"
     "if(c)c.scrollIntoView({block:'start'});},1500);"),
    ('_app4_savedwifi', 1750,
     "setTimeout(function(){go('settings');},800);"
     "setTimeout(function(){document.getElementById('btnSavedWifi').click();},1500);"),
]

for name, h, js in CASES:
    h2 = base.replace('</body>', '<script>window.onerror=function(m){document.title="ERR "+m;};'
                                + js + '</script></body>', 1)
    p = os.path.join(tempfile.gettempdir(), name + '.html')
    io.open(p, 'w', encoding='utf-8').write(h2)
    out = os.path.join(ROOT, name + '.png')
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([CH, '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
                    '--user-data-dir=' + os.path.join(tempfile.gettempdir(), 'mrp_' + name),
                    '--window-size=420,%d' % h,
                    '--allow-file-access-from-files', '--virtual-time-budget=7000',
                    '--hide-scrollbars', '--force-device-scale-factor=2',
                    '--screenshot=' + out, 'file:///' + p.replace('\\', '/')],
                   capture_output=True)
    print(name, os.path.getsize(out) if os.path.exists(out) else 'MISSING')
