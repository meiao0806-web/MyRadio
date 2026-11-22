// ============================================================
//  power.cpp —— 电源保护 / 电流保护（1.21.0）
//
//  三层保护，从上到下依次是「软的」到「硬的」：
//
//  ① 温度降载（默认开，不依赖任何额外接线）
//     芯片内部温度每 5 秒读一次，做慢平滑（新值 1/4）。三档：
//        ≥52℃ → 音量 ≤70 / 亮度 ≤65
//        ≥60℃ → 音量 ≤50 / 亮度 ≤40
//        ≥68℃ → 暂停播放，等凉下来自己接回去
//     回滞 4℃：降到阈值以下 4℃ 才逐级退回，避免在临界点来回抖。
//
//  ② 满电降载（默认开）
//     「已充满」由 battery.cpp 判（电压到 4180mV 并稳住 2 分钟）。
//     满电时亮度压到 60%、音量压到 90 —— 目的是让系统总电流小于充电器
//     能给的电流，别在满电点反复「充一点→放一点」。
//     音量只压到 90 是刻意的：压狠了用户会觉得"音量上不去"，收益却很小。
//
//  ③ 真停充（要硬件）
//     PIN_CHG_CTRL >= 0 才有用。满电 → 翻转使能脚停充；电压掉到
//     BATT_RECHG_MV(4050) → 恢复。就是笔记本的「保养模式」，
//     长期插电时电池大部分时间待在 4.05~4.2V 之间，而不是死顶在 4.2V。
//     没有这条线的板子，这一段整个编译掉，不影响任何行为。
//
//  为什么用芯片内部温度：
//    S3 的 temperatureRead() 绝对值有 ±5℃ 误差，但**相对变化**很准 ——
//    我们要判断的正是"比平时热了多少"，所以够用，也省一个外部元件。
// ============================================================
#include "power.h"
#include "config.h"
#include "state.h"
#include "battery.h"
#include "audio.h"          // 上限变了要通知音频任务重设音量

#if PWR_ENABLE

// ------------------------------------------------------------
//  状态
// ------------------------------------------------------------
static int      s_temp      = -999;    // 平滑后的温度（℃，先无条件采一次）
static int      s_lvl       = 0;       // 当前降载档 0..3
static bool     s_heatPause = false;   // 是否因过热暂停了播放
static uint32_t s_nextAt    = 0;
static bool     s_ctrlReady = false;   // 充电控制脚已初始化
static bool     s_cut       = false;   // 正在停充
static bool     s_lastFull  = false;   // 上一次采样时的「已充满」状态
static bool     s_overV     = false;   // 电池过压（充电异常）
static bool     s_overSaid  = false;   // 过压提示已经出过一次，别一直刷屏

// ---- 1.21.0 快充检测 ----
static int      s_vbus      = -1;      // USB 输入电压（mV，-1 = 没接/读不到）
static bool     s_fast      = false;   // 判定为快充（≥7V）
static int      s_rateMvpm  = 0;       // 充电速率（mV/分钟）
static bool     s_fastRate  = false;   // 充得太猛
static int      s_rateHist[5] = {0,0,0,0,0};
static int      s_rateAt    = 0;
static uint32_t s_rateMs    = 0;
static uint32_t s_plugAt    = 0;       // 插上电的时刻（判充电超时）
static bool     s_slowSaid  = false;   // 充电超时提示已经说过

// 温度 -> 档位（不带回滞；回滞在后面单独处理）
static int lvlOf(int t) {
  if (t >= PWR_T3_C) return 3;
  if (t >= PWR_T2_C) return 2;
  if (t >= PWR_T1_C) return 1;
  return 0;
}

// 带迟滞的档位推进：往上升要过阈值，往下退要低于「阈值 - 回滞」
static int lvlStep(int cur, int t) {
  const int raw = lvlOf(t);
  if (raw > cur) return raw;                  // 变热：立刻跟上
  if (raw < cur) {
    // 变凉：要掉到当前档对应阈值以下 PWR_HYST_C 才退
    const int th = (cur == 3) ? PWR_T3_C : (cur == 2) ? PWR_T2_C : PWR_T1_C;
    if (t < th - PWR_HYST_C) return raw;
    return cur;
  }
  return cur;
}

// ------------------------------------------------------------
//  ③ 真停充：只在接了使能脚时才存在
// ------------------------------------------------------------
#if PIN_CHG_CTRL >= 0
static void chgCtrlInit() {
  pinMode(PIN_CHG_CTRL, OUTPUT);
  // 上电先恢复「允许充电」，别一开机就把电断了
#if CHG_CTRL_ACTIVE_LOW
  digitalWrite(PIN_CHG_CTRL, HIGH);
#else
  digitalWrite(PIN_CHG_CTRL, LOW);
#endif
  s_cut = false;
  s_ctrlReady = true;
  Serial.printf("[pwr] 充电使能脚 IO%d 已接管（满电停充，掉到 %dmV 恢复）\n",
                PIN_CHG_CTRL, BATT_RECHG_MV);
}

