// ============================================================
//  battery.cpp —— 电量（1.19.0 起，1.46.64 重写映射）
//
//  一段话说完这个文件：
//    每秒量一次 ADC（21 次摊开、掐头去尾再平均），换算成电池毫伏，做一次慢平滑；
//    然后先判「这一秒的电压算不算数」（插着电 / 带载 / 空载），
//    **只把干净的那一份**喂给 OCV 表算出百分比，显示值一律限速移动。
//
// ------------------------------------------------------------
//  1.46.64 为什么整个重写（用户在 1.46.63 上报的三句话）
//
//  用户原话：
//    「它没电关机后应该不是真的没电，只是图标显示没电。关机后开机，插上电，
//      电量瞬间变成 77；拔掉电后，一会儿就变成 3。电池是 3.7V 锂电池 602535。」
//
//  把这三句翻成电压就全对上了（这台机器的电芯是 602535 软包，3.7V/600mAh）：
//
//    插上电   → 终端被充电电流顶到 3.98V 左右 → 查表 = 77%   ← 就是那个 77
//    拔掉电   → 终端掉回 3.43V 左右 → 查表 = 3%              ← 就是那个 3
//
//  也就是说 **77% 和 3% 都是"量到的电压"，只是量错了对象的电压**。
//  1.46.55 其实已经写了「充电期间不看电压，从插电前的真实电量按时间往上爬」，
//  可它前面有一道闸：认定充电要求 `s_mv >= BATT_PLUG_NEAR_MV(4050)`。
//  这块板 + 这颗小电芯在 CC 段只到 3.9~4.0V，**永远够不到 4050**
//  → `s_chg` 一天到晚是 false → 那段"充电分支"从来没执行过 → 屏上还是跟着电压跑。
//  这解释了为什么 1.46.55/1.46.58 改了两轮都没好：改的那条路根本没被走到。
//
//  所以这一版换掉三个前提：
//
//    ① 充电/放电靠**相对台阶**认，不看绝对电压。
//       插上充电器终端会跳 +300~550mV（实测 1.46.55 记的 580mV 就是这么来的），
//       而负载跳变最大只到 122mV（实测起播那次）。用 170mV 当门槛，两边都分得开，
//       而且和分压比、和电芯都无关 —— 换个电池也不用重调。
//
//    ② 充电时**不用电压查表**，用「插电那一刻的真实电量 + 终端电压的涨幅」推。
//       锚点是屏上那个真值（刚刚还在放电，它是准的），之后每涨一毫伏才涨一点。
//       这样从 3% 插上电还是 3%，不会一秒变 77%。
//
//    ③ 显示值**限速**（每拍最多 1 点，差 3 点才动）。
//       任何一次判错最多让数字慢慢挪，不会"啪"地跳几十点。
//
//  外加两件针对这颗电芯的事：
//
//    · 关机线 3550mV（开路），不是 3300/3400。两头都要顾：
//      1.46.58 用 3400 关机，用户说"不是真的没电"；可反过来放到 3.0V 以下会
//      **永久损伤电芯**（铜集流体溶解 / 析锂），用户要求留 10~15% 余量。
//      3550mV 在表上就是 9%，带载时终端还有 3.4V 以上，离保护板 2.75~3.0V 很远。
//      判据仍然用**开路电压**（关功放量），所以不会把带载压降当成没电。
//      代价是"还能再听二十分钟"被主动放弃 —— 那 10% 换的是电芯寿命，值。
//    · 分压比自校准。这块板原理图是 100k/100k（比值 2.0），固件里是 1.875，
//      差 6% 就是 30 个点。没有万用表时唯一的硬基准是**充电器的 CV 平台**：
//      恒压段终端就是 4.20V。于是「在平台上稳稳停了 5 分钟」就反算一次系数，
//      存 /battcal.txt。判据严格（要平、要够高、要够久），不满足就一个字不改。
//
// ------------------------------------------------------------
//  为什么百分比用 OCV 表而不是直线：
//    锂电平台段（3.75~3.95V）电压几乎不动、电量却走了 35 个点，直线会把这一段
//    压扁成 25 个点，底部 3.5~3.4V 又只给 12 个点 —— 表现就是"中段半天不掉、
//    然后突然掉光"。表是单节锂电的通用放电曲线（同源用户桌面那份
//    Battery18650Stats 的 101 点表，这里抽成 50mV 一档 20 点）。
//    ⚠️ 表必须从高到低写、pct 单调不增；改完跑 tools/verify_batt_ocv.py（表）
//       和 tools/verify_batt_dbl.py（状态机 + 仿真）。
//
//  谁在用：
//    · ui.cpp    顶栏那个 48×20 的电量方块（batteryOk() 为假就画斜线框）
//    · power.cpp 满电/外接电判定，用来降亮度音量
//    · net.cpp   /api/batt、/api/state、cloud.cpp 跨网
// ============================================================
#include "battery.h"
#include "config.h"
#include "state.h"
#include "voice.h"
#include "audio.h"

#include <esp_sleep.h>
#include <LittleFS.h>
#include "store.h"

// ------------------------------------------------------------
//  单节锂电 OCV（开路电压）→ SOC
// ------------------------------------------------------------
static const uint16_t kOcvMv[] = {
  4200, 4150, 4100, 4050, 4000, 3950, 3900, 3850, 3800, 3750,
  3700, 3650, 3600, 3550, 3500, 3450, 3400, 3350, 3300, 3200
};
static const uint8_t kOcvPct[] = {
  100, 95, 90, 85, 79, 73, 66, 58, 49, 39,
   29, 21, 14,  9,  6,  4,  3,  2,  1,  0
};
static const int kOcvN = (int)(sizeof(kOcvMv) / sizeof(kOcvMv[0]));

