// ============================================================
//  verify_power.js —— 电源/电流保护的回归（1.21.0）
//
//  这一段是"读源码 + 镜像逻辑"的验证，不需要板子、不需要编译器：
//
//    A. 配置自洽：温度三档递增、回滞不至于负值、各档上限递减、
//       满电判定有回滞（DETECT > CLEAR）、过压阈值在满电之上。
//    B. 逻辑镜像：把 power.cpp 的 lvlStep()（带迟滞的档位推进）和
//       powerVolCap / powerBrightCap 用 JS 重写一遍，喂温度序列进去，
//       检查「不会在阈值附近抖」和「取更狠的那个」。
//    C. 接线检查：applyVol 真的夹了 powerVolCap、uiLoop 真的夹了
//       powerBrightCap、main 真的调了 powerBegin/powerTick、
//       battery 真的算了 full/ext —— 少一处这功能就是死的。
//
//  用法：node tools/verify_power.js
// ============================================================
const fs = require('fs');
const path = require('path');
const ROOT = path.dirname(__dirname);

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const read = p => fs.readFileSync(path.join(ROOT, p), 'utf8');

// ============================================================
//  1. config.h
// ============================================================
console.log('[1] config.h 的电源保护参数');
const cfg = read('src/config.h');
const num = k => {
  const m = new RegExp('#define\\s+' + k + '\\s+(-?[0-9.]+)').exec(cfg);
  return m ? parseFloat(m[1]) : NaN;
};

const PWR_ENABLE = num('PWR_ENABLE');
const T1 = num('PWR_T1_C'), T2 = num('PWR_T2_C'), T3 = num('PWR_T3_C');
const HYST = num('PWR_HYST_C');
// 1.46.6：这几个上限 1.44.0 已改成「百分比」口径（PWR_*_VOL_PCT）——
//   两路音量量程不同（FM 0..63 / 网络 0..21），绝对值上限没意义。
//   老名字留作回退，免得以后再翻一次历史。
const pnum = k => { const v = num(k + '_PCT'); return isNaN(v) ? num(k) : v; };
const T1V = pnum('PWR_T1_VOL'), T1B = num('PWR_T1_BRIGHT');
const T2V = pnum('PWR_T2_VOL'), T2B = num('PWR_T2_BRIGHT');
const FULLV = pnum('PWR_FULL_VOL'), FULLB = num('PWR_FULL_BRIGHT');
const DETECT = num('BATT_FULL_DETECT_MV'), CLEAR = num('BATT_FULL_CLEAR_MV');
const RECHG = num('BATT_RECHG_MV'), EXTMV = num('BATT_EXT_MV');
const OVERV = num('BATT_OVERV_MV'), FULLMV = num('BATT_FULL_MV');
const FASTV = num('VBUS_FAST_MIN'), V5MIN = num('VBUS_5V_MIN'), V5MAX = num('VBUS_5V_MAX');

ok(PWR_ENABLE === 1, 'PWR_ENABLE 默认开着');
ok(T1 < T2 && T2 < T3, '温度三档递增 ' + T1 + ' < ' + T2 + ' < ' + T3);
ok(T1 >= 40 && T1 <= 58, '一档阈值 ' + T1 + '℃ 在 40~58（太低会误降载）');
ok(T3 <= 75, '过热暂停阈值 ' + T3 + '℃ ≤75（再高就该硬件保护了）');
ok(HYST >= 2 && HYST <= 8, '回滞 ' + HYST + '℃ 在 2~8（太小抖、太大反应慢）');

ok(T2V < T1V && T1V <= 100 && T2V >= 20,
   '音量上限逐档递减：' + T2V + '% < ' + T1V + '%（一档可以是 100% = 只降亮度，1.46.0 定的）');
ok(T1B < 100 && T2B < T1B, '亮度上限逐档递减：' + T2B + ' < ' + T1B + ' < 100');

// 满电档必须比温度档"温和"：用户一直插着电，不能让他觉得音量上不去
// 1.46.0 起一档温度降载只降亮度（音量 100%），所以「满电比一档温和」已经不成立。
//   这条今天该守的是：满电只是轻微压一下，绝不能压到二档那么狠 ——
//   用户是**插着电**听，凭什么比常温电池供电还小声。
ok(FULLV >= T2V && FULLV >= 85,
   '满电音量上限(' + FULLV + '%) 不压到二档(' + T2V + '%) 那么狠 —— 插电听不该被压太狠');
ok(FULLV >= 85, '满电音量上限 ' + FULLV + '% ≥85%（否则用户会觉得"音量上不去"）');
ok(FULLB < 100, '满电时亮度确实压了（' + FULLB + '）');

