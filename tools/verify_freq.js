// ============================================================
//  verify_freq.js —— 调谐盘「台名里那个频率」的回归
//
//  三份实现必须给出同一个答案：
//    ① src/ui.cpp            nameFreq10()   （真机，C++）
//    ② tools/app/js/core.js  nameFreq10()   （手机 App 镜像）
//    ③ tools/preview_template.html          （预览页的固件模拟器）
//
//  用法：node tools/verify_freq.js
// ============================================================
const fs = require('fs');
const path = require('path');
const ROOT = path.dirname(__dirname);

let pass = 0, fail = 0;
function ok(cond, msg) {
  if (cond) { pass++; }
  else { fail++; console.log('  FAIL ' + msg); }
}

// ---------- 从 core.js 抠出函数 ----------
function grabJS(file, startMark, endMark) {
  const s = fs.readFileSync(path.join(ROOT, file), 'utf8');
  const a = s.indexOf(startMark);
  const b = s.indexOf(endMark, a);
  if (a < 0 || b < 0) throw new Error(file + ' 找不到 ' + startMark);
  return new Function(s.slice(a, b) + '\nreturn nameFreq10;')();
}

const fApp = grabJS('tools/app/js/core.js', 'function nameFreq10', '// 信号柱（4 格）');

// 预览页要连 S / clamp 一起给（stationFreq10 依赖 S.list）
const prevSrc = fs.readFileSync(path.join(ROOT, 'tools', 'preview_template.html'), 'utf8');
const pa = prevSrc.indexOf('function freq10');
const pb = prevSrc.indexOf('// 这台在盘上指到哪');
const pc = prevSrc.indexOf('function drawTuner');
const prevCode = prevSrc.slice(pa, pc);
const fPrev = new Function('S', 'clamp', prevCode + '\nreturn {nameFreq10, stationFreq10};');

// ---------- 台表 ----------
function loadStations() {
  const s = fs.readFileSync(path.join(ROOT, 'src', 'preset_stations.h'), 'utf8');
  const st = s.match(/kPreset\[\] = \{([\s\S]*?)\n\};/)[1];
  const list = [];
  const re = /\{\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}/g;
  let m;
  while ((m = re.exec(st))) list.push({ sid: m[1], n: m[2] });
  // 1.15.0：kPresetGrp 已经不存在了（不再分组），整张表就是一个列表。
  // 顺序 = 播放顺序，调谐盘的「按序号摊频率」也照这张表的序号算。
  return { list };
}
const { list } = loadStations();

// ---------- ① 定点用例 ----------
const cases = [
  ['FM89.1吴江综合广播', 891],
  ['渭南广播FM90.9', 909],
  ['FM94.9包头综合广播', 949],
  ['中江综合广播994', 994],
  ['兵团882综合广播', 882],
  ['拉萨人民广播电台914', 914],
  ['泉州台889新闻综合广播', 889],
  ['北京大兴人民广播电台FM986', 986],
  ['常熟广播声动1008', 1008],
  ['晋城新闻综合广播FM1072', 1072],
  ['1042南阳新闻广播', 1042],
  ['惠州综合广播FM100', 1000],     // 三位数落不进波段 → 按整兆赫再试一次
  ['上海经典947', 947],
  ['AM1161无锡新闻综合广播', -1],   // 中波，不认
  ['上下五千年 001', -1],           // 集数，不认
  ['苏州新闻广播', -1],             // 没写频率
  ['', -1],
  ['FM108.5超波段', -1],
];
for (const [n, want] of cases) {
  const got = fApp(n);
  ok(got === want, '定点 ' + n + ' → ' + got + '（期望 ' + want + '）');
}