static void chgCtrlWrite(bool cut) {
  if (!s_ctrlReady || s_cut == cut) return;
  s_cut = cut;
#if CHG_CTRL_ACTIVE_LOW
  digitalWrite(PIN_CHG_CTRL, cut ? LOW : HIGH);
#else
  digitalWrite(PIN_CHG_CTRL, cut ? HIGH : LOW);
#endif
  Serial.printf("[pwr] %s充电\n", cut ? "停止" : "恢复");
}

static void chgCtrlTick() {
  if (!s_ctrlReady) return;
  const int mv = batteryMv();
  if (mv <= 0) return;
  if (!s_cut && (batteryFull() || mv >= BATT_OVERV_MV)) {
    chgCtrlWrite(true);                       // 满了 / 过压 → 断
  } else if (s_cut && !s_overV && mv <= BATT_RECHG_MV) {
    chgCtrlWrite(false);                      // 放到 4.05V → 再充
  }
}
#endif

// ------------------------------------------------------------
void powerBegin() {
  s_temp = -999;
  s_lvl = 0;
  s_heatPause = false;
  s_cut = false;
  s_ctrlReady = false;
  s_nextAt = millis() + 2000;                 // 开机先让温度稳一下

#if PIN_CHG_CTRL >= 0
  chgCtrlInit();
#endif

#if PIN_VBUS_ADC >= 0
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBUS_ADC, ADC_11db);
  s_vbus = (int)(analogReadMilliVolts(PIN_VBUS_ADC) * VBUS_DIV);
  Serial.printf("[pwr] USB 电压脚 IO%d，读到约 %dmV\n", PIN_VBUS_ADC, s_vbus);
#endif

  // 先读一次，把绝对值打出来 —— 装机以后出问题，看这一行就知道基线
  const int t0 = (int)temperatureRead();
  Serial.printf("[pwr] 电源保护开启：芯片温度基线 %d℃（档位 %d/%d/%d℃）\n",
                t0, PWR_T1_C, PWR_T2_C, PWR_T3_C);
#if PIN_CHG_CTRL < 0
  Serial.println("[pwr] 没接充电使能脚：只做温度/满电降载，不会真的停充");
#endif
}

// ------------------------------------------------------------
void powerTick() {
  const uint32_t now = millis();
  if (now < s_nextAt) return;
  s_nextAt = now + PWR_SAMPLE_MS;

  // ---- 温度：慢平滑，第一次直接采纳 ----
  const int raw = (int)temperatureRead();
  if (raw > -50 && raw < 150) {                // 偶尔会读出乱值，丢掉
    s_temp = (s_temp < -100) ? raw : (s_temp + (raw - s_temp) / 4);
  }

  const int before = s_lvl;
  if (s_temp > -100) s_lvl = lvlStep(s_lvl, s_temp);

  if (s_lvl != before) {
    static const char* kName[] = { "正常", "一档降载", "二档降载", "过热暂停" };
    Serial.printf("[pwr] 温度 %d℃ -> %s（音量上限 %d / 亮度上限 %d）\n",
                  s_temp, kName[s_lvl], powerVolCapPct(), powerBrightCap());
    A.dirty = true;
    audioRefreshVol();                         // 让音频任务按新上限重设一次
  }

  // ---- 满电状态变化：上限跟着变，立刻通知音频任务 ----
  const bool full = batteryFull();
  if (full != s_lastFull) {
    s_lastFull = full;
    Serial.printf("[pwr] %s（音量上限 %d / 亮度上限 %d）\n",
                  full ? "已充满 -> 降载" : "不再满电 -> 解除降载",
                  powerVolCapPct(), powerBrightCap());
    audioRefreshVol();
    A.dirty = true;
  }

  // ---- 电池过压：充电在失控 ----
  const int mv = batteryMv();
  const bool overNow = (mv > 0 && mv >= BATT_OVERV_MV);
  if (overNow != s_overV) {
    s_overV = overNow;
    if (s_overV) {
      Serial.printf("[pwr] !! 电池电压 %dmV 已超过 %dmV —— 充电可能失控，请拔掉充电器\n",
                    mv, BATT_OVERV_MV);
      if (!s_overSaid) {
        s_overSaid = true;
        setPlayState(PS_PLAY, "充电异常，请拔充电器");
        A.dirty = true;
      }
    } else {
      Serial.println("[pwr] 电池电压回到正常范围");
      s_overSaid = false;
      A.dirty = true;
    }
  }

  // ---- 快充检测 ①：USB 输入电压（只有接了分压才有） ----
#if PIN_VBUS_ADC >= 0
  {
    const int v = (int)(analogReadMilliVolts(PIN_VBUS_ADC) * VBUS_DIV);
    if (v > 3000 && v < 24000) s_vbus = (s_vbus < 0) ? v : (s_vbus + (v - s_vbus) / 4);
    const bool fast = (s_vbus >= VBUS_FAST_MIN);
    if (fast != s_fast) {
      s_fast = fast;
      Serial.printf("[pwr] USB 输入 %dmV -> %s\n", s_vbus,
                    fast ? "快充档（9V/12V）" : "普通 5V");
      A.dirty = true;
    }
  }
#endif

  // ---- 快充检测 ②：充电速率（5 分钟平均，每分钟涨多少 mV）----
  if (now - s_rateMs > 60000UL) {
    s_rateMs = now;
    const int mvNow = batteryMv();
    if (s_rateHist[0] == 0) {
      for (int i = 0; i < 5; i++) s_rateHist[i] = mvNow;
    } else {
      s_rateMvpm = (mvNow - s_rateHist[s_rateAt]) / 5;
      s_fastRate = (s_rateMvpm >= BATT_FAST_MVPM);
    }
    s_rateHist[s_rateAt] = mvNow;
    s_rateAt = (s_rateAt + 1) % 5;
  }

  // ---- 快充检测 ③：充电超时（不用任何硬件）----
  if (batteryOnExt() && !batteryFull()) {
    if (!s_plugAt) s_plugAt = now;
    if (!s_slowSaid && now - s_plugAt > BATT_CHG_MAX_MS) {
      s_slowSaid = true;
      Serial.printf("[pwr] 插电 %lu 小时还没充满（%dmV / %d mV每分），"
                    "多半是电池老化或充电器供不上\n",
                    (unsigned long)(BATT_CHG_MAX_MS / 3600000UL), batteryMv(), s_rateMvpm);
      setPlayState(PS_PLAY, "充电很慢，检查充电器");
      A.dirty = true;
    }
  } else {
    s_plugAt = 0;
    s_slowSaid = false;
  }

  // ---- 过热暂停 / 恢复 ----
  if (s_lvl >= 3 && !s_heatPause) {
    s_heatPause = true;
    setPlayState(PS_IDLE, "太热了，歇一会儿");
    audioStop();                               // 停流比暂停更彻底：解码线程也歇
    Serial.println("[pwr] 过热，暂停播放");
  } else if (s_lvl < 3 && s_heatPause) {
    s_heatPause = false;
    Serial.println("[pwr] 温度回落，恢复播放");
    if (A.cur >= 0) audioPlayIndex(A.cur);
    A.dirty = true;
  }

#if PIN_CHG_CTRL >= 0
  chgCtrlTick();
#endif
}