// 满电判定要有回滞，否则充电 IC 脉冲补电就会来回抖
ok(DETECT > CLEAR, '满电判定有回滞：判定 ' + DETECT + 'mV > 取消 ' + CLEAR + 'mV');
ok(DETECT < FULLMV + 60, '满电判定 ' + DETECT + ' 不高于标称满电 ' + FULLMV + ' 太多');
ok(RECHG >= 3900 && RECHG < DETECT, '真停充的回充阈值 ' + RECHG + ' 在 3.9V~判定值之间');
ok(CLEAR > RECHG, '取消满电(' + CLEAR + ') 在回充阈值(' + RECHG + ') 之上，两套逻辑不打架');
ok(EXTMV < DETECT, '外接电源判定(' + EXTMV + ') 低于满电判定 —— 先认出"插着电"再认出"满了"');
// 1.46.65：config.h 里不许有同名宏定义两次。
//   吃过一次亏：1.46.64 新加的那组 BATT_FULL_DETECT_MV 4150 / BATT_FULL_CLEAR_MV 4050
//   / BATT_EXT_MV 4110 被老的一组（4180 / 4080 / 4120）静默覆盖，编译只报一句
//   "redefined"，实际生效的全是旧值 —— 而 num() 读的是*第一个*，回归还绿着。
//   这条直接数出现次数，重复就红。
['BATT_FULL_DETECT_MV', 'BATT_FULL_CLEAR_MV', 'BATT_EXT_MV', 'BATT_EXT_HOLD_MS',
 'BATT_RECHG_MV', 'BATT_FULL_HOLD_MS', 'FVOL_MAX', 'PWR_FULL_VOL_PCT'].forEach(k => {
  const n = (cfg.match(new RegExp('^#define\\s+' + k + '\\b', 'gm')) || []).length;
  ok(n === 1, 'config.h 里 ' + k + ' 只定义一次（实际 ' + n + ' 次；重复会静默覆盖）');
});

ok(OVERV > FULLMV, '过压 ' + OVERV + 'mV 高于满电 ' + FULLMV + 'mV');
ok(OVERV <= 4350, '过压阈值 ≤4.35V（单节锂电安全上限附近）');
ok(V5MIN < V5MAX && V5MAX < FASTV, 'USB 电压分档有序：5V 区间 ' + V5MIN + '~' + V5MAX + ' < 快充 ' + FASTV);

// ============================================================
//  2. 逻辑镜像（照 power.cpp 抄）
// ============================================================
console.log('\n[2] 档位推进与上限（镜像 power.cpp）');
const lvlOf = t => (t >= T3 ? 3 : t >= T2 ? 2 : t >= T1 ? 1 : 0);
function lvlStep(cur, t) {
  const raw = lvlOf(t);
  if (raw > cur) return raw;
  if (raw < cur) {
    const th = (cur === 3) ? T3 : (cur === 2) ? T2 : T1;
    if (t < th - HYST) return raw;
    return cur;
  }
  return cur;
}
const volCap = (lvl, full) => {
  let c = 100;
  if (full) c = FULLV;
  if (lvl >= 2) c = Math.min(c, T2V);
  if (lvl >= 1) c = Math.min(c, T1V);
  return c;
};
const brightCap = (lvl, full) => {
  let c = 100;
  if (full) c = FULLB;
  if (lvl >= 2) c = Math.min(c, T2B);
  if (lvl >= 1) c = Math.min(c, T1B);
  return c;
};

ok(lvlOf(T1 - 1) === 0 && lvlOf(T1) === 1, '刚过一档阈值就进一档');
ok(lvlOf(T2) === 2 && lvlOf(T3) === 3, '二档/三档阈值准确');
ok(volCap(0, false) === 100 && brightCap(0, false) === 100, '正常状态不限制（100/100）');
ok(volCap(3, false) === T2V, '过热档沿用二档音量上限（暂停播放时音量无所谓，但不能更宽）');
ok(volCap(0, true) === FULLV, '满电 + 不热 = 只用满电上限');
ok(volCap(1, true) === Math.min(FULLV, T1V), '满电 + 一档 = 取更狠的');
ok(brightCap(2, true) === Math.min(FULLB, T2B), '满电 + 二档 = 取更狠的（亮度）');

// 回滞：在 T1 附近上下抖 1℃，档位不该跟着抖
{
  let lvl = 0, changes = 0;
  const seq = [];
  for (let i = 0; i < 40; i++) seq.push(T1 + (i % 2 ? 1 : -1));   // T1±1 反复
  for (const t of seq) { const n = lvlStep(lvl, t); if (n !== lvl) changes++; lvl = n; }
  ok(changes === 1, '在 T1±1℃ 抖 40 次只切换 1 次（回滞有效，实际 ' + changes + ' 次）');
}
// 升温要立刻跟上
{
  let lvl = 0;
  lvl = lvlStep(lvl, T3 + 2);
  ok(lvl === 3, '直接从常温跳到 ' + (T3 + 2) + '℃ → 立刻到三档');
}
// 降温要等回滞
{
  let lvl = 3;
  ok(lvlStep(lvl, T3 - 1) === 3, '刚降到阈值下 1℃ 还不退（回滞 ' + HYST + '℃）');
  ok(lvlStep(lvl, T3 - HYST - 1) === lvlOf(T3 - HYST - 1), '降到阈值下 ' + (HYST + 1) + '℃ 才退档');
}
// 单向降温：3 → 2 → 1 → 0 一路走完
{
  let lvl = 3;
  for (const t of [T3 - 5, T2 - 5, T1 - 5, 30]) lvl = lvlStep(lvl, t);
  ok(lvl === 0, '一路降到 30℃ 能回到 0 档（实际 ' + lvl + '）');
}

