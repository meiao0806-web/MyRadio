# -*- coding: utf-8 -*-
"""verify_pincross.py —— 1.46.7「跨网也输 4 位码就连」的回归

三段：
  A. cloud.js 的纯逻辑（在 Node 里跑，不需要浏览器）：findByPin / pinLogin
     的三种分支、pin 解析宽容度、老固件不发 pin 的降级。
  B. 源码级接线：固件真的把 pin 发进 dev 广播、改了码立刻重播、
     cloud.js 真的把它读进来并导出。
  C. 远程版网页的入口真的换成了 4 格（含旧 12 位输入框收进折叠）。

用法：python tools/verify_pincross.py
"""
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def P(*a): return os.path.join(ROOT, *a)
def rd(*a): return io.open(P(*a), encoding='utf-8', errors='replace').read()

PASS = FAIL = 0
def ok(c, m):
    global PASS, FAIL
    if c: PASS += 1; print('  OK   ' + m)
    else: FAIL += 1; print('  FAIL ' + m)

# ============================================================ A. 逻辑
print('[A] cloud.js 的「按码找机器」逻辑（Node 里跑真文件）')
NODE = None
for c in (r'C:/Users/lenovo/.workbuddy/binaries/node/versions/22.22.2-3/node.exe', 'node'):
    try:
        subprocess.run([c, '-e', '0'], capture_output=True, timeout=15); NODE = c; break
    except Exception: pass
if not NODE:
    print('  !! 找不到 node，跳过 A 段'); FAIL += 1
