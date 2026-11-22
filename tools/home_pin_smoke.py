# -*- coding: utf-8 -*-
"""home_pin_smoke.py —— 在真浏览器里跑首页那段「输 4 位码连机器」的 DOM 逻辑

为什么不用静态断言：这段 JS 是插进已有的巨型 IIFE 里的，
写错一个括号，整页按钮全废（1.41.0 出过这个事故）。
这里把页面喂给无头 Chrome，用假桥（window.MRApp）点一遍：
  · 输 3 位不该乱找
  · 输满 4 位要自动调 pinFind('1234')
  · 阶段 / 成功 / 失败三种回调都要落到提示行
  · 夹在里面的字母要被剔掉
"""
import io
import os
import re
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'
SRC = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'assets', 'home.html')

FAKE = '''<script>
window.__calls = [];
window.MRApp = {
  ver: function(){ return 'test'; },
  pinFind: function(c){ window.__calls.push(c); },
  scan: function(){}, recent: function(){ return '[]'; },
  sdk: function(){ return 34; }, curSsid: function(){ return ''; },
  provProbe: function(){}, provSend: function(){}, toastMsg: function(){}
};
</script>
'''

TEST = '''<script>
(function(){
  var out = [];
  function ck(n, c, e){ out.push((c ? 'OK   ' : 'FAIL ') + n + (c ? '' : '   ' + (e === undefined ? '' : e))); }
  var pin = document.getElementById('pin');
  var btn = document.getElementById('btnPin');
  var note = document.getElementById('pinNote');
  ck('有 4 位输入框', !!pin);
  ck('有连接按钮', !!btn);
  ck('提示里写了出厂值 1234', !!note && note.textContent.indexOf('1234') >= 0, note && note.textContent);
  ck('输入框是数字键盘', !!pin && pin.getAttribute('inputmode') === 'numeric');
  ck('最多 4 位', !!pin && pin.getAttribute('maxlength') === '4');
  window.__calls.length = 0;
  if (pin) { pin.value = '123'; pin.dispatchEvent(new Event('input')); }
  ck('输到 3 位不触发查找', window.__calls.length === 0, window.__calls.join(','));
  if (pin) { pin.value = '1234'; pin.dispatchEvent(new Event('input')); }
  ck('输满 4 位自动调 pinFind', window.__calls.length === 1 && window.__calls[0] === '1234',
     JSON.stringify(window.__calls));
  ck('查找中按钮变灰', !!btn && btn.disabled === true);
  window.__mrPair({ stage: '正在广播找 1234…' });
  ck('阶段文案落到提示行', !!note && note.textContent.indexOf('广播') >= 0, note && note.textContent);
  window.__mrPair({ ok: 1, ip: '10.0.0.5', name: 'MyRadio' });
  ck('成功后提示带上机器', !!note && (note.textContent.indexOf('10.0.0.5') >= 0 || note.textContent.indexOf('MyRadio') >= 0),
     note && note.textContent);
  ck('成功后按钮恢复', !!btn && btn.disabled === false);
  window.__mrPair({ err: '配对码不对 —— 看看收音机屏幕上那 4 位' });
  ck('失败提示会显示', !!note && note.textContent.indexOf('码不对') >= 0, note && note.textContent);
  if (pin) { pin.value = '1a2b'; pin.dispatchEvent(new Event('input')); }
  ck('字母被剔掉', !!pin && pin.value === '12', pin && pin.value);
  ck('空白码点按钮不乱跑', (function(){
      if (!pin || !btn) return false;
      window.__calls.length = 0;
      pin.value = ''; btn.click();
      return window.__calls.length === 0;
  })());
  var pre = document.createElement('pre');
  pre.id = 'out';
  pre.textContent = out.join('\\n') + '\\n=== 通过 ' +
      out.filter(function(x){ return x.indexOf('OK') === 0; }).length +
      ' / 失败 ' + out.filter(function(x){ return x.indexOf('FAIL') === 0; }).length + ' ===';
  document.body.appendChild(pre);
})();
</script>
'''


def main():
    html = io.open(SRC, encoding='utf-8').read()
    # 假桥要在页面自己的脚本之前生效
    i = html.find('<script>')
    assert i > 0
    html = html[:i] + FAKE + html[i:]
    html = html.replace('</body>', TEST + '</body>')

    d = tempfile.mkdtemp(prefix='mr_home_')
    f = os.path.join(d, 'home_test.html')
    io.open(f, 'w', encoding='utf-8').write(html)

    prof = os.path.join(d, 'prof')
    cmd = [CHROME, '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
           '--no-default-browser-check', '--allow-file-access-from-files',
           '--user-data-dir=' + prof, '--virtual-time-budget=4000',
           '--dump-dom', 'file:///' + f.replace('\\', '/')]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    dom = r.stdout or ''
    m = re.search(r'<pre id="out">(.*?)</pre>', dom, re.S)
    if not m:
        print('!! 页面里没有断言输出，说明 JS 中途抛错了。DOM 片段：')
        print(dom[:1200])
        sys.exit(2)
    body = (m.group(1).replace('&lt;', '<').replace('&gt;', '>').replace('&amp;', '&'))
    print(body)
    sys.exit(0 if '失败 0 ===' in body else 1)


if __name__ == '__main__':
    main()