// ============================================================
//  3. 接线检查（漏一处这功能就是死的）
// ============================================================
console.log('\n[3] 源码接线');
const audio = read('src/audio.cpp');
const ui = read('src/ui.cpp');
const mainc = read('src/main.cpp');
const battc = read('src/battery.cpp');
const pc = read('src/power.cpp');
const netc = read('src/net.cpp');

// 1.44.0 起 applyVol 夹的是 netVolCap()（内部再乘 powerVolCapPct()/100）——
//   直接夹百分比会把 6..21 那一路夹坏，所以别再要求 applyVol 里出现 powerVolCap()。
ok(/applyVol\(\)\s*\{[\s\S]{0,400}netVolCap\(\)/.test(audio), 'audio.cpp 的 applyVol 夹了 netVolCap()');
ok(/static int netVolCap\(\)[\s\S]{0,200}powerVolCapPct\(\)/.test(audio),
   'netVolCap() 里真的乘了 powerVolCapPct()（否则电源保护对网络那一路是空的）');
ok(audio.includes('audioRefreshVol()'), 'audio.cpp 提供 audioRefreshVol()');
ok(!/s_audio\.setVolume\(toI2S\(A\.vol\)\)/.test(audio), 'audio.cpp 里没有绕过上限的裸 setVolume(A.vol)');
ok(/powerBrightCap\(\)/.test(ui), 'ui.cpp 的亮度应用走 powerBrightCap()');
ok(/powerBegin\(\)/.test(mainc) && /powerTick\(\)/.test(mainc), 'main.cpp 调了 powerBegin() 和 powerTick()');
ok(/FVOL_MAX\s*\*\s*powerVolCapPct\(\)\s*\/\s*100/.test(mainc),
   'main.cpp 的 FM 音量也按 powerVolCapPct() 折算（FVOL_MAX * pct / 100）');
ok(/batteryFull\s*\(\)/.test(battc) && /batteryOnExt\s*\(\)/.test(battc), 'battery.cpp 实现了 batteryFull / batteryOnExt');
// 1.46.64 把电量判据改成了状态机：放电态 = ST_DISC，插着电 = s_ext。
//   老版本是 s_chg / s_ext 两个 bool，这里两个写法都认。
ok(/s_st\s*==\s*ST_DISC\s*&&\s*!s_ext/.test(battc) ||
   /!s_chg && !s_ext/.test(battc), 'battery.cpp 的自动关机判据排除了「插着电」的情况');
ok(/powerJson\(o\)/.test(netc), 'net.cpp 的 /api/batt 带上了 powerJson');

// 过压必须能触发停充（接了使能脚的情况）
ok(/BATT_OVERV_MV/.test(pc), 'power.cpp 里有过压判定');
ok(/batteryFull\(\) \|\| mv >= BATT_OVERV_MV/.test(pc), '停充条件包含「满电或过压」');

// 温度暂停不能把用户的暂停状态搞乱：用的是 audioStop 而不是 audioPauseToggle
ok(/s_lvl >= 3[\s\S]{0,300}audioStop\(\)/.test(pc), '过热暂停用 audioStop()（不碰用户自己的暂停态）');

// 快充检测三件都在
ok(/PIN_VBUS_ADC/.test(pc), 'power.cpp 里有 USB 电压读取（可选硬件）');
ok(/s_rateMvpm/.test(pc), 'power.cpp 里有充电速率估算（不用硬件）');
ok(/BATT_CHG_MAX_MS/.test(pc), 'power.cpp 里有充电超时提示（不用硬件）');

// ============================================================
//  4. 安全性：不能被配置搞成"永远降载"
// ============================================================
console.log('\n[4] 安全性');
ok(T1 > 40, '一档阈值 ' + T1 + '℃ 不会在室温附近就触发（夏天机器本来就 40℃ 上下）');
ok(FULLV === 100 || FULLV >= 85, '满电音量上限 ' + FULLV + '% 不低于 85% —— 老人会觉得"声音变小了"');
ok(DETECT >= 4150, '满电判定 ' + DETECT + 'mV 不会在充到一半时就误判（4.15V 以上才可能到 4.2V）');

console.log('\n通过 ' + pass + ' / 失败 ' + fail);
process.exit(fail ? 1 : 0);