else:
    HARNESS = r'''
const fs = require('fs'), path = require('path'), vm = require('vm');
const ROOT = process.argv[2];
const src = fs.readFileSync(path.join(ROOT, 'tools/app/js/cloud.js'), 'utf8');
// cloud.js 是 IIFE，挂到 CLOUD。它用到 localStorage / WebSocket / TextEncoder，
// 这里给最小替身（不连网，只喂 devs）
const store = {};
const ctx = {
  console,
  localStorage: { getItem: k => (k in store ? store[k] : null),
                  setItem: (k, v) => { store[k] = String(v); },
                  removeItem: k => { delete store[k]; } },
  WebSocket: function () { this.close = () => {}; this.send = () => {}; },
  TextEncoder, TextDecoder, Date, Math, JSON, setTimeout, clearTimeout,
  setInterval: () => 0, clearInterval: () => {},
  Promise, Map, String, Number, Object, Array, Error, URLSearchParams,
};
ctx.window = ctx;
vm.createContext(ctx);
// ⚠️ cloud.js 是 `const CLOUD = (function(){...})()`：vm 里 const 是**词法声明**，
//   不会成为 globalThis 的属性（第一步就在这儿拿到 undefined）。追加一句显式导出。
vm.runInContext(src + '\n;globalThis.__C = CLOUD;', ctx);
const C = ctx.__C;
if (!C) { console.log('FAIL 加载 cloud.js 后拿不到 CLOUD'); process.exit(1); }
const out = [];
const eq = (a, b, m) => out.push([JSON.stringify(a) === JSON.stringify(b), m + '  → ' + JSON.stringify(a)]);

// 造三台：两台 1234（其中一台在线一台过期）、一台 5678、一台老固件（无 pin）
C.S.devs = [
  { dev: 'A1B2C3', id: 'A1B2C3D4E5F6', n: 'MyRadio-A1B2C3', open: 1, batt: 82, pin: '1234', seen: Date.now() },
  { dev: 'BBBBBB', id: 'BBBBBBBBBBBB', n: 'MyRadio-BBBBBB', open: 1, batt: 40, pin: '1234', seen: Date.now() - 200000 },
  { dev: 'FFFFFF', id: 'FFFFFFFFFFFF', n: 'MyRadio-FFFFFF', open: 0, batt: 51, pin: '5678', seen: Date.now() },
  { dev: 'EEEEEE', id: 'EEEEEEEEEEEE', n: 'MyRadio-EEEEEE', open: 1, batt: 60, pin: '',     seen: Date.now() }
];

// 1) 唯一命中：只返回在线的 A1B2C3（过期的 BBBBBB 不算）
eq(C.findByPin('1234').map(d => d.dev), ['A1B2C3'], '输 1234 只命中在线的那台');
// 2) 码不存在
eq(C.findByPin('9999').length, 0, '输 9999 一台都没有');
// 3) 两台的码都在线时 → 两台都要看到（不替人决定）
C.S.devs[1].seen = Date.now();
eq(C.findByPin('1234').map(d => d.dev).sort(), ['A1B2C3', 'BBBBBB'], '两台同码都在线时都能被找到');
// 4) 非数字/空格/超长 → 宽容处理
eq(C.findByPin(' 12 34 ').length, 2, '输入带空格也能认');
eq(C.findByPin('12345').length, 2, '多输一位（12345）按前 4 位算 —— 宽容，不报错');
C.S.devs[1].seen = Date.now() - 200000;

// 5) pinLogin：唯一命中 → n=1 且自动选好（免配对 → 不用再输码）
const r1 = C.pinLogin('1234');
eq([r1.n, r1.dev, r1.needCode], [1, 'A1B2C3', false], 'pinLogin 唯一命中：n=1、已选定、不用补码');
eq(C.S.dev, 'A1B2C3', 'pinLogin 之后当前设备就是它');
eq(C.S.code, '274682', '遥控码自动变成派生码（跟点列表一样）');
eq(C.paired(), true, '已配对状态成立');

// 6) 关了免配对的机器：命中但 needCode=true（要人补 12 位）
const r2 = C.pinLogin('5678');
eq([r2.n, r2.dev, r2.needCode], [1, 'FFFFFF', true], 'open=0 的机器：命中但要补码');
eq(C.S.code, '', '补码前遥控码是空的');

// 7) 多台同码 → n=2 且不自动选
C.S.dev = ''; C.S.code = '';
C.S.devs[1].seen = Date.now();
const r3 = C.pinLogin('1234');
eq([r3.n, r3.list.length, r3.dev === undefined], [2, 2, true], '多台同码：n=2、给列表、不自动选');

// 8) 老固件没有任何一台报码 → anyPin() 为 false（界面要提示"固件太老"）
C.S.devs.forEach(d => { d.pin = ''; });
eq(C.anyPin(), false, '全都不是 1.46.7 → anyPin()=false（界面据此提示老固件）');
C.S.devs.forEach(d => { d.pin = '1234'; });
eq(C.anyPin(), true, '有机器报码 → anyPin()=true');

// 9) pickPin（列表里点一台）
C.S.dev = ''; C.S.code = '';
eq(C.pickPin('A1B2C3'), true, 'pickPin 免配对机器返回 true');
eq(C.S.dev, 'A1B2C3', 'pickPin 真的切了设备');

// 10) 老固件（无 pin 字段）解析不能崩，pin 记成空串
C.S.devs = [];
out.push([typeof C.findByPin === 'function', 'findByPin 已导出']);
out.push([typeof C.pinLogin === 'function', 'pinLogin 已导出']);
out.push([typeof C.anyPin === 'function', 'anyPin 已导出']);

let bad = 0;
for (const [c, m] of out) { console.log((c ? 'OK   ' : 'FAIL ') + m); if (!c) bad++; }
console.log('RESULT ' + (out.length - bad) + ' / ' + bad);
process.exit(bad ? 1 : 0);
'''
    tmp = P('_harn.js')
    io.open(tmp, 'w', encoding='utf-8').write(HARNESS)
    r = subprocess.run([NODE, tmp, ROOT], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    for line in (r.stdout or '').splitlines():
        if line.startswith('OK   '): ok(True, line[5:])
        elif line.startswith('FAIL '): ok(False, line[5:])
        elif line.startswith('RESULT'): print('  --- ' + line)
    if r.returncode not in (0, 1):
        print('  !! node 跑挂了：', (r.stderr or '')[:400]); FAIL += 1
    os.remove(tmp)

# ============================================================ B. 固件接线
print('\n[B] 固件：pin 真的跟着在线广播发出去')
cfg = rd('src', 'config.h')
cp = rd('src', 'cloud.cpp')
ch = rd('src', 'cloud.h')
cj = rd('tools/app/js/cloud.js')

ver = re.search(r'#define\s+FW_VER\s+"([^"]+)"', cfg).group(1)
# 版本号别钉死（1.46.6 那次踩过一模一样的坑）：这条功能是 1.46.7 进来的，
#   写成 == 就每升一版假 FAIL 一次。一律比大小。
vt = tuple(int(x) for x in ver.split('.'))
ok(vt >= (1, 46, 7), 'FW_VER = %s（≥ 1.46.7，跨网输码这一版起才有）' % ver)
ok('#include "pairpin.h"' in cp, 'cloud.cpp 引了 pairpin.h（拿得到那 4 位码）')
ok('\\"pin\\":\\"%s\\"' in cp and 'pinCode()' in cp, 'pubPresence 的 JSON 里带了 pin')
ok('char b[208]' in cp, '广播缓冲从 176 提到 208（够装 pin 这一段）')
ok(re.search(r'strcmp\(s_pinSeen, pc\)', cp) is not None, '改了码会立刻重播广播（不等 30 秒那一轮）')
ok('const bool first = (s_pinSeen[0] == 0);' in cp, '首次启动不重复发包（first 判据）')
ok('o += ",\\"pin\\":\\""' in cp, 'cloudJson 里也报了 pin（诊断/App 可读）')
ok('1.46.7' in ch, 'cloud.h 的协议注释写了这一层（后人不用猜）')

# cloud.js 侧
ok("pin: j.pin ? String(j.pin)" in cj, 'cloud.js 把 pin 解析进设备记录')
ok('function findByPin' in cj and 'function pinLogin' in cj and 'function anyPin' in cj,
   'cloud.js 有 findByPin / pinLogin / anyPin')
ok("devAlive(d) && d.pin === p" in cj, 'findByPin 只认「在线且码相同」（过期广播不算）')
ok('.sort((a, b) => (b.seen || 0) - (a.seen || 0))' in cj, '多台时按最近报到排序（最新的排前面）')
ok('const has = pickDev(hit[0].dev);' in cj, '唯一命中直接调 pickDev（复用原来的选中逻辑）')
ok('needCode: !has' in cj, '关了免配对的机器会标 needCode，让界面要人补码')

# 关键：不能把 pin 挪到单独主题（retain 覆盖会连错别人家）
def code_only(t):
    # 去掉 // 与 /* */ 注释后再查（注释里正是把它当反例写的）
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)
ok('myradio/pin/' not in code_only(cp) and 'myradio/pin/' not in code_only(cj),
   '没有另开 myradio/pin/ 主题（retain 单条会被同码机器互相覆盖）')
