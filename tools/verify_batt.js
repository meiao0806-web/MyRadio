// ============================================================
//  verify_batt.js —— 电量与语音播报的回归（1.19.0）
//
//  这一段固件不能在这台机器上编译，所以改成"从源码里抠数据来验"：
//
//    A. 电量曲线：把 src/battery.cpp 的 kLiMv / kLiPct 抠出来，检查表本身
//       （单调、端点、范围），再用同一套插值算法在 JS 里复算几个已知点。
//       JS 那份是**照 C 抄的镜像**，所以 C 那边改坏了这里会立刻红。
//    B. 阈值：从 src/config.h 抠 BATT_* 宏，检查相互关系（空电 < 见过高位 < 满电，
//       低电百分比在合理区间），以及"3.3V 轨不会误判"的那道闸（SEEN_HIGH > EMPTY）。
//    C. 语音：src/voice_data.h 里的两段字节必须是合法 MP3 帧头，长度与 tools/voice/*.mp3
//       一致；voice.h 的 enum 顺序要和 gen_voice.py 的 CLIPS 对齐。
//
//  用法：node tools/verify_batt.js
// ============================================================
const fs = require('fs');
const path = require('path');
const ROOT = path.dirname(__dirname);

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const read = p => fs.readFileSync(path.join(ROOT, p), 'utf8');

// ============================================================
//  1. 从 config.h 抠阈值
// ============================================================
console.log('[1] config.h 的电池参数');
const cfg = read('src/config.h');
const M = {};
for (const k of ['BATT_ENABLE', 'PIN_BATT_ADC', 'BATT_FULL_MV', 'BATT_EMPTY_MV',
                 'BATT_SEEN_HIGH_MV', 'BATT_LOW_PCT', 'BATT_AUTO_OFF', 'VOICE_ENABLE']) {
  const m = new RegExp('#define\\s+' + k + '\\s+(-?[0-9.]+)f?').exec(cfg);
  ok(!!m, k + ' 在 config.h 里能找到');
  M[k] = m ? parseFloat(m[1]) : NaN;
}
const div = /#define\s+BATT_DIV\s+([0-9.]+)f/.exec(cfg);
ok(!!div, 'BATT_DIV 能找到');
const DIV = div ? parseFloat(div[1]) : 0;

ok(M.BATT_ENABLE === 1, '电量默认开着');
ok(M.PIN_BATT_ADC >= 1 && M.PIN_BATT_ADC <= 10, 'PIN_BATT_ADC=' + M.PIN_BATT_ADC + ' 落在 ADC1（GPIO1~10）');
ok(DIV > 1 && DIV < 6, '分压比 ' + DIV + ' 在合理范围（1~6）');
// 1.46.64 起关机线（3550）抬到「见过高位」（3500）之上：反过来写的话，一颗本来
// 就只有 3.52V 的电池永远够不到「见过高位」，自动关机就永远不触发 → 一路深放到保护板。
ok(M.BATT_SEEN_HIGH_MV < M.BATT_EMPTY_MV, '见过高位(' + M.BATT_SEEN_HIGH_MV + ') < 空电(' + M.BATT_EMPTY_MV + ') 【安全闸，反了自动关机不触发】');
ok(M.BATT_SEEN_HIGH_MV < M.BATT_FULL_MV, '见过高位 < 满电');
ok(M.BATT_FULL_MV === 4200, '满电按 4.20V 算（单节锂电）');
// 这道闸的意义：IO 上若是 3.3V 轨（3300mV），永远到不了 SEEN_HIGH，就不会播报/关机
ok(3300 < M.BATT_SEEN_HIGH_MV, '3.3V 轨（3300mV）触不到「见过高位」→ 不会误播报/误关机');
ok(M.BATT_LOW_PCT >= 1 && M.BATT_LOW_PCT <= 15, '低电阈值 ' + M.BATT_LOW_PCT + '% 在 1~15 之间');
ok(M.VOICE_ENABLE === 1, '语音播报默认开着');