// 电压 → 百分比（分段线性，C 的整除朝零截断）
static int pctOf(int mv) {
  if (mv >= kOcvMv[0]) return 100;
  if (mv <= kOcvMv[kOcvN - 1]) return 0;
  for (int i = 1; i < kOcvN; i++) {
    if (mv >= kOcvMv[i]) {
      const int v0 = kOcvMv[i - 1], v1 = kOcvMv[i];
      const int p0 = kOcvPct[i - 1], p1 = kOcvPct[i];
      return p1 + (mv - v1) * (p0 - p1) / (v0 - v1);
    }
  }
  return 0;
}

// 百分比 → 电压（pctOf 的反函数）。
//   用在"已知电量、反推此刻的开路电压"上：插电那一刻要把屏上的真实电量
//   换成一个电压基线，之后才好在电压域里做增量。
static int ocvOfPct(int pct) {
  if (pct >= 100) return kOcvMv[0];
  if (pct <= 0)   return kOcvMv[kOcvN - 1];
  for (int i = 1; i < kOcvN; i++) {
    if (pct >= kOcvPct[i]) {
      const int p0 = kOcvPct[i - 1], p1 = kOcvPct[i];
      const int v0 = kOcvMv[i - 1], v1 = kOcvMv[i];
      return v0 + (pct - p0) * (v1 - v0) / (p1 - p0);
    }
  }
  return kOcvMv[kOcvN - 1];
}

// ------------------------------------------------------------
//  状态
// ------------------------------------------------------------
enum { ST_DISC = 0, ST_PLUG_WAIT, ST_CHG, ST_UNPLUG_WAIT };

static int      s_pin      = -1;
static bool     s_ok       = false;   // 读数可信（顶栏据此决定画不画）
static bool     s_seenHigh = false;   // 本次上电见过 3550mV 以上（自动关机的安全闸）
static int      s_plausCnt = 0;
static int      s_mv       = -1;      // 平滑后的终端电压（mV）
static int      s_pct      = -1;      // 显示值（限速移动）
static uint32_t s_nextAt   = 0;
static uint32_t s_bootAt   = 0;

static int      s_rawMv    = -1;

// ---- 1.46.64：状态机 ----
static int      s_st       = ST_DISC;
static uint32_t s_stAt     = 0;       // 进入当前状态的时刻
static uint32_t s_enterAt  = 0;       // 本次充电的起点（判"短充不算进度"）
static bool     s_firstDone = false;  // 开机第一次决策做过没有
static bool     s_chgByAbs = false;   // 充电是靠"绝对电压高"认出来的（没有台阶可看）
static int      s_chgEnterMv = 0;     // 进入充电时的终端电压
static int      s_chgPeakMv  = 0;     // 充电期间最高的终端电压（学"充电虚高"用）

// ---- 空载锚点 / 负载压降 ----
static int      s_ocvMv    = -1;      // 最近一次关功放量到的开路电压
static uint32_t s_ocvAt    = 0;
static int      s_sagMv    = 0;       // 学到的"带载压降"（正数）
static int      s_vSoc     = -1;      // 平滑后的开路电压（再查表）
static bool     s_ocvReady = true;    // 拔线后要等终端电压平了才重新采信

// ---- 充电进度 ----
static int      s_anchor   = -1;      // 插电那一刻的真实电量（%）
static uint32_t s_flatAt   = 0;       // 终端电压开始"平"的时刻（恒压平台）
static int      s_flatMv   = 0;

// ---- 充电虚高（终端电压 - 开路电压）----
//   1.46.55 那个默认 550mV 是实测值，留着当兜底；每次拔线用实测覆盖（1/4 权重）。
static int      s_chgStep  = BATT_CHG_OFF_DEF;
static int      s_offDbg   = 0;

// ---- 满电 / 外接 ----
static bool     s_full     = false;
static uint32_t s_fullAt   = 0;
static bool     s_ext      = false;
static uint32_t s_extAt    = 0;

// ---- 低电播报 ----
static uint32_t s_lowAt    = 0;
static uint32_t s_annAt    = 0;
static bool     s_announcedLow = false;
static bool     s_offSaid  = false;
static uint32_t s_offSaidAt = 0;
static uint32_t s_emptyAt  = 0;

// ---- 0% 空载复核 ----
static int      s_freeMv   = -1;
static uint32_t s_freeAt   = 0;

// ---- 分压系数自校准（存 /battcal.txt）----
static float    s_scale     = 1.0f;
static bool     s_scaleLoaded = false;
static bool     s_scaleTried  = false;
static int      s_scaleDbg  = 1000;   // ×1000，给 /api/batt 看

// ---- 15 秒中位数 ----
static int      s_med[BATT_MED_N];
static int      s_medCnt = 0, s_medAt = 0;

// ---- 连续没出声的起点 ----
//   1.46.66 真机实测：`audioIsPlaying()` 在网络流播放中会随缓冲状态抖，
//   每抖一下就被当成"没在出声" ⇒ 把带载电压当开路电压、还把 s_ocvAt 重置，
//   于是 readNoLoadMv **一次都轮不到**（真机 /api/batt 的 bsag 恒 0 就是这么来的，
//   而 bocv 一直贴着 bmv 走）。判定必须要求"连续安静一段时间"。
static uint32_t s_quietFrom = 0;

// ---- 拔线后"等它平"的参照值 ----
static int      s_prevForFlat = -1;

// ---- 量测故障保险（拔线时取样链断过，1.46.27 的老坑）----
static int      s_lastGood = -1;
static int      s_glitch   = 0;

// ---- 120 秒窗口（认插拔台阶）----
//   只有 120 个 uint16（=240 字节），比原来 60 个 int 还省。
static uint16_t s_win[BATT_PLUG_WIN_N];
static int      s_winCnt = 0, s_winAt = 0;

