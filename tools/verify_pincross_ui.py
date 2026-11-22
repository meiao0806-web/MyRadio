# -*- coding: utf-8 -*-
"""verify_pincross_ui.py —— 1.46.7 App「我的机器」输码行的真浏览器回归

为什么单独一个文件：verify_pincross.py 只做源码级断言，
而「输框会不会顶出卡片、码标有没有画出来、打字母会不会被剔掉」必须真排版才知道。
用 --dump-dom 就够（页面上没有 rAF / transition 依赖的断言）。

⚠️ 两个坑（都踩过）：
  1. `.pinrow` 是 flex 容器，getBoundingClientRect 给的是 **border-box**，
     padding 算在里面 —— 拿它和卡片比会永远得 0 余量。要量**子元素**
     （#devPin 的左缩进 / #devPinGo 的右缩进）。
  2. 页面里的 CLOUD 是 `<script>` 顶层的 `const`，属于**全局词法声明**：
     直接写 `CLOUD` 能访问，但 `window.CLOUD` 是 undefined。

用法：python tools/verify_pincross_ui.py
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PASS = FAIL = 0
def ok(c, m):
    global PASS, FAIL
    if c: PASS += 1; print('  OK   ' + m)
    else: FAIL += 1; print('  FAIL ' + m)

CHROME = r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'
if not os.path.exists(CHROME):
    print('没有 Chrome，跳过（这台机器装不了就手工看一眼）'); sys.exit(0)

wa = io.open(os.path.join(ROOT, 'src', 'webapp.h'), encoding='utf-8').read()
m = re.search(r'R"HTMLX\((.*)\)HTMLX"', wa, re.S)
if not m:
    print('webapp.h 里找不到 HTML'); sys.exit(2)
html = m.group(1)

STUB = r'''
<script>
setTimeout(function(){
  var out=[]; function ck(c,m){ out.push((c?'OK   ':'FAIL ')+m); }
  try{ setTab('me'); }catch(e){}

  var card = document.getElementById('devCard');
  var el   = document.getElementById('devPin');
  var go   = document.getElementById('devPinGo');
  ck(!!card && !!el && !!go, '设置页里有「我的机器」卡片和输码行');

  // ---- 排版 ----
  var cr = card.getBoundingClientRect();
  var a  = el.getBoundingClientRect();
  var b  = go.getBoundingClientRect();
  var dl = Math.round(a.left - cr.left), dr = Math.round(cr.right - b.right);
  ck(dl >= 16 && dr >= 16, '左右都留了 18px，没顶出卡片（左 ' + dl + ' / 右 ' + dr + '）');
  ck(Math.abs(a.top - b.top) < 6 && b.left > a.right - 2, '同一行、按钮在框右边');
  ck(a.width > 140 && b.width > 44, '两者都有正常宽度（' + Math.round(a.width) + ' / ' + Math.round(b.width) + '）');
  var de = document.documentElement;
  ck(de.scrollWidth <= de.clientWidth + 1, '整页没有横向溢出（' + de.scrollWidth + ' vs ' + de.clientWidth + '）');
  var st = getComputedStyle(el);
  ck(/inset/.test(st.boxShadow), '输入框是凹陷阴影（新拟物里「能输入」的语言）');
  ck(st.letterSpacing !== 'normal' && parseFloat(st.letterSpacing) > 3, '数字有字距，像 4 格');
  ck(el.getAttribute('inputmode') === 'numeric' && el.maxLength === 4, '数字键盘 + 最多 4 位');

  // ---- 输入清洗 ----
  el.value = 'a1b2';  el.dispatchEvent(new Event('input', {bubbles:true}));
  ck(el.value === '12', '字母自动剔掉（a1b2 → ' + el.value + '）');
  el.value = '12345'; el.dispatchEvent(new Event('input', {bubbles:true}));
  ck(el.value === '1234', '超过 4 位自动截断（12345 → ' + el.value + '）');

  // ---- 码标：两台同码 + 一台老固件 ----
  // ⚠️ 用 renderDevs(list) 传参，别去改 CLOUD.S.devs：
  //    CLOUD 是 script 顶层 const（全局词法），但 window.CLOUD 是 undefined。
  renderDevs([
    {dev:'A1B2C3', id:'A1B2C3D4E5F6', n:'MyRadio-客厅',   open:1, batt:82, pin:'1234', seen:Date.now()},
    {dev:'FFFFFF', id:'FFFFFFFFFFFF', n:'MyRadio-老家',   open:1, batt:51, pin:'1234', seen:Date.now()},
    {dev:'EEEEEE', id:'EEEEEEEEEEEE', n:'MyRadio-旧固件', open:1, batt:60, pin:'',     seen:Date.now()}
  ]);
  var rows = document.querySelectorAll('#devList .devrow');
  ck(rows.length === 3, '列表画出 3 台（' + rows.length + '）');
  var pns = document.querySelectorAll('#devList .devrow .pn');
  ck(pns.length === 2, '只有报码的两台有码标（' + pns.length + ' 个；老固件那台没有）');
  ck(pns[0] && pns[0].textContent === '1234', '码标内容就是 1234');
  var nm = rows[0] ? rows[0].querySelector('.nm') : null;
  ck(nm && nm.textContent === 'MyRadio-客厅', '台名照旧显示（码标没挤掉它）');

  // ---- 两台同名时靠码标分清 ----
  renderDevs([
    {dev:'A1B2C3', n:'MyRadio', open:1, batt:82, pin:'1234', seen:Date.now()},
    {dev:'FFFFFF', n:'MyRadio', open:1, batt:51, pin:'5678', seen:Date.now()}
  ]);
  var p2 = document.querySelectorAll('#devList .devrow .pn');
  ck(p2.length === 2 && p2[0].textContent !== p2[1].textContent,
     '两台名字都叫 MyRadio 时，靠码标还能分清（' +
     (p2[0] ? p2[0].textContent : '?') + ' / ' + (p2[1] ? p2[1].textContent : '?') + '）');

  // ---- 连接码这一行：能看、能改 ----
  ck(!!document.getElementById('i-key-round'), '「连接码」那行有图标（key-round 真在图标表里）');
  var pinBtn = document.getElementById('btnPinEdit');
  ck(!!pinBtn && !!document.getElementById('svPin'), '设置里有「连接码」一行 + 显示当前码的位置');
  if (pinBtn){
    pinBtn.click();
    var peIn = document.getElementById('peIn');
    ck(!!peIn, '点一下弹出改码弹层');
    if (peIn){
      peIn.value = 'ab12cd34';
      peIn.dispatchEvent(new Event('input', {bubbles:true}));
      ck(peIn.value === '1234', '弹层里只留数字、最多 4 位（ab12cd34 → ' + peIn.value + '）');
      ck(!!document.getElementById('peYes') && !!document.getElementById('peNo'), '有「改成这个 / 算了」');
      var noBtn = document.getElementById('peNo');
      if (noBtn) noBtn.click();
    }
  }

  var pre = document.createElement('pre'); pre.id = 'out';
  pre.textContent = out.join('\n');
  document.body.appendChild(pre);
}, 900);
</script>
'''

i = html.rfind('</body>')
fx = os.path.join(tempfile.gettempdir(), 'mr_pincross_ui.html')
io.open(fx, 'w', encoding='utf-8').write(html[:i] + STUB + html[i:])

prof = os.path.join(tempfile.gettempdir(), 'mr-pincross-ui-prof')
shutil.rmtree(prof, ignore_errors=True)
r = subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
                    '--allow-file-access-from-files', '--window-size=430,760',
                    '--virtual-time-budget=8000', '--user-data-dir=' + prof,
                    '--dump-dom', 'file:///' + fx.replace('\\', '/')],
                   capture_output=True, text=True, encoding='utf-8', errors='replace')
mm = re.search(r'<pre id="out">(.*?)</pre>', r.stdout or '', re.S)
if not mm:
    print('没拿到断言输出（dom %d 字节）' % len(r.stdout or ''))
    sys.exit(2)
for line in mm.group(1).splitlines():
    if line.startswith('OK   '): ok(True, line[5:])
    elif line.startswith('FAIL '): ok(False, line[5:])

print('\n通过 %d / 失败 %d' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