// ------------------------------------------------------------
//  上限们：温度档和满电档取**更狠**的那个
// ------------------------------------------------------------
int powerVolCapPct() {
  int cap = 100;
  if (batteryFull()) cap = PWR_FULL_VOL_PCT;
  if (s_lvl >= 2)    cap = PWR_T2_VOL_PCT < cap ? PWR_T2_VOL_PCT : cap;
  if (s_lvl >= 1)    cap = PWR_T1_VOL_PCT < cap ? PWR_T1_VOL_PCT : cap;
  return cap;
}

int powerBrightCap() {
  int cap = 100;
  if (batteryFull()) cap = PWR_FULL_BRIGHT;
  if (s_lvl >= 2)    cap = PWR_T2_BRIGHT < cap ? PWR_T2_BRIGHT : cap;
  if (s_lvl >= 1)    cap = PWR_T1_BRIGHT < cap ? PWR_T1_BRIGHT : cap;
  return cap;
}

int  powerTempC()     { return s_temp; }
int  powerLevel()     { return s_lvl; }
bool powerHeatPaused(){ return s_heatPause; }
bool powerChgCut()    { return s_cut; }
bool powerOnExt()     { return batteryOnExt(); }
int  powerVbusMv()    { return s_vbus; }
bool powerFastChg()   { return s_fast; }
int  powerChgRateMvpm() { return s_rateMvpm; }
bool powerFastRate()  { return s_fastRate; }

void powerJson(String& o) {
  o += "\"ptemp\":";
  o += s_temp;
  o += ",\"plvl\":";
  o += s_lvl;
  o += ",\"pvcap\":";
  o += powerVolCapPct();
  o += ",\"pccap\":";
  o += powerBrightCap();
  o += ",\"pheat\":";
  o += s_heatPause ? 1 : 0;
  o += ",\"pcut\":";
  o += s_cut ? 1 : 0;
  o += ",\"pfull\":";
  o += batteryFull() ? 1 : 0;
  o += ",\"pvbus\":";
  o += s_vbus;
  o += ",\"pfast\":";
  o += s_fast ? 1 : 0;
  o += ",\"prate\":";
  o += s_rateMvpm;
}

#else   // PWR_ENABLE == 0：全部退化成常量，一行逻辑都不跑

void powerBegin() {}
void powerTick()  {}
int  powerVolCapPct()  { return 100; }
int  powerBrightCap()  { return 100; }
int  powerTempC()      { return -999; }
int  powerLevel()      { return 0; }
bool powerHeatPaused() { return false; }
bool powerChgCut()     { return false; }
bool powerOnExt()      { return false; }
int  powerVbusMv()     { return -1; }
bool powerFastChg()    { return false; }
int  powerChgRateMvpm(){ return 0; }
bool powerFastRate()   { return false; }
void powerJson(String& o) { o += "\"ptemp\":-999,\"plvl\":0,\"pvcap\":100,\"pccap\":100,\"pheat\":0,\"pcut\":0,\"pfull\":0"; }

#endif