// ------------------------------------------------------------
//  ADC：摊开在 ~42ms 里读 21 次，掐掉两端各 5 个再平均
//   为什么不是"同一瞬间连读 9 次取中位数"：WiFi 发射突发、并口屏写显存带来的
//   干扰会把一整组同时抬高或压低，中位数只压随机噪声，对"整组同向偏移"无效。
//   实测（1.46.22）：引脚 IO4 裸值在 2078~2233mV 之间跳，摊开之后降到 21mV。
// ------------------------------------------------------------
static int readOnce() {
  const int N = 21;
  int v[N];
  for (int i = 0; i < N; i++) {
    v[i] = analogReadMilliVolts(s_pin);
    if (i != N - 1) delayMicroseconds(2000);   // 2ms → 整组约 42ms
  }
  for (int i = 1; i < N; i++) {              // 插入排序，21 个数不心疼
    const int k = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  long sum = 0;
  for (int i = 5; i < N - 5; i++) sum += v[i];
  return (int)(sum / (N - 10));
}

// ------------------------------------------------------------
//  关功放量一次空载电压（返回 mV，-1 = 这台板没有功放使能脚）
//   这是唯一能拿到"真开路电压"的办法：功放一响终端就掉 100~300mV，
//   带着这个压降去查表，等于把电量报低 20~30 个点。
//   静音 180ms，人耳几乎听不出（实测用户在放歌时没察觉）。
// ------------------------------------------------------------
static int readNoLoadMv(int holdMs = 350) {
#if PIN_AMP_EN >= 0 && AMP_EN_ENABLE
  const int keep = digitalRead(PIN_AMP_EN);
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, AMP_EN_LEVEL ? LOW : HIGH);   // 关功放
  delay(holdMs);
  int mx = 0;
  for (int i = 0; i < 8; i++) {
    const int v = analogReadMilliVolts(s_pin);
    if (v > mx) mx = v;
    delayMicroseconds(7000);
  }
  digitalWrite(PIN_AMP_EN, keep);                        // 立刻恢复
  return (int)((float)mx * BATT_DIV * s_scale);
#else
  (void)holdMs;
  return -1;
#endif
}

static void powerOffNow() {
  Serial.println("[batt] 电量耗尽，切断整板电源");
  audioStop();
  delay(300);
  storeFlush();                     // 断电前把设置落一次盘（平时只在没声时写）
#if defined(BOARD_ATSMINI)
  pinMode(15, OUTPUT);
  digitalWrite(15, LOW);            // IO15 = 整板外设电源自锁，拉低就是真断电
  delay(200);
#endif
  esp_deep_sleep_start();
}

// ------------------------------------------------------------
//  分压系数（/battcal.txt 里就一个浮点数）
//   1.38.2 起这个文件存的是"用户校准系数"，1.43.0 起废掉；
//   1.46.64 起它改成**自动学出来的量程修正**（见 tick 里的 CV 平台）。
// ------------------------------------------------------------
static void scaleLoad() {
  if (s_scaleLoaded) return;
  s_scaleLoaded = true;
  s_scale = 1.0f;
  if (!LittleFS.exists("/battcal.txt")) return;
  File f = LittleFS.open("/battcal.txt", "r");
  if (!f) return;
  const String ln = f.readStringUntil('\n');
  f.close();
  const float v = ln.toFloat();
  if (v > 0.80f && v < 1.25f) {
    s_scale = v;
    s_scaleDbg = (int)(v * 1000.0f + 0.5f);
    Serial.printf("[batt] 读回量程系数 %.4f\n", (double)s_scale);
  }
}

static void scaleSave() {
  File f = LittleFS.open("/battcal.txt", "w");
  if (!f) { Serial.println("[batt] 量程系数存盘失败"); return; }
  f.printf("%.4f\n", (double)s_scale);
  f.close();
}

// ------------------------------------------------------------
//  上次电量（一个整数）—— 开机就插着电时靠它起算
// ------------------------------------------------------------
static int s_socSaved = -1;
static uint32_t s_socAt = 0;

static void socLoad() {
  if (!LittleFS.exists("/battsoc.txt")) return;
  File f = LittleFS.open("/battsoc.txt", "r");
  if (!f) return;
  const int v = f.readStringUntil('\n').toInt();
  f.close();
  if (v >= 0 && v <= 100) {
    s_socSaved = v;
    Serial.printf("[batt] 上次电量 %d%%（插着电开机时用它起算）\n", v);
  }
}

static void socSave(int pct) {
  File f = LittleFS.open("/battsoc.txt", "w");
  if (!f) return;
  f.printf("%d\n", pct);
  f.close();
  s_socSaved = pct;
  s_socAt    = millis();
}

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------
static void winPush(int v) {
  if (v < 0) v = 0;
  if (v > 60000) v = 60000;
  s_win[s_winAt] = (uint16_t)v;
  s_winAt = (s_winAt + 1) % BATT_PLUG_WIN_N;
  if (s_winCnt < BATT_PLUG_WIN_N) s_winCnt++;
}

static int winMin() {
  if (s_winCnt <= 0) return s_mv;
  int m = s_win[0];
  for (int i = 1; i < s_winCnt; i++) if (s_win[i] < m) m = s_win[i];
  return m;
}

static int winMax() {
  if (s_winCnt <= 0) return s_mv;
  int m = s_win[0];
  for (int i = 1; i < s_winCnt; i++) if (s_win[i] > m) m = s_win[i];
  return m;
}