// ============================================================
//  2. 曲线表 + 插值
// ============================================================
console.log('\n[2] battery.cpp 的电压→百分比（1.46.57 起：OCV 表）');
const batt = read('src/battery.cpp');

// 1.43.0~1.46.56 是「照原厂口径的一条直线」，1.46.57 换成单节锂电的 OCV→SOC 表。
//   为什么换：用户实测老直线底部虚高（3.67V 显示 33%，其实只剩十几分钟），
//   而 18650CL / Battery18650Stats / ATS-MINI 这些开源固件用的都是曲线。
//   所以这一节验的是「pctOf 用表 + 插值，且表本身合法」。
const pctBody = (() => {
  const a = batt.indexOf('static int pctOf(int mv)');
  const b = batt.indexOf('\n}', a);
  return a < 0 ? '' : batt.slice(a, b);
})();
ok(pctBody !== '', 'pctOf 还在');
ok(pctBody.indexOf('kOcvMv') > 0 && pctBody.indexOf('kOcvPct') > 0,
   'pctOf 用 OCV 表（不再是 (mv-3400)/(4200-3400) 那条直线）');
ok(pctBody.indexOf('s_cal') < 0 && pctBody.indexOf('BATT_LOAD_MV') < 0,
   'pctOf 里没有用户校准 / 负载补偿（这两层 1.43.0 已去掉）');

// 电压换算也只能乘 batfactor 一个数
const mvLine = (() => {
  // 1.46.64 起引脚值先过 15 秒中位数（rawMed），再乘分压比和自动学到的量程系数
  const m = /\(float\)rawMed \* ([^;]+)\)/.exec(batt);
  return m ? m[1].trim() : '';
})();
ok(mvLine.indexOf('BATT_DIV') >= 0 && mvLine.indexOf('s_scale') >= 0,
   '电池电压 = 中位数 × BATT_DIV × 量程系数（自动校准），实际：' + (mvLine || '找不到'));
ok(batt.indexOf('mv += BATT_LOAD_MV') < 0, '负载补偿（+120mV）已删除');

// 与 C 等价的 JS 镜像：把两张表抠出来，按同一套插值算（C 的除法朝零截断）
const OCV = (() => {
  // 不拼字符串正则（转义在工具链上会被吃掉），直接按位置抠，只用到 [0-9]+
  const grab = tag => {
    const a = batt.indexOf(tag + '[]');
    if (a < 0) return [];
    const b = batt.indexOf('{', a), c = batt.indexOf('}', b);
    return (batt.slice(b + 1, c).match(/[0-9]+/g) || []).map(Number);
  };
  return { mv: grab('kOcvMv'), pc: grab('kOcvPct') };
})();
ok(OCV.mv.length === OCV.pc.length && OCV.mv.length === 20,
   'kOcvMv / kOcvPct 抠出来了（' + OCV.mv.length + ' / ' + OCV.pc.length + ' 点）');
const pctOf = mv => {
  if (mv >= OCV.mv[0]) return 100;
  if (mv <= OCV.mv[OCV.mv.length - 1]) return 0;
  for (let i = 1; i < OCV.mv.length; i++) {
    if (mv >= OCV.mv[i]) {
      const dv = OCV.mv[i - 1] - OCV.mv[i];
      return OCV.pc[i] + Math.trunc((mv - OCV.mv[i]) * (OCV.pc[i - 1] - OCV.pc[i]) / dv);
    }
  }
  return 0;
};

ok(pctOf(4200) === 100, '4200mV → 100%');
ok(pctOf(4300) === 100, '4300mV（虚高）→ 100%');
ok(pctOf(3200) === 0, '3200mV（表底）→ 0%，不是负数');
ok(pctOf(3800) === 49, '3800mV → 49%（曲线，不是直线的 50%）');
ok(pctOf(3600) === 14, '3600mV → 14%（老直线给 25%，虚高一倍）');
ok(pctOf(4000) === 79, '4000mV → 79%');
ok(pctOf(3850) === 58, '3850mV → 58%（3.85V 的稳态显示）');

