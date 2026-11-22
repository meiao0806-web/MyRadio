# -*- coding: utf-8 -*-
"""用 --dump-dom 验预览页的「停播台跳过」（不需要 CDP / 驱动）。

做法：把生成的 preview/index.html 复制一份，在**最后一个** </body> 前插一段断言脚本
（注意必须 rfind —— 页面里把 App 的整段 HTML 当字符串内嵌了，里面也有 </body>），
跑 chrome --headless --dump-dom，再把 <pre id="out"> 抠出来看。
"""
import io, os, re, subprocess, sys, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(ROOT, 'preview', 'index.html')
s = io.open(src, encoding='utf-8', newline='').read()

ASSERT = r"""
<script>
(function(){
  const out = [];
  function ck(cond, msg){ out.push((cond ? 'OK   ' : 'FAIL ') + msg); }

  // 1) 开局：?off=3,5 标了两台
  const offNames = Object.keys(S.off).map(k => S.list[+k].n);
  ck(offHidden() === 2, '开局跳过 2 台（' + offNames.join(' / ') + '）');

  // 2) 旋钮转一圈，落点里不能出现被跳过的台
  S.off = {}; S.off[2] = true; S.off[3] = true; S.off[4] = true;
  S.cur = 0; S.preview = -1; S.pending = 0;
  const land = [];
  for (let k = 0; k < S.list.length - 3; k++) { S.cur = stepUnit(S.cur, +1); land.push(S.cur); }
  ck(!land.some(i => S.off[i]), '连转一圈全部跳过停播台');
  ck(new Set(land).size === S.list.length - 3, '正好走遍剩下的 ' + (S.list.length - 3) + ' 台');
  // 走满「剩下的台数」正好绕回起点 —— 说明跳过没有吃掉/重复任何一台
  ck(land.length === S.list.length - 3 && land[land.length-1] === 0,
     '转满 ' + (S.list.length - 3) + ' 格正好绕回起点（不漂、不重复）');

  // 3) 反向也跳过
  S.cur = 0;
  const back = stepUnit(0, -1);
  ck(!S.off[back], '反向一格落在 #' + back + '（不是停播台）');

  // 4) 整表都被跳过 → 退回普通换台，不能卡死
  S.off = {};
  for (let i = 0; i < S.list.length; i++) S.off[i] = true;
  const j = stepUnit(0, +1);
  ck(j === 1, '整表都停播时仍能换台（#0 → #' + j + '）');

  // 5) 屏上「跳过N」真的画出来了
  S.off = {}; S.off[2] = true; S.off[3] = true;
  S.cur = 0; S.preview = -1; render();
  const ob = '跳过' + offHidden();
  ck(offHidden() === 2 && ob === '跳过2', 'offHidden() = 2（屏上写「跳过2」）');

  // 6) 列表里的「停播」小标 + 点一下放回
  renderList();
  const chip = document.querySelector('#list [data-off]');
  ck(!!chip, '列表里出现「停播」小标');
  if (chip) { chip.click(); ck(offHidden() === 1, '点小标后放回 1 台（剩 1）'); }

  // 7) 删台时 off 下标要跟着挪
  S.off = {}; S.off[5] = true;
  const nm5 = S.list[5].n;
  offDropAt(2);
  S.list.splice(2, 1);
  ck(offHidden() === 1 && S.list[4].n === nm5, '删掉第 3 台后，被标记的还是原来那台（' + nm5 + '）');

  const pre = document.createElement('pre');
  pre.id = 'out';
  pre.textContent = out.join('\n');
  document.body.appendChild(pre);
})();
</script>
"""

i = s.rfind('</body>')
assert i > 0
html = s[:i] + ASSERT + s[i:]

tmp = os.path.join(tempfile.gettempdir(), 'mr_offair_pv.html')
io.open(tmp, 'w', encoding='utf-8').write(html)

chrome = r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'
prof = os.path.join(tempfile.gettempdir(), 'mr_offair_prof')
shutil.rmtree(prof, ignore_errors=True)

cmd = [chrome, '--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
       '--allow-file-access-from-files', '--virtual-time-budget=8000',
       '--user-data-dir=' + prof, '--dump-dom',
       'file:///' + tmp.replace('\\', '/') + '?off=3,5']
r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
dom = r.stdout or ''
m = re.search(r'<pre id="out">(.*?)</pre>', dom, re.S)
if not m:
    print('没拿到断言输出（dom %d 字节）' % len(dom))
    print(dom[:800])
    sys.exit(2)
txt = m.group(1)
print(txt)
fails = txt.count('FAIL')
print('\n通过 %d / 失败 %d' % (txt.count('OK   '), fails))
sys.exit(1 if fails else 0)