// 显示值限速：差 BATT_PCT_HYST 点才动，一次最多 1 点。
//   为什么不用"分档"压噪声：分档和 OCV 表天生打架（见 config.h 里 1.46.58 那段）。
//   限速的好处是任何一次判错都只是"慢慢挪"，不会"啪"地跳几十点。
static void slewTo(int target) {
  if (target < 0)   target = 0;
  if (target > 100) target = 100;
  if (s_pct < 0) { s_pct = target; return; }
  // 1.46.64：端点不走死区。
  //   死区 3 点、步子 1 点/拍，于是「目标 100、现显示 98」会永远差 2 点不动 ——
  //   用户看到的就是「明明充满了，屏上却是 98%」；下头同理卡在 2% 不归零。
  //   所以 100 直接给；0 要等已经到 4% 以下才直接给（否则一口气跳 0 像坏了）。
  if (target >= 100) { s_pct = 100; return; }
  if (target <= 0 && s_pct <= 4) { s_pct = 0; return; }
  if (target <= s_pct - BATT_PCT_HYST) s_pct -= 1;
  else if (target >= s_pct + BATT_PCT_HYST) s_pct += 1;
  if (s_pct < 0)   s_pct = 0;
  if (s_pct > 100) s_pct = 100;
}

static void rawClamp() {
  if (s_pct < 0)   s_pct = 0;
  if (s_pct > 100) s_pct = 100;
}

// ------------------------------------------------------------
void batteryBegin() {
  s_pin      = -1;
  s_ok       = false;
  s_seenHigh = false;
  s_plausCnt = 0;
  s_mv       = -1;
  s_pct      = -1;
  s_rawMv    = -1;
  s_bootAt   = millis();
  s_nextAt   = millis() + 3000;          // 开机先让电源稳 3 秒

  s_st        = ST_DISC;
  s_stAt      = 0;
  s_enterAt   = 0;
  s_firstDone = false;
  s_chgByAbs  = false;
  s_chgEnterMv = 0;
  s_chgPeakMv  = 0;

  s_ocvMv    = -1;
  s_ocvAt    = 0;
  s_sagMv    = 0;
  s_vSoc     = -1;
  s_ocvReady = true;
  s_prevForFlat = -1;

  s_anchor   = -1;
  s_flatAt   = 0;
  s_flatMv   = 0;
  s_chgStep  = BATT_CHG_OFF_DEF;
  s_offDbg   = 0;

  s_full    = false;
  s_fullAt  = 0;
  s_ext     = false;
  s_extAt   = 0;

  s_lowAt  = 0;
  s_annAt  = 0;
  s_announcedLow = false;
  s_offSaid = false;
  s_offSaidAt = 0;
  s_emptyAt = 0;
  s_freeMv  = -1;
  s_freeAt  = 0;

  s_scaleTried = false;
  s_lastGood = -1;
  s_glitch   = 0;

  for (int i = 0; i < BATT_MED_N; i++) s_med[i] = 0;
  for (int i = 0; i < BATT_PLUG_WIN_N; i++) s_win[i] = 0;
  s_medCnt = 0; s_medAt = 0;
  s_winCnt = 0; s_winAt = 0;

  scaleLoad();                           // 1.46.64：读回量程修正
  socLoad();                             // 上次电量（插着电开机时用它起算）

#if BATT_ENABLE
  if (PIN_BATT_ADC < 0) {
    Serial.println("[batt] 没接电量检测（PIN_BATT_ADC = -1）");
    return;
  }
  s_pin = PIN_BATT_ADC;
  analogReadResolution(12);
  analogSetPinAttenuation(s_pin, ADC_11db);

#if PIN_CHG_STAT >= 0
  // 充电 IC 的状态脚（CHRG/STDBY）：有它就比拿电压猜准得多。
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);
  Serial.printf("[batt] 充电状态脚 IO%d（%s）\n", PIN_CHG_STAT,
                digitalRead(PIN_CHG_STAT) == LOW ? "正在充电" : "未在充电");
#endif

  // 立刻读一次：读数离谱就当没有电池，后面所有判定都不参与
  const int raw = readOnce();
  const int mv  = (int)((float)raw * BATT_DIV * s_scale + 0.5f);
  Serial.printf("[batt] 引脚 IO%d，引脚 %dmV，电池约 %dmV（分压 ×%.3f，量程 ×%.4f）\n",
                s_pin, raw, mv, (double)BATT_DIV, (double)s_scale);
  if (mv < 500) {
    Serial.println("[batt] 读数太低，当作没接电池：电量功能关闭");
    s_pin = -1;
    return;
  }
#else
  Serial.println("[batt] BATT_ENABLE = 0，电量功能关闭");
#endif
}

// ------------------------------------------------------------
//  进/出充电状态
// ------------------------------------------------------------
static void chgEnter(uint32_t now, bool byAbs) {
  s_st         = ST_CHG;
  s_stAt       = now;
  s_enterAt    = now;
  s_chgByAbs   = byAbs;
  s_chgEnterMv = s_mv;
  s_chgPeakMv  = s_mv;
  s_flatAt     = 0;

  // 锚点 = 插电那一刻的真实电量。四种来源，按可信度排：
  //   ① 终端已经在 CV 平台（≥4150）→ 这电芯基本满了，别管存的是什么
  //   ② 上次关机记下的电量，而且明显比"按电压算"低 ⇒ 电压是被充电顶高的
  //   ③ 屏上那个数：刚刚还在放电，它是准的（这是绝大多数情况）
  //   ④ 只能按"电压 - 充电虚高"倒推一把
  int p = -1;
  if (byAbs && s_mv >= BATT_FULL_DETECT_MV) {
    p = 98;
  } else if (s_socSaved >= 0 && s_socSaved < pctOf(s_mv) - 25) {
    p = s_socSaved;
  } else if (s_pct >= 0) {
    p = s_pct;
  } else {
    p = pctOf(s_mv - s_chgStep);
  }
  if (p < 0)  p = 50;
  if (p > 99) p = 99;
  s_anchor   = p;

  // 屏上那个数如果明显比锚点高，说明它刚才是在读"被顶高的电压"，直接拉回来。
  //   这正是用户报的「插上电瞬间变成 77」：77 是充电电压查表得来的，不是电量。
  if (s_pct < 0 || s_pct > p + 30) s_pct = p;

  Serial.printf("[batt] 开始充电（%s，终端 %dmV）：按 %d%% 起算（屏上 %d%%）\n",
                byAbs ? "电压已在充电位" : "看到插电台阶", s_mv, s_anchor, s_pct);
}