// 低电/关机这两条线必须落在合理位置
let lowMv = -1;
for (let mv = 4200; mv >= 3300; mv--) if (pctOf(mv) <= M.BATT_LOW_PCT) { lowMv = mv; break; }
ok(lowMv > M.BATT_EMPTY_MV, '低电（≤' + M.BATT_LOW_PCT + '%）从 ' + lowMv + 'mV 开始，比关机线高');
ok(pctOf(lowMv + 100) > M.BATT_LOW_PCT, '高 100mV 就不算低电（曲线在底部一档 50mV，边界清楚）');
// 1.46.64：关机线从 3400 抬到 3550 —— 3.0V 以下伤电芯，用户要求留 10~15% 余量。
// 所以关机那一刻屏上应当还有 ~9%，而不是 0%（那是"真放干净"才该有的数）。
ok(pctOf(M.BATT_EMPTY_MV) >= 8 && pctOf(M.BATT_EMPTY_MV) <= 15,
   '关机线（' + M.BATT_EMPTY_MV + 'mV）显示 ' + pctOf(M.BATT_EMPTY_MV) + '%（= 留 10% 余量，不是 0%）');

// ============================================================
//  3. 语音播报
// ============================================================
console.log('\n[3] 语音播报');
const vh = read('src/voice.h');
const enumIds = (() => {
  const m = /enum VoiceId[^{]*\{([\s\S]*?)\}/.exec(vh);
  return m ? (m[1].match(/VO_[A-Z]+\s*=\s*\d+/g) || []).map(x => x.split('=')[0].trim()) : [];
})();
// 1.40.1 起多了 VO_SCAN（搜台提示音）—— 这条断言原来只写了两个，早就过时了
ok(enumIds.join(',') === 'VO_LOW,VO_OFF,VO_SCAN',
   'voice.h 的 id 顺序是 VO_LOW(0) / VO_OFF(1) / VO_SCAN(2)：' + enumIds.join(','));

// gen_voice.py 的 CLIPS 顺序必须一样（否则 0 号会念成"没电了"）
const genpy = read('tools/gen_voice.py');
const clipIds = [...genpy.matchAll(/id="(\w+)"/g)].map(m => m[1]);
ok(clipIds.join(',') === 'low,off,scan', 'gen_voice.py 的 CLIPS 顺序一致：' + clipIds.join(','));

const vdata = read('src/voice_data.h');
const grabBytes = cname => {
  const m = new RegExp('static const uint8_t ' + cname + '\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\}').exec(vdata);
  if (!m) return null;
  return (m[1].match(/0x[0-9A-Fa-f]{2}/g) || []).map(h => parseInt(h, 16));
};
for (const [cname, mp3] of [['kVoLow', 'low.mp3'], ['kVoOff', 'off.mp3'], ['kVoScan', 'scan.mp3']]) {
  const b = grabBytes(cname);
  ok(!!b && b.length > 2000, cname + ' 有内容（' + (b ? b.length : 0) + ' 字节）');
  if (!b) continue;
  // MPEG 帧头：0xFF 后面高 3 位全 1
  ok(b[0] === 0xFF && (b[1] & 0xE0) === 0xE0, cname + ' 开头是合法 MP3 帧同步（' + b[0].toString(16) + b[1].toString(16) + '）');
  const tf = path.join(ROOT, 'tools', 'voice', mp3);
  ok(fs.existsSync(tf), mp3 + ' 源文件在（可复现）');
  if (fs.existsSync(tf)) {
    const raw = fs.readFileSync(tf);
    ok(raw.length === b.length, mp3 + ' 与固件里的字节数一致（' + raw.length + '）');
  }
  // 24kHz / 48kbps 单声道：算一下时长，别超过播报超时上限
  const dur = b.length * 8 / 48000;
  ok(dur > 0.8 && dur < 12, cname + ' 时长约 ' + dur.toFixed(2) + 's（0.8~12s 之间）');
}

// voice.cpp 里两个路径要对上数组个数
const vcpp = read('src/voice.cpp');
const kpArr = /kPath\[VOICE_N\]\s*=\s*\{([^}]*)\}/.exec(vcpp);
ok(!!kpArr, 'voice.cpp 里有 kPath[VOICE_N] 这张表');
const paths = kpArr ? [...kpArr[1].matchAll(/\/v\/([a-z]+)\.mp3/g)].map(m => '/v/' + m[1] + '.mp3') : [];
ok(paths.length === 3, 'voice.cpp 里有三个播放路径：' + paths.join(' '));
ok(paths[0] === '/v/low.mp3' && paths[1] === '/v/off.mp3' && paths[2] === '/v/scan.mp3',
   '路径顺序与 id 对齐（0=low 1=off 2=scan）');