// ---------- ② 271 个台名，App 版与预览版结果必须完全一致 ----------
let hit = 0, mismatch = 0;
for (const it of list) {
  const a = fApp(it.n);
  const b = fPrev({ list: [{ n: it.n }] }, (v, lo, hi) => Math.max(lo, Math.min(hi, v))).nameFreq10(it.n);
  if (a !== b) { mismatch++; if (mismatch < 4) console.log('  FAIL 两处不一致：' + it.n + ' app=' + a + ' preview=' + b); }
  if (a > 0) hit++;
}
ok(mismatch === 0, 'core.js 与 preview 的 nameFreq10 在 ' + list.length + ' 个台名上不一致 ' + mismatch + ' 处');
ok(hit === 65, '解析出真实频率的台数 ' + hit + '（期望 65）');

// 所有解析结果都要落在 64.0~108.0
let outRange = 0;
for (const it of list) {
  const f = fApp(it.n);
  if (f > 0 && (f < 640 || f > 1080)) outRange++;
}
ok(outRange === 0, '有 ' + outRange + ' 个频率落在波段外');

// ---------- ③ 「FM89.1吴江综合广播」在盘上指到 89.1 ----------
// 序号不写死：台表一变（加中央台 / 并组）它就挪。真正的不变量是「它算出来的频率 = 89.1」。
const idx = list.findIndex(x => x.sid === '5022050');
ok(idx >= 0, '吴江综合广播在台表里（第 ' + (idx + 1) + ' 台）');

const win = { list };
const r = fPrev(win, (v, lo, hi) => Math.max(lo, Math.min(hi, v)));
const fcur = r.stationFreq10(idx, win.list.length);
ok(fcur === 891, '盘面中心频率 ' + fcur + '（期望 891，即 89.1）');

// 指针在 DIAL_CX=160；88.0 的刻度在 (880-859)*5=105 → 之间正好 1.1MHz
const DIAL_CX = 160, DIAL_PX = 5;
const left = fcur - Math.round(DIAL_CX / DIAL_PX);
ok((880 - left) * DIAL_PX === 105, '88.0 的刻度 x=' + (880 - left) * DIAL_PX + '（期望 105）');
ok((fcur - left) * DIAL_PX === DIAL_CX, '89.1 的刻度 x=' + (fcur - left) * DIAL_PX + '（期望 160 = 指针）');
ok(((fcur - 880) * DIAL_PX) === 55, '指针离 88.0 有 ' + (fcur - 880) * DIAL_PX + 'px（11 格 = 1.1MHz）');

// 没有台名频率的台，仍然按序号摊到 87.5~108.0（行为不变）—— 表大小从真台表取，不写死。
// 注意：摊的是「整张表的序号」（1.15.0 起只有一张表），所以首尾要用「第一个 / 最后一个
// **没写频率**的台」来断言，不能直接拿第 0 台和最后一台 —— 万一最后一台台名里带频率，
// stationFreq10 会返回那个频率，断言就假失败。
const nG = win.list.length;
const free = i => fApp(list[i].n) === -1;
if (free(0)) ok(r.stationFreq10(0, nG) === 875, '第一台（没写频率）仍是 87.5');
else console.log('  跳过「第一台 87.5」：第 1 台台名里带频率 ' + list[0].n);
let lastFree = nG - 1;
while (lastFree > 0 && !free(lastFree)) lastFree--;
ok(r.stationFreq10(lastFree, nG) === 1080,
   '最后一个没写频率的台（第 ' + (lastFree + 1) + ' 台）仍是 108.0，实得 ' + r.stationFreq10(lastFree, nG));

// 整表：盘面频率必须落在 87.5~108.0，且不能有 NaN
let badF = 0;
for (let i = 0; i < nG; i++) {
  const f = r.stationFreq10(i, nG);
  if (!(f >= 875 && f <= 1080)) { badF++; if (badF < 4) console.log('  越界：第 ' + (i + 1) + ' 台 ' + list[i].n + ' -> ' + f); }
}
ok(badF === 0, '整表 ' + nG + ' 台的盘面频率都在 87.5~108.0（越界 ' + badF + ' 个）');

console.log('\n通过 ' + pass + ' / 失败 ' + fail);
process.exit(fail ? 1 : 0);