static void chgLeave(uint32_t now) {
  const uint32_t dur = now - s_enterAt;
  s_st     = ST_UNPLUG_WAIT;
  s_stAt   = now;
  s_offDbg = s_chgPeakMv - s_mv;
  // 学"充电虚高"：充电期峰值 − 拔线后的终端。这颗小电芯实测 ~580mV，
  //   1.46.55 的学习窗写的是 40~260mV，所以从来没学到过，一直在用默认值。
  if (s_offDbg >= 80 && s_offDbg <= 900) {
    s_chgStep = (s_chgStep * 3 + s_offDbg) / 4;
  }
  // 短充不认进度：充不到 10 分钟，终端电压涨的那点主要是极化，不是电量。
  //   用户"插上电看一眼、拔掉"的动作走的就是这条路，屏上不该因此变高。
  if (dur < BATT_CHG_MIN_TRUST_MS && s_anchor >= 0 && s_pct > s_anchor) {
    s_pct = s_anchor;
  }
  s_flatAt   = 0;
  s_ocvReady = false;          // 表面电荷还在消散，等平了再采信
  s_ocvMv    = -1;
  s_ocvAt    = 0;
  Serial.printf("[batt] 充电结束（充了 %lu 秒，虚高 %+dmV / 记住 %dmV）→ %d%%\n",
                (unsigned long)(dur / 1000), s_offDbg, s_chgStep, s_pct);
}

// ------------------------------------------------------------
//  充电中的电量推算（1.46.64）
//
//  一句话：**充电期间电压不能用**，只能从"插电那一刻的真实电量"按时间往上走。
//
//  为什么连"终端涨了多少"都不能用（1.46.64 实测推出来的）：
//    这块板充电时终端先被顶到 4.0 左右，一两分钟内就爬到充电器的 4.20V 恒压点，
//    然后**整段充电都钉在那儿**（600mAh 的电芯，4.2V 时电流早就到 CV 收尾区了）。
//    也就是说"充电过程中终端是平的"——涨的那一点是极化，不是电量。
//    所以按电压增量推出来的电量，永远只比锚点高几个点，没意义。
//
//  唯一还指示进度的东西是**恒压段持续多久**（电流收尾才算满）：
//    所以 ramp 按时间走，而"充满"由"平台稳住 ≥45 分钟"来定，不靠电压。
// ------------------------------------------------------------
static void chgTick(uint32_t now) {
  if (s_mv > s_chgPeakMv) s_chgPeakMv = s_mv;

  // 平台跟踪：终端稳住不动（±30mV）才开始计时；一有起伏就重来。
  if (s_mv >= BATT_CV_MV - 40) {
    if (!s_flatAt) { s_flatAt = now; s_flatMv = s_mv; }
    else if (abs(s_mv - s_flatMv) > BATT_CV_FLAT_MV) { s_flatAt = now; s_flatMv = s_mv; }
  } else {
    s_flatAt = 0;
  }

  // 时间推算：600mAh 的电芯，常见充电头 0.5~1A ⇒ 1% / 48 秒（≈80 分钟充满）
  int target = s_anchor + (int)((now - s_enterAt) / BATT_CHG_PCT_MS);
  if (target > 99) target = 99;
  if (target < s_anchor) target = s_anchor;      // 充电不会让电量变少
  if (s_full) target = 100;
  slewTo(target);
}