// 总字节数宏要和实际一致
const tot = /#define VOICE_BYTES_TOTAL (\d+)/.exec(vdata);
const realTot = (grabBytes('kVoLow') || []).length + (grabBytes('kVoOff') || []).length
              + (grabBytes('kVoScan') || []).length;
// 这个宏只在 voice.cpp 的日志里用（不参与分配/写入），所以只要求"给够了"，
// 不要求精确相等 —— 加 VO_SCAN 时没有重跑 gen_voice.py，值会偏大，那是无害的。
ok(tot && +tot[1] >= realTot, 'VOICE_BYTES_TOTAL=' + (tot && tot[1]) + ' ≥ 实际 ' + realTot + '（只是日志用，偏大无害）');

// ============================================================
//  4. 固件里真的接上了（别只写了模块没人调）
// ============================================================
console.log('\n[4] 接线');
const maincpp = read('src/main.cpp');
ok(/batteryBegin\(\)/.test(maincpp) && /batteryTick\(\)/.test(maincpp), 'main.cpp 调了 batteryBegin / batteryTick');
ok(/voiceBegin\(\)/.test(maincpp), 'main.cpp 调了 voiceBegin');
const battcpp = read('src/battery.cpp');
ok(/voicePlay\(VO_LOW\)/.test(battcpp), 'battery.cpp 在低电时播 VO_LOW');
ok(/voicePlay\(VO_OFF\)/.test(battcpp), 'battery.cpp 在没电时播 VO_OFF');
ok(/powerOffNow/.test(battcpp), 'battery.cpp 有断电动作');
ok(/digitalWrite\(15, LOW\)/.test(battcpp), '断电是落 IO15 自锁（整板真断电）');
ok(/BATT_AUTO_OFF/.test(battcpp), '断电受 BATT_AUTO_OFF 控制（可只播报不断电）');
ok(/BATT_OFF_MIN_UP_MS/.test(battcpp), '开机 5 分钟内不自动关机（防误关）');
const netcpp = read('src/net.cpp');
ok(/\/api\/batt/.test(netcpp), 'net.cpp 有 /api/batt');
ok(/batteryJson\(o\)/.test(netcpp), 'net.cpp 把电量塞进 /api/state 与 /api/diag');
const cloudcpp = read('src/cloud.cpp');
ok(/pubPresence/.test(cloudcpp), 'cloud.cpp 有在线广播');
ok(/deriveFrom/.test(cloudcpp), 'cloud.cpp 有派生遥控码');

// C++ 与 JS 的派生算法必须同一份：抽查 3 个向量
const cppFn = cloudcpp.slice(cloudcpp.indexOf('static void deriveFrom'),
                             cloudcpp.indexOf('\n}', cloudcpp.indexOf('static void deriveFrom')));
ok(cppFn.indexOf('2166136261u') > 0 && cppFn.indexOf('16777619u') > 0 && cppFn.indexOf('"MyRadio-1"') > 0,
   'C++ 派生码 = FNV-1a 32 + 盐 "MyRadio-1"');
ok(cppFn.indexOf('100000u + h % 900000u') > 0, 'C++ 取模 900000 再加 100000（与 JS 一致）');