ok('"/pin/"' not in code_only(cp) and "'/pin/'" not in code_only(cj),
   '代码里也没有拼过 /pin/ 这种路径')
b = rd('tools', 'mqtt_broker_sim.js')
ok("pin: '1234'" in b and "pin: '5678'" in b, '模拟器的两台假机器码不同（能验"唯一命中"）')
ok('pin: d.pin || ' in b, '模拟器广播里也带 pin')

# ============================================================ C. 远程版入口
print('\n[C] 远程版网页：入口换成 4 格')
gr = rd('tools', 'gen_remote.py')
ok('id="rmPin"' in gr, '有一格 4 位输入（#rmPin）')
ok('maxlength="4"' in gr, '限 4 位')
ok('CLOUD.pinLogin(v)' in gr, '输满就调 pinLogin')
ok("if (r.n === 1){" in gr.replace(' ', '') or 'if (r.n === 1)' in gr, '唯一命中直接连，不用再点')
ok("if (r.n > 1)" in gr, '多台同码列出来让用户挑')
ok('CLOUD.anyPin()' in gr, '0 台时区分「没上线」和「老固件不报码」')
ok('<details id="rmMore">' in gr, '12 位遥控码收进折叠（兜底还在，但不挡路）')
ok('id="rmIn"' in gr, '12 位输入框还在（老固件/关免配对的机器仍可用）')
ok('CLOUD.enable(true);\n\n  var t0' in gr or 'CLOUD.enable(true)' in gr, '一进页面就连接收广播')

print('\n[D] App「我的机器」里也能输码')
bd = rd('tools/app', 'body.html')
pj = rd('tools/app/js', 'pages.js')
cs = rd('tools/app', 'style.css')
ok('id="devPin"' in bd, 'body.html 有 4 位码输入行（#devPin）')
ok('id="devPinGo"' in bd, '有一个「连接」按钮（不是只能回车）')
ok('function devPinGo' in pj, 'pages.js 有 devPinGo()')
ok('CLOUD.pinLogin(v)' in pj, 'App 里调的是同一个 pinLogin（逻辑只有一份）')
ok('initDevPin();' in pj, 'initDevPin 在 bindPages 里被调用（否则输框是死的）')
ok('INIT.push(bindPages)' in pj and '  initDevPin();' in pj, 'initDevPin 在那次绑定里，不会重复绑定')
ok('renderDevs(r.list)' in pj, '多台同码时只把这几台留在列表里')
ok('CLOUD.anyPin()' in pj, 'App 里也区分「没找到」和「老固件」')
ok('.pinrow input' in cs, 'style.css 有拟物凹陷的输码样式')
ok('box-shadow:var(--sh-in)' in cs.split('.pinrow input')[1][:400], '输入框用凹陷阴影（可输入的语言）')
ok('esc(d.pin)' in pj and 'pn' in pj, '列表里显示每台的 4 位码（两台同名时能分清）')
wa = rd('src', 'webapp.h')
ok('id="devPin"' in wa, 'webapp.h 里已经带上了（机器发给手机的页面）')

rm = rd('MyRadio-远程版.html')
ok('id="rmPin"' in rm, '生成的远程版里真的有 4 格输入')
ok('id="rmMore"' in rm, '生成的远程版里有折叠的 12 位兜底')
ok('pinLogin' in rm, '生成的远程版里调了 pinLogin')
ok(rm.count('</body>') == 1, '远程版仍然只有一个 </body>（没把 App 内嵌的 HTML 算进去）')

print('\n通过 %d / 失败 %d' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