// ------------------------------------------------------------
void batteryTick() {
  if (s_pin < 0) return;
  const uint32_t now = millis();
  if (now < s_nextAt) return;
  s_nextAt = now + BATT_SAMPLE_MS;

  const int rawNow = readOnce();
  s_rawMv = rawNow;

  // 15 秒中位数（跨秒的慢漂移靠它压）
  s_med[s_medAt] = rawNow;
  s_medAt = (s_medAt + 1) % BATT_MED_N;
  if (s_medCnt < BATT_MED_N) s_medCnt++;
  int sorted[BATT_MED_N];
  for (int i = 0; i < s_medCnt; i++) sorted[i] = s_med[i];
  for (int i = 1; i < s_medCnt; i++) {
    const int k = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > k) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = k;
  }
  const int rawMed = sorted[s_medCnt / 2];

  int mv = (int)((float)rawMed * BATT_DIV * s_scale + 0.5f);
  if (mv < 0) mv = 0;

  // 1 秒内掉 400mV 以上 → 不是电池，是取样链断了（分压那头没电 / 引脚浮空）。
  //   这一笔直接丢掉，屏上保持上一次的数；连续 30 秒才认栽（画斜线框）。
  if (s_lastGood > 0 && mv < s_lastGood - 400) {
    if (s_glitch < 30) {
      s_glitch++;
      if (s_glitch == 1)
        Serial.printf("[batt] 读数异常跳变 %dmV → %dmV，先丢掉这一笔\n", s_lastGood, mv);
      return;
    }
    if (s_ok) { s_ok = false; Serial.println("[batt] 读数连续 30 秒异常，标记为不可信"); }
    return;
  }
  if (s_glitch) { Serial.printf("[batt] 读数恢复（%dmV）\n", mv); s_glitch = 0; }
  s_lastGood = mv;

  // 电压平滑：1/6，约 6 秒收敛。喇叭唱两句停一下也进不来。
  s_mv = (s_mv < 0) ? mv : (s_mv + (mv - s_mv) / 6);

  winPush(s_mv);
  const int rise = s_mv - winMin();
  const int fallDn = winMax() - s_mv;

  // ---- 开机第一次：先决定"现在是不是插着电"，再决定屏上写几 ----
  //   为什么必须前置：插着电开机的第一拍如果先按电压算，就是 77%，
  //   之后再怎么纠都要"慢慢挪"下去 —— 用户看到的就是"一开机就 77，过一会儿变 3"。
  if (!s_firstDone) {
    s_firstDone = true;
    const int vp = pctOf(s_mv);
    const bool savedFar = (s_socSaved >= 0 && s_socSaved <= vp - 25 && s_mv >= 3800);
    if (s_mv >= BATT_ABS_PLUG_MV) {
      chgEnter(now, true);
    } else if (savedFar) {
      chgEnter(now, true);          // 关机前 3%、开机读到 77% ⇒ 一定是在充电
    }
  }

  // ---- 插拔识别：只看相对台阶，不看绝对值 ----
  switch (s_st) {
    case ST_DISC:
      if (rise >= BATT_PLUG_RISE_MV) { s_st = ST_PLUG_WAIT; s_stAt = now; }
      break;

    case ST_PLUG_WAIT:
      // 等 30 秒：真插电会一直高，负载跳变（实测最多 +122mV）会自己落回去。
      if (rise < BATT_PLUG_RISE_MV / 2 && s_mv < BATT_ABS_PLUG_MV) {
        s_st = ST_DISC;                          // 落回去了，是起播/换歌
      } else if (now - s_stAt >= BATT_PLUG_SETTLE_MS) {
        if (rise >= (BATT_PLUG_RISE_MV * 3) / 4 || s_mv >= BATT_ABS_PLUG_MV) {
          chgEnter(now, false);
        } else {
          s_st = ST_DISC;
        }
      }
      break;

    case ST_CHG:
      // 拔线：掉了 170mV 以上；或者从"充电位"掉下来（绝对电压那条进来的）。
      //   再加一条"比进来时低了 110mV" —— 用来兜住"误判成充电后歌一停就掉"的情况。
      if (fallDn >= BATT_UNPLUG_FALL_MV ||
          s_mv <= s_chgEnterMv - BATT_CHG_LEAVE_DROP_MV ||
          (s_chgByAbs && s_mv < BATT_FULL_CLEAR_MV)) {
        chgLeave(now);
      }
      break;

    case ST_UNPLUG_WAIT:
      // 拔线后终端电压里还挂着"表面电荷"（几分钟到十几分钟才散掉），那不是电量。
      // 判"散完了"用**120 秒窗口的跨度**：跨度小说明它在平，不是在往下滑。
      // 最多等 BATT_UNPLUG_MAX_MS 就不管了 —— 不能让屏上永远冻着。
      if (now - s_stAt >= BATT_UNPLUG_HOLD_MS &&
          (winMax() - winMin() < BATT_UNPLUG_FLAT_MV ||
           now - s_stAt > BATT_UNPLUG_MAX_MS)) {
        s_st = ST_DISC;
        s_vSoc = ocvOfPct(s_pct);       // 从当前显示接着走，不跳
        s_ocvMv = -1;
        s_ocvAt = 0;
        s_ocvReady = true;
      }
      break;
  }
  s_prevForFlat = s_mv;

  // ---- 按状态算目标百分比 ----
  if (s_st == ST_CHG) {
    chgTick(now);
  } else if (s_st == ST_PLUG_WAIT || s_st == ST_UNPLUG_WAIT) {
    // 判定窗口内屏上不动：宁可稳住，也不要在"还不知道"的时候报一个数
    rawClamp();
  } else {
    // ---- 放电：开路电压 → 表 ----
    // 1.46.66：`audioIsPlaying()` 只认网络流 —— 收音时它恒为 false，可功放其实
    //   正在放 FM 的声音。真机实测抓住了证据：FM 模式下 bocv(3794) 比 bmv(3828)
    //   还低，而开路电压不可能低于带载电压 ⇒ 当时是拿带载值当开路值查表，
    //   屏上电量比真实值低一截。所以这里把「在收音」也当成有负载。
    const bool loud = audioIsPlaying() || A.fmMode;
    if (loud) s_quietFrom = 0;
    else if (!s_quietFrom) s_quietFrom = now;
    // 连续安静够久才算"确实没有负载压降"；抖一下不算（见 s_quietFrom 的说明）
    const bool calm = (s_quietFrom != 0 && now - s_quietFrom >= BATT_QUIET_AS_OCV_MS);
    int ocv;
    if (calm) {
      // 没在出声 = 没有负载压降，这一刻的读数就是开路电压
      s_ocvMv = s_mv;
      s_ocvAt = now;
      ocv = s_mv;
    } else {
      if (s_ocvMv < 0 || now - s_ocvAt > BATT_OCV_REFRESH_MS) {
        const int f = readNoLoadMv(BATT_OCV_MUTE_MS);
        if (f > 0) {
          // 1.46.66：**符号写反了**。压降 = 开路 − 带载，是正数；原来写成
          //   `s_mv - f`（带载 − 开路）得到负数（真机约 -270），接着又被下一行
          //   的 `> -120` 滤掉 ⇒ s_sagMv 一次都没学成，永远是 0。真机上
          //   /api/batt 的 bsag 恒为 0 就是这么来的；后果是两次开路刷新之间
          //   只能用带载值查表，屏上电量偏低（正是用户最初报的"虚低"）。
          const int sag = f - s_mv;
          if (sag > -60 && sag < BATT_SAG_MAX_MV) s_sagMv = (s_sagMv * 3 + sag) / 4;
          s_ocvMv = f;
          s_ocvAt = now;
          Serial.printf("[batt] 空载 %dmV（带载 %dmV，压降 %+dmV / 记住 %dmV）→ %d%%\n",
                        f, s_mv, sag, s_sagMv, pctOf(f));
        } else {
          s_ocvAt = now;                 // 没有功放使能脚：别每秒重试
        }
      }
      const bool fresh = (s_ocvMv > 0 && (now - s_ocvAt) < 2UL * BATT_OCV_REFRESH_MS);
      ocv = fresh ? s_ocvMv : (s_mv + s_sagMv);
    }
    if (ocv > BATT_OVERV_MV) ocv = BATT_OVERV_MV;
    if (ocv < BATT_OCV_LO_MV) ocv = BATT_OCV_LO_MV;

    // 平滑做在毫伏上，表只查一次。
    //   ⚠️ 顺序不能反：表的下半段是凸的（+60mV 值 21 个点、-60mV 只值 12 个点），
    //     先平均百分比再查表，噪声会被系统性地往下拉。
    if (s_vSoc < 0) s_vSoc = ocv;
    else            s_vSoc += (ocv - s_vSoc) / 8;

    slewTo(pctOf(s_vSoc));
  }

  // ---- 要显示 0% 了 → 关功放量一次空载，确认真没电 ----
  if (s_st == ST_DISC && s_pct >= 0 && s_pct <= 0 && now - s_freeAt > 60000UL) {
    s_freeAt = now;
    const int f = readNoLoadMv();
    if (f > 0) {
      s_freeMv = f;
      if (f > s_mv) s_mv = f;
      const int p2 = pctOf(s_vSoc > 0 ? s_vSoc : f);
      s_pct = (p2 < 1) ? 0 : p2;
      Serial.printf("[batt] 0%% 复核：关功放 0.4 秒，空载 %dmV → %d%%%s\n",
                    f, s_pct, (s_pct > 0 ? "（还有电，不报 0）" : "（确实没电）"));
    }
  }
  if (s_st == ST_DISC && s_pct <= 0 &&
      s_freeMv >= BATT_EMPTY_MV + BATT_EMPTY_MARGIN_MV) {
    const int pf = pctOf(s_freeMv);
    s_pct = (pf < 1) ? 1 : pf;          // 空载还有电就守住地板，别滑回 0%
  }

  // ---- 放电期间把真实电量存下来（变化 ≥2 点、间隔 ≥60 秒才写，省 flash） ----
  if (s_st == ST_DISC && s_pct >= 0 &&
      (s_socSaved < 0 || s_socSaved != s_pct) &&
      (now - s_socAt > 60000UL || s_socSaved < 0)) {
    if (s_socSaved < 0 || (s_pct - s_socSaved >= 2) || (s_socSaved - s_pct >= 2)) socSave(s_pct);
  }

  if (s_mv >= BATT_SEEN_HIGH_MV) s_seenHigh = true;

  // ---- 外接电源：充电中当然算；否则看绝对电压 ----
  //   1.46.64：s_chg 也算进来。以前只认 4120mV，而这块板充电时终端到不了，
  //   于是 power.cpp 的"满电降载"和"充电超时提示"从来没生效过。
  if (s_st == ST_CHG || s_mv >= BATT_EXT_MV) {
    if (!s_extAt) s_extAt = now;
    if (!s_ext && (s_st == ST_CHG || now - s_extAt >= BATT_EXT_HOLD_MS)) {
      s_ext = true;
      Serial.println("[batt] 判定插着外接电源");
    }
  } else {
    s_extAt = 0;
    if (s_ext) { s_ext = false; Serial.println("[batt] 外接电源已断开"); }
  }

  // ---- 满电 / 外接电源 ----
  //   满电只认一件事：**恒压平台稳住够久**。电流收尾是充电器自己判的，
  //   我们看不到电流，但"终端钉在 4.2V 上一直不动"就是它在收尾的表现。
  //   600mAh 的电芯，0.5C 进 CV 之后大约 40~60 分钟收尾，取 45 分钟。
  //   1.46.55 用 20 分钟 + 电压到位，插上 20 分钟就报"已充满"，是假的。
  const bool plateau = (s_st == ST_CHG && s_flatAt &&
                        now - s_flatAt >= BATT_CV_FLAT_HOLD_MS && s_mv >= BATT_CV_MV - 40);
  if (plateau && !s_full && s_flatAt &&
      now - s_flatAt >= BATT_CHG_PLATEAU_FULL_MS) {
    s_full = true;
    Serial.printf("[batt] 判定已充满（恒压平台 %dmV 稳住 %lu 分钟）\n",
                  s_mv, (unsigned long)(BATT_CHG_PLATEAU_FULL_MS / 60000UL));
  }
  if (s_st != ST_CHG && s_mv < BATT_FULL_CLEAR_MV) {
    s_fullAt = 0;
    if (s_full) { s_full = false; Serial.printf("[batt] 不再是满电（%dmV）\n", s_mv); }
  }

  // ---- 分压系数自校准：恒压平台上终端就是充电器的 4.20V ----
  //   这是唯一不靠万用表就能拿到的硬基准（TP4056 的 CV 点是 4.2V±1%）。
  //   判据：在充电、终端 ≥4130、且已经平了 5 分钟（此时 R·I 已被恒压收尾压掉）。
  //   修正量小于 1.5% 就不动 —— 免得把充电器的 1% 公差学进系数里。
  if (plateau && s_flatAt && now - s_flatAt >= BATT_CV_FLAT_HOLD_MS &&
      s_mv >= 4130 && !s_scaleTried) {
    s_scaleTried = true;
    const float want = 4200.0f / (float)s_mv;
    float ns = s_scale * want;
    if (ns < 0.90f) ns = 0.90f;
    if (ns > 1.12f) ns = 1.12f;
    if (ns > s_scale * 1.015f || ns < s_scale * 0.985f) {
      Serial.printf("[batt] 平台 %dmV → 量程系数 %.4f → %.4f\n",
                    s_mv, (double)s_scale, (double)ns);
      s_scale = ns;
      s_scaleDbg = (int)(ns * 1000.0f + 0.5f);
      scaleSave();
    } else {
      Serial.printf("[batt] 平台 %dmV，量程系数 %.4f 不动\n", s_mv, (double)s_scale);
    }
  }

  // ---- 可信度：见过高位 或 连续 3 次读数都合理 → 认作真电池 ----
  if (s_mv >= BATT_PLAUSIBLE_MV && s_mv <= BATT_OVERV_MV) {
    if (s_plausCnt < 3) s_plausCnt++;
  } else {
    s_plausCnt = 0;
  }
  if ((s_seenHigh || s_plausCnt >= 3) && !s_ok) {
    s_ok = true;
    Serial.printf("[batt] 判定为真电池（%dmV / %d%%）\n", s_mv, s_pct);
  }

  // ---- 低电播报（10 分钟重复一次；在充电就不出声） ----
  const bool lowNow = s_ok && s_pct >= 0 && s_pct <= BATT_LOW_PCT;
  const bool quiet  = (s_st == ST_CHG) || (s_st == ST_PLUG_WAIT);

  if (s_st == ST_CHG && s_lowAt) s_lowAt = 0;     // 在充电，取消低电提醒

  if (quiet) {
    if (!s_announcedLow) {
      s_announcedLow = true;
      if (!s_annAt) s_annAt = now;
    }
  } else if (lowNow) {
    if (!s_lowAt) s_lowAt = now;
    // 开机头 6 分钟不念（1.46.64：以前靠"趋势环攒满 12 笔"，那个环已经删了）
    if (now - s_bootAt >= BATT_LOW_QUIET_MS &&
        now - s_lowAt >= BATT_LOW_QUIET_MS &&
        (!s_announcedLow || now - s_annAt > BATT_LOW_REPEAT_MS)) {
      s_announcedLow = true;
      s_annAt = now;
      Serial.printf("[batt] 电量低 %d%%（%dmV），播报\n", s_pct, s_mv);
      voicePlay(VO_LOW);
    }
  } else {
    s_lowAt = 0;
    s_announcedLow = false;
  }

  // ---- 没电：撑满 BATT_EMPTY_HOLD_MS 才动手（充电/插电就取消） ----
  //   判据用**开路电压**（s_vSoc），不用带载的 s_mv —— 功放一响掉 250mV，
  //   拿带载值判会在还有十几个点的时候就关机（1.46.58 修过一次，这次底线也降了）。
  const int mvEmpty = (s_vSoc > 0) ? s_vSoc : s_mv;
  if (s_ok && s_seenHigh && s_st == ST_DISC && !s_ext &&
      mvEmpty <= BATT_EMPTY_MV) {
    if (!s_emptyAt) s_emptyAt = now;
  } else {
    if (s_emptyAt) Serial.println("[batt] 电压回上来了，取消关机");
    s_emptyAt = 0;
    if (s_st == ST_CHG) s_offSaid = false;
  }

  if (!s_offSaid && s_emptyAt &&
      now - s_emptyAt > BATT_EMPTY_HOLD_MS &&
      now - s_bootAt > BATT_OFF_MIN_UP_MS) {
    s_offSaid = true;
    s_offSaidAt = now;
    Serial.printf("[batt] 电量耗尽 %d%%（开路 %dmV / 带载 %dmV），播报后关机\n",
                  s_pct, mvEmpty, s_mv);
    voicePlay(VO_OFF);
  }

  if (s_offSaid && (!voiceBusy() || now - s_offSaidAt > 12000UL)) {
#if BATT_AUTO_OFF
    powerOffNow();
#else
    s_offSaid = false;
    s_emptyAt = now;
#endif
  }
}