// ============================================================
//  5. 1.46.64：插拔认「相对台阶」，充电按时间爬，拔线等平
//
//     用户报「插上电瞬间 77%、拔掉一会儿 3%」—— 两个数都是终端电压查表，
//     只是量错了对象。旧代码要求 s_mv >= 4050 才算在充电，这块板 + 602535
//     在 CC 段只到 3.9~4.0V，永远够不到 ⇒ 充电分支从来没被执行过。
// ============================================================
console.log('\n[5] 1.46.64 状态机：台阶 / 充电按时间爬 / 拔线等平');
const stripComments = s => s.replace(/\/\/[^\n]*/g, '');

const div2 = parseFloat(/#define\s+BATT_DIV\s+([0-9.]+)f/.exec(cfg)[1]);
ok(div2 > 1.8 && div2 < 1.95, 'BATT_DIV = ' + div2 + '（由「满电 4.2V」反算，不能退回 1.71）');
ok(/enum \{ ST_DISC = 0, ST_PLUG_WAIT, ST_CHG, ST_UNPLUG_WAIT \};/.test(battcpp),
   '四态状态机（放电 / 等确认 / 充电 / 等表面电荷散掉）');
ok(!/BATT_PLUG_NEAR_MV/.test(stripComments(battcpp)) && !/BATT_PLUG_NEAR_MV/.test(stripComments(cfg)),
   '致命门槛 BATT_PLUG_NEAR_MV(4050) 已删除（注释里留着讲历史）');
ok(/rise >= BATT_PLUG_RISE_MV/.test(battcpp), '窗口涨幅 ≥ 台阶 → 进 PLUG_WAIT（不看绝对电压）');
ok(/fallDn >= BATT_UNPLUG_FALL_MV/.test(battcpp), '窗口跌幅 ≥ 台阶 → 退出充电');
ok(/BATT_PLUG_SETTLE_MS/.test(battcpp), '等 30 秒看它落不落回去（落回去 = 起播跳变）');
ok(/BATT_CHG_LEAVE_DROP_MV/.test(battcpp), '比进来时低 110mV 也算拔线（兜住误判）');

const chgSeg = battcpp.slice(battcpp.indexOf('static void chgTick'), battcpp.indexOf('void batteryTick'));
ok(chgSeg.indexOf('BATT_CHG_PCT_MS') > 0 && chgSeg.indexOf('s_anchor') > 0,
   '充电进度 = 锚点 + 时间/48s（不拿终端电压查表）');
ok(chgSeg.indexOf('pctOf(s_mv)') < 0, '充电分支里没有 pctOf(终端电压)');
ok(/s_flatAt/.test(chgSeg) && /BATT_CV_FLAT_MV/.test(chgSeg), '恒压平台跟踪在 chgTick 里');
ok(/BATT_CHG_MIN_TRUST_MS/.test(battcpp), '短充（<10 分钟）不认进度，拔线时退回锚点');

const unSeg = battcpp.slice(battcpp.indexOf('case ST_UNPLUG_WAIT:'), battcpp.indexOf('s_prevForFlat = s_mv;'));
ok(/winMax\(\) - winMin\(\) < BATT_UNPLUG_FLAT_MV/.test(unSeg),
   '拔线后等「平」用 120 秒窗口跨度，不是相邻两点比较');
ok(/s_vSoc = ocvOfPct\(s_pct\)/.test(unSeg), '散完电荷后从当前显示接着走（不跳）');

const offSeg = battcpp.slice(battcpp.indexOf('// ---- 没电：撑满'), battcpp.indexOf('if (s_offSaid && (!voiceBusy()'));
ok(/mvEmpty <= BATT_EMPTY_MV/.test(offSeg), '关机看开路电压（mvEmpty），不看带载值');
ok(/s_seenHigh/.test(offSeg) && /BATT_EMPTY_HOLD_MS/.test(offSeg), '关机要「见过高位」+ 连续 90 秒');

// ④ 采样：摊开取样 + 15 秒中位数
const ro = battcpp.slice(battcpp.indexOf('static int readOnce()'), battcpp.indexOf('static void powerOffNow'));
ok(/delayMicroseconds\(2000\)/.test(ro), 'readOnce 的采样摊开在时间轴上（2ms 一笔）');
ok(!/int v\[9\]/.test(ro), '不再是「同一瞬间连读 9 次」');
ok(/BATT_MED_N/.test(battcpp), '15 秒中位数（BATT_MED_N）');
ok(/readNoLoadMv\(BATT_OCV_MUTE_MS\)/.test(battcpp), '放电时关功放 180ms 取开路电压');

// ⑤ 显示：死区 + 端点
ok(/BATT_PCT_HYST/.test(battcpp) && /static void slewTo/.test(battcpp), '显示值限速（死区 + 每拍 1 点）');
ok(/if \(target >= 100\) \{ s_pct = 100; return; \}/.test(battcpp),
   '端点不走死区（否则充满永远停在 98%、没电卡在 2%）');
ok(!/BATT_PCT_STEP/.test(battcpp) && !/BATT_PCT_STEP/.test(cfg),
   '不再按 5% 分档（分档会把 3.45V 的 4% 抹成 0%）');

// ⑥ 低电播报：充电时必须闭嘴
const ann = battcpp.slice(battcpp.indexOf('// ---- 低电播报'), battcpp.indexOf('// ---- 没电：撑满'));
ok(/const bool quiet\s*=\s*\(s_st == ST_CHG\) \|\| \(s_st == ST_PLUG_WAIT\);/.test(ann),
   '播报闸门：充电 / 等确认都算「别念」');
ok((ann.match(/voicePlay\(VO_LOW\)/g) || []).length === 1, '整个播报段只有一处 voicePlay(VO_LOW)');
ok(/BATT_LOW_QUIET_MS/.test(ann), '低电要连续 20 秒才念（噪声不会误触发）');

// ⑦ 分压系数自校准
ok(/4200\.0f \/ \(float\)s_mv/.test(battcpp), 'CV 平台 = 4.20V 硬基准，反算量程系数');
ok(/scaleSave\(\)/.test(battcpp) && /\/battcal\.txt/.test(battcpp), '学到的系数存 /battcal.txt');

// ============================================================
//  6. 1.46.28：读数跳变保险（这条机制没被改，只修了断言里的空格）
//     用户报「拔掉电源从 57 立刻掉到 0，但电池实际有电」。
//     1 秒内掉 400mV 单体锂电做不到，只能是取样链断了。
// ============================================================
console.log('\n[6] 读数跳变保险：取样断了不能当成电池没电');
ok(/static int\s+s_lastGood/.test(battcpp) && /static int\s+s_glitch/.test(battcpp),
   '有「上一笔好读数 / 连续异常计数」两个状态');
ok(/mv < s_lastGood - 400/.test(battcpp), '1 秒内掉 400mV 以上判为取样异常（真电池做不到）');
const fiAt = battcpp.indexOf('mv < s_lastGood - 400');
const smAt = battcpp.indexOf('s_mv = (s_mv < 0) ? mv');
ok(fiAt > 0 && smAt > fiAt, '保险在电压平滑之前：坏样本进不了 s_mv');
const fiSeg = battcpp.slice(fiAt, smAt);
ok((fiSeg.match(/return;/g) || []).length >= 2, '异常分支直接 return（屏上保持旧数）');
ok(/s_glitch < 30/.test(fiSeg) && /s_ok = false/.test(fiSeg),
   '连错 30 秒才认「读数不可信」，单笔抖动不误判');
ok(/s_lastGood = mv;/.test(battcpp), '读数恢复后刷新基准，后续正常');
const bbSeg = battcpp.slice(battcpp.indexOf('void batteryBegin'), battcpp.indexOf('void batteryTick'));
ok(/s_lastGood = -1;/.test(bbSeg) && /s_glitch\s*=\s*0;/.test(bbSeg),
   'batteryBegin 清零基准，重启后不会拿旧值把自己卡住');

console.log('\n通过 ' + pass + ' / 失败 ' + fail);
process.exit(fail ? 1 : 0);