// ------------------------------------------------------------
bool batteryOk()       { return s_ok; }
int  batteryPct()      { return s_ok ? s_pct : -1; }
int  batteryMv()       { return s_mv; }
bool batteryCharging() { return s_st == ST_CHG; }
bool batteryLow()      { return s_ok && s_lowAt != 0; }
int  batteryPin()      { return s_pin; }
bool batteryFull()     { return s_full; }
bool batteryOnExt()    { return s_ext; }

float batteryCalFactor() { scaleLoad(); return s_scale; }
int   batteryRawMv()     { return s_rawMv; }

void batteryCalibrate(int realMv) {
  // 1.43.0 起不再接受人工校准（要和原厂口径一致）；1.46.64 起量程改由
  // CV 平台自动学。接口留着只为不回老 App 的 404。
  Serial.printf("[batt] 校准请求 %dmV 已忽略（量程自动学）\n", realMv);
}

void batteryJson(String& o) {
  o += "\"bok\":";
  o += s_ok ? 1 : 0;
  o += ",\"batt\":";
  o += (s_ok ? s_pct : -1);
  o += ",\"bmv\":";
  o += s_mv;
  o += ",\"chg\":";
  o += (s_st == ST_CHG) ? 1 : 0;
  o += ",\"blow\":";
  o += batteryLow() ? 1 : 0;
  o += ",\"bpin\":";
  o += s_pin;
  o += ",\"bfull\":";
  o += s_full ? 1 : 0;
  o += ",\"bext\":";
  o += s_ext ? 1 : 0;
  o += ",\"bcal\":";
  o += String((double)s_scale, 4);
  o += ",\"braw\":";
  o += s_rawMv;
  // 1.46.64：状态机与两个学到的量，真机对不上时看这里
  o += ",\"bst\":";
  o += s_st;
  o += ",\"banc\":";
  o += s_anchor;
  o += ",\"bocv\":";
  o += s_vSoc;
  o += ",\"bsag\":";
  o += s_sagMv;
  o += ",\"bstep\":";
  o += s_chgStep;
  o += ",\"boff\":";
  o += s_offDbg;
  o += ",\"bscale\":";
  o += s_scaleDbg;
  o += ",\"bfree\":";
  o += s_freeMv;
}
