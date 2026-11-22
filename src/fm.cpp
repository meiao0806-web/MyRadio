// ============================================================
//  fm.cpp —— Si4732 全波段收音（I2C 控制）
//
//  命令集按 Silicon Labs AN332（Si473x 编程指南）实现。
//
//  1.10.0 的变化：以前只开 FM 一个波段，理由是「AM/SW/LW 要灌 patch」——
//  这个理由是错的。AN332 第 2 页写明 SI4732-A10 的 FMRX 与 AM_SW_LW
//  两个固件模块与 SI4735-D60 相同，**AM 直接 POWER_UP(FUNC=1) 就能收**，
//  只有 SSB 才要 patch。所以这里把三种 AM 波段一起开了。
//
//  对外只有一个「kHz 频率」，芯片模式由 applyTune() 自己切：
//    长波/中波/短波 → FUNC=1(AM)   调频 → FUNC=0(FM)
//  切模式要 POWER_DOWN → POWER_UP（约 0.3~0.5 秒），只在跨波段那一下发生。
//
//  数据流：
//    旋钮 → fmTune(kHz) 只改目标值 → fmLoop 停手 160ms 后 applyTune()
//         → I2C 写 TUNE_FREQ → 芯片自己收台
//    声音：模块耳机口 → 3.5mm → 功放 LINE IN，和 ESP32 无关
// ============================================================
#include "fm.h"
#include "evlog.h"      // 1.46.42：搜台/恢复台表留痕
#include "config.h"
#include "state.h"
#include <Wire.h>
#include <LittleFS.h>   // 1.38.0：搜台结果存盘
#include "voice.h"      // 1.40.1：搜台开始时念一句「正在搜台」
#include "store.h"      // 1.46.59：记住最后一次调到的频率

#if FM_ENABLE

// ---------------- Si473x 命令（AN332 表 12~19）----------------
static const uint8_t CMD_POWER_UP       = 0x01;
static const uint8_t CMD_GET_REV        = 0x10;
static const uint8_t CMD_POWER_DOWN     = 0x11;
static const uint8_t CMD_SET_PROPERTY   = 0x12;
static const uint8_t CMD_GET_PROPERTY   = 0x13;   // 1.46.39：读属性（诊断用）
static const uint8_t CMD_FM_TUNE_FREQ   = 0x20;
static const uint8_t CMD_FM_SEEK_START  = 0x21;
static const uint8_t CMD_FM_TUNE_STATUS = 0x22;
static const uint8_t CMD_AM_TUNE_FREQ   = 0x40;
static const uint8_t CMD_AM_SEEK_START  = 0x41;
static const uint8_t CMD_AM_TUNE_STATUS = 0x42;

// ---------------- 属性号（AN332 表 24）----------------
static const uint16_t PROP_FM_DEEMPHASIS      = 0x1100;   // 0=75us 1=50us（中国用 50）
static const uint16_t PROP_FM_SEEK_BAND_BOTTOM = 0x1400;
static const uint16_t PROP_FM_SEEK_BAND_TOP    = 0x1401;
static const uint16_t PROP_FM_SEEK_FREQ_STEP   = 0x1402;
static const uint16_t PROP_FM_SEEK_TUNE_SNR_THRESHOLD = 0x1403;
static const uint16_t PROP_FM_SOFT_MUTE_MAX_ATTEN = 0x1302;  // 1.46.59：软静音最大衰减
static const uint16_t PROP_AM_CHANNEL_FILTER   = 0x3102;  // 低 4 位=带宽  bit12=电源噪声滤波
static const uint16_t PROP_AM_SOFT_MUTE_MAX_ATTEN = 0x3302;
static const uint16_t PROP_RX_VOLUME           = 0x4000;  // 0..63
static const uint16_t PROP_RX_HARD_MUTE        = 0x4001;  // bit1=左 bit0=右
// 1.46.65：弱台退单声道 / 窄带滤波（见 config.h 的 FM_STEREO_MIN_RSSI 那段）
static const uint16_t PROP_FM_CHANNEL_FILTER   = 0x1101;
static const uint16_t PROP_FM_BLEND_STEREO_THRESHOLD = 0x1102;

// AM 中频带宽：1 = 4kHz（说话听得清又不至于太吵）+ 电源线噪声抑制
static const uint16_t AM_FILTER_VALUE   = 0x1001;

// ---------------- 波段表（环的顺序 = 旋钮顺时针）----------------
struct BandInfo { int lo, hi, step; };
static const BandInfo kBands[RADIO_BANDS] = {
  { RADIO_LW_LO, RADIO_LW_HI, RADIO_LW_STEP },   // 0 长波
  { RADIO_MW_LO, RADIO_MW_HI, RADIO_MW_STEP },   // 1 中波
  { RADIO_FM_LO, RADIO_FM_HI, RADIO_FM_STEP },   // 2 调频
  { RADIO_SW_LO, RADIO_SW_HI, RADIO_SW_STEP },   // 3 短波
};

// ---------------- 状态 ----------------
// 1.22.0：芯片实际的 I2C 地址。SI473x 由 SEN 脚决定：SEN 低=0x11，SEN 高=0x63。
//   这块板实测是 **0x63**（自检 1.22.0 扫出来并 GET_CHIP_ID 确认过）。
//   写死任何一个都会在另一种板子上连不上，所以启动时两个都试。
static uint8_t  s_addr   = FM_I2C_ADDR;
static bool     s_ready  = false;          // 芯片起来了
static bool     s_am     = false;          // 芯片当前是 AM 模式（否则 FM）
static int      s_khz    = RADIO_START_KHZ;// 目标频率（屏幕上显示的就是它）
static int      s_applied = -1;            // 芯片已经按这个频率调好了
static int      s_vol    = FVOL_DEFAULT;
// 1.46.65：音量"写丢了"的保险。
//   SET_PROPERTY 是 I2C 上的一次事务，POWER_UP 后芯片正忙、或总线被抢，都可能
//   回 ERR —— 而 setProp 失败时**什么都不会发生**（芯片保持上一次的值，通常是
//   POWER_UP 的默认 A.fmVol 或者是更早的旧值）。用户那边看到的是屏上写着 63、
//   声音却停在 40 多，而且再怎么拧都不会变（因为 s_fmVol 已经是 63，主循环
//   认为"已经写过了"）。这里把失败记下来，fmLoop 里每秒补一次，直到写进去为止。
static bool     s_volLost = false;
static uint32_t s_volLostAt = 0;
static bool     s_mute   = false;
static int      s_rssi   = 0;              // 0..100

// ---- 1.22.0：静噪状态 ----
//  s_sqRaw  = 芯片原始 RSSI/SNR（不是换算后的 0..100），门限直接比这两个值
//  s_sqMute = 当前是否因为静噪而压着
//  s_sqSince= 「和上一次判定不一样」从什么时候开始 —— 要持续 SQUELCH_HOLD_MS 才动
static int      s_sqRawRssi = 0;
static int      s_sqRawSnr  = 0;
static bool     s_sqMute    = false;
static bool     s_sqWant    = false;
static uint32_t s_sqSince   = 0;

static uint32_t s_changedAt = 0;
static uint32_t s_lastPoll  = 0;

// ---------------- 波段环：数学部分（界面/预览页/手机是同一套规则）----------------
static inline int iabs(int v) { return v < 0 ? -v : v; }

int radioBandLo(int band)   { return kBands[band & 3].lo; }
int radioBandHi(int band)   { return kBands[band & 3].hi; }
int radioBandStep(int band) { return kBands[band & 3].step; }

int radioBand(int khz) {
  for (int b = 0; b < RADIO_BANDS; b++)
    if (khz >= kBands[b].lo && khz <= kBands[b].hi) return b;
  // 落在空档：归到更近的那个波段（比如 1000kHz 归中波，64000 归调频）
  int best = 0, bd = 0x7FFFFFFF;
  for (int b = 0; b < RADIO_BANDS; b++) {
    const int d = iabs(khz - kBands[b].lo);
    if (d < bd) { bd = d; best = b; }
    const int d2 = iabs(khz - kBands[b].hi);
    if (d2 < bd) { bd = d2; best = b; }
  }
  return best;
}

const char* radioBandCode(int khz) {
  static const char* kCode[RADIO_BANDS] = { "LW", "MW", "FM", "SW" };
  return kCode[radioBand(khz)];
}

// 1.37.0：**吸附到频道栅格** —— 原来这一步是缺的，是「频率越转越乱、带一堆零头」的根因。
//
//   旧版只做「夹进波段」，落在波段里就原样返回。于是只要 s_khz 里混进过一个非整值
//   （芯片回读、除 10 取整、I2C 抖一下…），之后每次 +100 都带着那个零头跑：
//       98123 → 98223 → 98323 …
//   屏上按 0.1MHz 截断显示，就成了「98.1 / 98.2 / 98.1」来回跳，
//   而且**再也回不到正好 98.0**。用户看到的就是「数字乱跳、频道乱窜」。
//
//   吸附之后，任何入口（旋钮 / 遥控 / 芯片回读 / App）进来的频率都落在本波段栅格上，
//   零头不会被带下去。开了半步进（A.halfstep）就吸附到半格，行为不变。
int radioSnap(int khz) {
  for (int b = 0; b < RADIO_BANDS; b++) {
    if (khz < kBands[b].lo || khz > kBands[b].hi) continue;
    int st = kBands[b].step;
    if (A.halfstep && st >= 2) st /= 2;         // 1.22.0 的半步进
    if (st < 1) st = 1;
    const int off = khz - kBands[b].lo;
    int k = kBands[b].lo + ((off + st / 2) / st) * st;   // 四舍五入到最近的格
    if (k > kBands[b].hi) k = kBands[b].hi;
    if (k < kBands[b].lo) k = kBands[b].lo;
    return k;
  }
  // 落在空档（比如 1000kHz 之外的缝里）：夹到最近的波段边界。边界本身一定在栅格上。
  int best = kBands[0].lo, bd = 0x7FFFFFFF;
  for (int b = 0; b < RADIO_BANDS; b++) {
    const int d1 = iabs(khz - kBands[b].lo);
    if (d1 < bd) { bd = d1; best = kBands[b].lo; }
    const int d2 = iabs(khz - kBands[b].hi);
    if (d2 < bd) { bd = d2; best = kBands[b].hi; }
  }
  return best;
}

// 转速 → 加速倍数。手停下来再转那一下（间隔 >400ms）算「精调」，一格一个频道；
// 正常速度一格 5 个频道，快转 10 个，甩起来 30 个 —— 老人不用慢慢挪，转两下就换台。
int radioGear(int msPerClick, int khz) {
  (void)khz;
  if (msPerClick <= 60)  return 30;   // 甩：中波 270kHz / 调频 3MHz 一格
  if (msPerClick <= 130) return 10;   // 快转
  if (msPerClick <= 400) return 5;    // 正常
  return 1;                           // 慢慢转：精调
}

// 1.22.0：半步进 —— 一格走半个频道。
//   只在 A.halfstep 为真时生效，默认关，所以老行为的回归向量一条都不用改。
//   除以 2 之后可能出现 4.5 / 2.5 这种非整数，一律向下取整到 1kHz 以上（不会走到 0）。
static inline int halfStepOf(int step) {
  if (!A.halfstep) return step;
  int h = step / 2;
  return h < 1 ? 1 : h;
}

int radioWalk(int khz, int clicks, int gear) {
  int k = radioSnap(khz);
  int b = radioBand(k);
  if (gear < 1) gear = 1;
  int n = clicks < 0 ? -clicks : clicks;
  if (n > 400) n = 400;                        // 转一圈的格数足够，防呆
  for (int i = 0; i < n; i++) {
    const int st = halfStepOf(kBands[b].step) * gear;
    if (clicks > 0) {
      int v = k + st;
      if (v > kBands[b].hi) { b = (b + 1) % RADIO_BANDS; v = kBands[b].lo; }
      k = v;
    } else {
      int v = k - st;
      if (v < kBands[b].lo) { b = (b + RADIO_BANDS - 1) % RADIO_BANDS; v = kBands[b].hi; }
      k = v;
    }
  }
  return k;
}

// ---------------- 底层 ----------------
static bool waitCts(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (Wire.requestFrom((uint8_t)s_addr, (uint8_t)1) == 1) {
      const uint8_t st = (uint8_t)Wire.read();
      if (st & 0x80) return (st & 0x40) == 0;   // CTS 置位；同时看 ERR
    }
    delay(2);
  }
  return false;
}

// 1.22.0：SI473x 的 I2C 地址由 SEN 脚决定（AN332）：
//    SEN = 低 → 0x11        SEN = 高 → 0x63
// 这块板实测是 **0x63**（自检 1.22.0 扫出来并 GET_CHIP_ID 确认过）。
// 厂商原理图写的是 0x11（SEN 低那版），所以只信配置会在这一批板子上永远连不上。
// 这里两个都试，找到哪个就用哪个 —— 两种板子都能跑。
static bool chipDetect() {
  const uint8_t cand[2] = {
    (uint8_t)FM_I2C_ADDR,
    (uint8_t)(FM_I2C_ADDR == 0x11 ? 0x63 : 0x11)
  };
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(cand[i]);
    if (Wire.endTransmission() == 0) {
      s_addr = cand[i];
      return true;
    }
  }
  return false;
}

// SET_PROPERTY 一次只写一条，够用且不会因为参数个数写错
static bool setProp(uint16_t prop, uint16_t val) {
  // ---- 1.46.60 修 P0：帧里多了一个 0x00 ----
  //
  //   AN332 / PU2CLR 的 SET_PROPERTY 帧是 **6 字节**：
  //       命令(0x12) + 保留(0x00) + 属性号(2) + 值(2)
  //   这里原来写了两个 0x00（ARG1 + ARG2），于是芯片按 6 字节去解析：
  //       属性号 = 0x00 <<8 | propHi        （错的）
  //       值     = propLo<<8 | valHi        （错的）
  //       剩下的 valLo 被当成下一条命令的开头
  //   ⇒ **一条属性都没写进去**，而且除了 ERR 位之外什么都不报（waitCts 只看 CTS/ERR，
  //      命令本身"收下了"，所以日志里一切正常）。从 1.10.0 加 FM 起就是错的。
  //
  //   它一次解释了三个老问题（都是"寄存器写了没反应"的症状）：
  //     · 底噪听不到：FM_SOFT_MUTE_MAX_ATTEN 一直是默认 16dB，空频率被软静音压掉
  //     · 收音音量不动：RX_VOLUME 一直是默认 63（回读得到 63 就是证据）
  //     · 64~87.5MHz 搜不到：FM_SEEK_BAND_BOTTOM 一直是默认 87.5MHz
  //   自检：写完立刻 GET_PROPERTY 回读，值不对就是帧错了（见 fmBegin 末尾那段）。
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_SET_PROPERTY);
  Wire.write(0x00);                    // 唯一的保留字节（ARG1）
  Wire.write((uint8_t)(prop >> 8)); Wire.write((uint8_t)(prop & 0xFF));
  Wire.write((uint8_t)(val  >> 8)); Wire.write((uint8_t)(val  & 0xFF));
  if (Wire.endTransmission() != 0) return false;
  return waitCts(200);
}

// POWER_UP：ARG1 = XOSCEN(bit4) | FUNC(bit0=1 → AM)   ARG2 = 0b0101（模拟音频输出，同原厂库）
static bool chipPowerUp(bool am) {
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_POWER_UP);
  // ARG1：低 4 位 = FUNC（0=FM，1=AM），bit4=XOSCEN，bit5=PATCH，bit6=GPO2OEN，bit7=CTSIEN
  //   1.46.62：原来写 0x10，等于把 XOSCEN 置 1（"用外部时钟/RCLK"）。这块板子是
  //   模块自带的 32.768kHz 晶体，原厂 PU2CLR 库走的是 XOSCEN=0。改成和原厂一致。
  Wire.write((uint8_t)(0x10 | (am ? 0x01 : 0x00)));   // 1.46.62b：保留 XOSCEN=1（实测去掉后芯片直接不响应）
  // ARG2：低 4 位 = OPMODE。原厂库 SI473X_ANALOG_AUDIO = **0b0101**（模拟音频输出）。
  //   1.46.62：原来写 0x00 —— 芯片照样应答、能调频率、能读到 RSSI/SNR，
  //   但**耳机口一点声音都没有**（连底噪都没有），正是用户报的那个症状。
  //   证据：桌面《收音机项目》里原厂 ALL_IN_ONE_T-Display_S3.ino 用 rx.setup()
  //   → SI4735::radioPowerUp() → Wire.write(powerUp.raw[1]) 恒为 0b0101。
  Wire.write((uint8_t)0x05);
  if (Wire.endTransmission() != 0) return false;
  return waitCts(600);
}

static void chipPowerDown() {
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_POWER_DOWN);
  Wire.endTransmission();
  waitCts(300);
  delay(20);                            // 手册要求 POWER_DOWN 后停一下再 POWER_UP
}

// 调频；不阻塞等锁定（芯片要 60~200ms 才稳），状态由 fmLoop 慢慢刷
static void tuneFmRaw(int freq10) {     // freq10：10kHz 单位，9800 = 98.0MHz
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_FM_TUNE_FREQ);
  Wire.write(0x00);                     // ARG1：bit0=FAST，这里走正常速度
  Wire.write((uint8_t)(freq10 >> 8)); Wire.write((uint8_t)(freq10 & 0xFF));
  Wire.endTransmission();
  waitCts(60);                          // 只等命令收下，不等锁定，否则旋钮连转会卡
}

static void tuneAmRaw(int khz) {        // AM 直接就是 kHz，15 位够到 32MHz
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_AM_TUNE_FREQ);
  Wire.write(0x00);                     // ARG1：bit0=FAST / bit1=FREEZE
  Wire.write((uint8_t)((khz >> 8) & 0x7F)); Wire.write((uint8_t)(khz & 0xFF));
  Wire.endTransmission();
  waitCts(60);
}

// 读一次调谐状态：READFREQ / RSSI / SNR
// 应答：STATUS(1) RESP1(1) READFREQ(2) RSSI(1) SNR(1) MULTI(1) ANTCAP(1)
// 调频的 READFREQ 是 10kHz 单位，中波/长波/短波直接是 kHz
// 1.46.39：GET_PROPERTY —— 读一条芯片属性（AN332 CMD 0x13）
//   返回 true 表示读到了，*val 是 16 位值。
static bool getProp(uint16_t prop, uint16_t* val) {
  Wire.beginTransmission(s_addr);
  Wire.write(CMD_GET_PROPERTY);
  Wire.write(0x00);                    // 保留
  Wire.write((uint8_t)(prop >> 8)); Wire.write((uint8_t)(prop & 0xFF));
  if (Wire.endTransmission() != 0) return false;
  if (!waitCts(200)) return false;
  uint8_t b[4];
  if (Wire.requestFrom((uint8_t)s_addr, (uint8_t)4) != 4) return false;
  for (int i = 0; i < 4; i++) b[i] = (uint8_t)Wire.read();
  // b[0]=status b[1]=保留 b[2..3]=值
  if (val) *val = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
  return true;
}

// 1.46.59：等芯片**真的搜完**（STC 位），不是等"命令被收下"。
//
//   为什么必须新加这个：SEEK_START 之后代码原来只 waitCts()，而 CTS 只表示
//   "命令收到了"。搜台每轮 40ms 就往下走，读回来的可能是搜到一半的频率，
//   更糟的是上一轮还没搜完就把下一条命令塞进去 —— 芯片回 ERR，读回原频率，
//   于是"f > s_scanCur"不成立，整个波段当场判定搜完（真机实测：4 个波段
//   全部搜完只要 0.9 秒，一个台都没找到）。
//
//   轮询 TUNE_STATUS 的 bit7（STC）是 AN332 规定的"调谐/搜台完成"标志。
//   INTACK 必须传 0，否则每读一次就把 STC 清掉，永远等不到。
static bool waitStc(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    Wire.beginTransmission(s_addr);
    Wire.write(s_am ? CMD_AM_TUNE_STATUS : CMD_FM_TUNE_STATUS);
    Wire.write(0x00);                    // ARG1 = 0：不动中断状态
    if (Wire.endTransmission() == 0 && waitCts(120)) {
      uint8_t b[7];
      if (Wire.requestFrom((uint8_t)s_addr, (uint8_t)7) == 7) {
        for (int i = 0; i < 7; i++) b[i] = (uint8_t)Wire.read();
        if (b[0] & 0x80) return true;    // bit7 = STC
      }
    }
    delay(8);
  }
  return false;
}

static bool readTuneStatus(int* khz, int* rssi, int* snr) {
  Wire.beginTransmission(s_addr);
  Wire.write(s_am ? CMD_AM_TUNE_STATUS : CMD_FM_TUNE_STATUS);
  Wire.write(0x01);                     // ARG1：bit0=INTACK，读的同时清中断
  if (Wire.endTransmission() != 0) return false;
  if (!waitCts(200)) return false;

  uint8_t b[7];
  if (Wire.requestFrom((uint8_t)s_addr, (uint8_t)7) != 7) return false;
  for (int i = 0; i < 7; i++) b[i] = (uint8_t)Wire.read();

  int f = ((int)b[2] << 8) | b[3];
  if (s_am) f &= 0x7FFF;               // AM 是 15 位，最高位是天线电容
  else      f *= 10;                   // 8750(10kHz) → 87500kHz
  if (khz)  *khz  = f;
  if (rssi) *rssi = b[4];              // 芯片给的 0..127（dBuV 的近似）
  if (snr)  *snr  = b[5];
  return true;
}

// 切芯片模式：AM ↔ FM。POWER_UP 之后音量/静音/属性全部回默认，必须重写。
static void switchMode(bool am) {
  chipPowerDown();
  if (!chipPowerUp(am)) {
    Serial.printf("[fm] 切到 %s 失败（POWER_UP 无 CTS）\n", am ? "AM" : "FM");
    return;
  }
  s_am = am;

  if (am) {
    // 中波/短波：4kHz 带宽、关掉软静音（不然弱台直接被吞掉，老人以为没台）
    setProp(PROP_AM_CHANNEL_FILTER,       AM_FILTER_VALUE);
    setProp(PROP_AM_SOFT_MUTE_MAX_ATTEN,  0);
  } else {
    setProp(PROP_FM_DEEMPHASIS,           FM_DEEMPHASIS);
    // 1.46.59：POWER_UP 会把所有属性复位，软静音必须在这里再设一次
    setProp(PROP_FM_SOFT_MUTE_MAX_ATTEN,  FM_SOFTMUTE_ATTEN);
    // 1.46.65：同上，立体声门限 / 带宽也是 POWER_UP 会清掉的
    setProp(PROP_FM_CHANNEL_FILTER,       FM_CHANNEL_FILTER_VAL);
    setProp(PROP_FM_BLEND_STEREO_THRESHOLD, FM_STEREO_MIN_RSSI);
    setProp(PROP_FM_SEEK_BAND_BOTTOM,     (uint16_t)(FM_BAND_BOTTOM * 10));  // 640 → 64.0MHz
    setProp(PROP_FM_SEEK_BAND_TOP,        (uint16_t)(FM_BAND_TOP * 10));     // 1080 → 108.0MHz
    setProp(PROP_FM_SEEK_FREQ_STEP,       (uint16_t)(FM_SEEK_STEP * 10));    // 100kHz → 10
    setProp(PROP_FM_SEEK_TUNE_SNR_THRESHOLD, 0);   // 不按信噪比卡搜台，弱台也停
  }
  // 音量/静音：每次 POWER_UP 都会被清掉
  // 1.44.0：音量就是芯片值，不换算
  setProp(PROP_RX_VOLUME,    (uint16_t)constrain(s_vol, 0, FVOL_MAX));
  // 1.22.0：硬静音有两个来源（用户静音 / 静噪），这里也要合成，
  //   否则切波段（AM↔FM）之后静噪状态被冲掉，空频道又开始沙沙响。
  setProp(PROP_RX_HARD_MUTE, (s_mute || s_sqMute) ? 0x0003 : 0x0000);

  Serial.printf("[fm] 芯片模式 → %s\n", am ? "AM（长波/中波/短波）" : "FM（调频）");
}

// 1.37.0：诊断计数 —— 串口里 10 秒报一次「真往芯片写了几次频率」。
//   闪/乱窜时这两个数一比，就能分清「是软件在反复写」还是「芯片/硬件的事」。
static uint32_t s_tuneN = 0;

// 1.40.1：搜台语音的待播标志。
//
//   为什么不能「一句 voicePlay 就完事」：
//   开机没网时 enterFm() 的顺序是 audioStop() → …→ fmScanStart()，
//   而 audioStop() 会把音频指令槽占成 CMD_STOP，此时 voicePlay()
//   会因为「指令槽不空」直接返回 false —— 于是开机那次自动搜台**听不到播报**。
//   （手动在 App 里点重新搜台时指令槽是空的，反而正常，这种「一半好一半不好」
//     最难查，所以这里做成「发不出去就记着，能发的时候再发」。）
//
//   过期时间 6 秒：搜台一共十几秒，超过 6 秒还没念出来就别念了 ——
//   等到快搜完才说「正在搜台」会很怪。
static bool     s_scanSayPending = false;
static uint32_t s_scanSayAt      = 0;

// 把目标频率真正写进芯片
static void applyTune() {
  if (!s_ready) return;
  s_tuneN++;
  const int b  = radioBand(s_khz);
  const bool am = (b != RADIO_FM);
  if (am != s_am) switchMode(am);
  if (am) tuneAmRaw(s_khz);
  else    tuneFmRaw(s_khz / 10);          // kHz → 10kHz 单位
  s_applied = s_khz;
  s_lastPoll = 0;                         // 立刻读一次信号
  // 1.22.0：换台了，静噪判定从头来 —— 旧台的"空频道"结论不能套到新台上，
  //   否则换到一个强台还要先静音 1.2 秒才放开，听着像卡了一下。
  s_sqWant = false;
  s_sqMute = false;
  s_sqSince = millis();
}

// ---------------- 对外接口 ----------------
bool fmBegin() {
  // ① 先给收音机那一路 LDO 上电（ATS MINI：IO15 = RADIO_EN，1=通电）
  if (PIN_FM_EN >= 0) {
    pinMode(PIN_FM_EN, OUTPUT);
    digitalWrite(PIN_FM_EN, HIGH);
    delay(120);
  }
  // ② 复位脉冲（ATS MINI：IO16 = SI4732 RST，低电平复位）
  if (PIN_FM_RST >= 0) {
    pinMode(PIN_FM_RST, OUTPUT);
    digitalWrite(PIN_FM_RST, LOW);
    delay(20);
    digitalWrite(PIN_FM_RST, HIGH);
    delay(150);
  }

  Wire.begin(PIN_FM_SDA, PIN_FM_SCL);
  Wire.setClock(100000);
  delay(20);

  if (!chipDetect()) {
    Serial.printf("[fm] I2C 0x%02X / 0x%02X 都没应答（SDA=%d SCL=%d），收音功能关闭\n",
                  (unsigned)FM_I2C_ADDR,
                  (unsigned)(FM_I2C_ADDR == 0x11 ? 0x63 : 0x11),
                  PIN_FM_SDA, PIN_FM_SCL);
    return false;
  }
  Serial.printf("[fm] 芯片地址确认 0x%02X\n", s_addr);

  // 1.22.0：芯片音量用**收音那一路**的初值，不是网络那个
  // 1.44.0：下限 FVOL_MIN（6），默认原厂 35
  s_vol = (A.fmVol >= FVOL_MIN) ? constrain(A.fmVol, FVOL_MIN, FVOL_MAX) : FVOL_DEFAULT;
  s_am  = false;
  if (!chipPowerUp(false)) {
    Serial.println("[fm] POWER_UP 没有 CTS，芯片没起来");
    return false;
  }

  // ---- 1.40.2 修 P0：这里必须把外部静音电路放开 ----
  //
  //  IO3 是这块板的 AUDIO_MUTE（原厂 #define AUDIO_MUTE 3），接的是一个
  //  「放大器输入端」的静音电路。原厂 SI4735 库在 radioPowerUp() 的**最后一句**
  //  一定调 setHardwareAudioMute(false) —— 也就是一上电就把这个脚拉低（放声）。
  //
  //  我们是手写 I2C 的，之前**漏了这一步**：整个 setup 里没有任何地方碰过 IO3，
  //  于是开机走网络模式时 IO3 一直是浮空。而 ESP32-S3 的 IO3 是 JTAG 的 strap 脚，
  //  上电默认有内部弱上拉 → 实际是 HIGH → 静音电路一直压着。
  //  症状就是「网络广播点了不出声」，而且和网络好不好完全没关系，极难查。
  //
  //  为什么放在这里（POWER_UP 之后）：原厂库也是这个位置。POWER_UP 瞬间芯片
  //  音频输出可能有跳动，先压住再放开才是防 POP 的正确顺序；反过来先放开
  //  再 POWER_UP，喇叭会「噗」一声。
  #if PIN_FM_AMP_MUTE >= 0
    pinMode(PIN_FM_AMP_MUTE, OUTPUT);
    digitalWrite(PIN_FM_AMP_MUTE, LOW);      // LOW = 放声（原厂 setHardwareAudioMute(false)）
    Serial.printf("[fm] 外部静音脚 IO%d 已放开（放声）\n", PIN_FM_AMP_MUTE);
  #endif

  // 频率单位 10kHz；步进 10 = 0.1MHz
  setProp(PROP_FM_DEEMPHASIS,        FM_DEEMPHASIS);
  // 1.46.59：放开软静音 —— 弱台和底噪都要能听见（手动调频靠耳朵找台）
  setProp(PROP_FM_SOFT_MUTE_MAX_ATTEN, FM_SOFTMUTE_ATTEN);
  // 1.46.65：弱信号优先听得清 —— 先窄带，再让立体声随 RSSI 退掉
  setProp(PROP_FM_CHANNEL_FILTER,     FM_CHANNEL_FILTER_VAL);
  setProp(PROP_FM_BLEND_STEREO_THRESHOLD, FM_STEREO_MIN_RSSI);
  // 1.46.60：芯片只认 76.0~108.0 做 SEEK 波段（AN332 Range 7600~10800）。
  //   这台机器手动调频能转到 64.0，但 SEEK 到不了那么低 —— 先按 64.0 发，
  //   芯片回 ERR（不是回 CTS）就退到 76.0，别让整条波段属性被拒。
  if (!setProp(PROP_FM_SEEK_BAND_BOTTOM, (uint16_t)(FM_BAND_BOTTOM * 10))) {
    setProp(PROP_FM_SEEK_BAND_BOTTOM, 7600);
    Serial.println("[fm] 芯片不收 64.0MHz 做 SEEK 下限 → 用 76.0MHz");
  }
  setProp(PROP_FM_SEEK_BAND_TOP,     (uint16_t)(FM_BAND_TOP * 10));
  setProp(PROP_FM_SEEK_FREQ_STEP,    (uint16_t)(FM_SEEK_STEP * 10));
  setProp(PROP_FM_SEEK_TUNE_SNR_THRESHOLD, 0);
  // 1.44.0：直接写芯片（原厂 rx.setVolume(volume) 就是这个意思）
  setProp(PROP_RX_VOLUME,            (uint16_t)s_vol);
  setProp(PROP_RX_HARD_MUTE,         0x0000);    // 双声道都开

  s_ready   = true;
  // 1.40.0：把音量打出来 —— 出「没声音」时第一眼看这行就知道是不是音量问题
  // 1.44.0：原厂口径，写进去多少就是多少，所以只打一个数
  Serial.printf("[fm] 音量 %d/63（原厂口径：直接写 RX_VOLUME）\n", s_vol);
  // 1.46.60：写完立刻回读 —— SET_PROPERTY 帧错的时候芯片不报错，只有回读能看出来
  {
    uint16_t sm = 0xFFFF, vo = 0xFFFF, hm = 0xFFFF, bb = 0xFFFF;
    getProp(PROP_FM_SOFT_MUTE_MAX_ATTEN, &sm);
    getProp(PROP_RX_VOLUME,              &vo);
    getProp(PROP_RX_HARD_MUTE,           &hm);
    getProp(PROP_FM_SEEK_BAND_BOTTOM,    &bb);
    Serial.printf("[fm] 属性回读 软静音=%d 音量=%d 硬静音=0x%04X SEEK下限=%d\n",
                  (int)sm, (int)vo, (unsigned)hm, (int)bb);
  }
  // 1.46.59：开机回到**上次听的那个频率**（A.fmFreq 由 storeLoadCfg 读回来）。
  //   自动搜台删掉之后，"开机落到信号最好的台"这条路没有了；如果每次都从 98.0
  //   起步，用户在别的城市/别的台就得每次重新转。
  {
    const int want = A.fmFreq;
    const bool ok = (want >= RADIO_LW_LO && want <= RADIO_SW_HI) ||
                    (want >= RADIO_FM_LO && want <= RADIO_FM_HI);
    s_khz = radioSnap(ok ? want : RADIO_START_KHZ);
  }
  s_applied = -1;
  applyTune();

  if (radioBand(s_khz) == RADIO_FM)
    Serial.printf("[fm] Si4732 就绪，FM 调到 %.1f MHz\n", s_khz / 1000.0);
  else
    Serial.printf("[fm] Si4732 就绪，%s 调到 %d kHz\n", radioBandCode(s_khz), s_khz);
  return true;
}

bool fmReady() { return s_ready; }

void fmTune(int khz) {
  if (!s_ready) return;
  const int k = radioSnap(khz);
  if (k == s_khz) return;
  s_khz = k;
  A.fmFreq = k;                    // 屏上立刻跟手（needFreq 比的就是它）
  s_changedAt = millis();          // 只记目标，fmLoop 等停手再真调
  // 1.46.59：记住这个频率。1.5 秒合并一次；收音时 I2S 那条链是空着的，
  //   写 flash 不会像网络音频那样"空一下"（见 store.cpp 1.46.29 那段）。
  storeSaveDebounced();
}

int fmFreq() { return s_khz; }
int fmRssi() { return s_rssi; }

void fmSeek(int dir) {
  if (!s_ready) return;
  if (s_khz != s_applied) applyTune();

  Wire.beginTransmission(s_addr);
  Wire.write(s_am ? CMD_AM_SEEK_START : CMD_FM_SEEK_START);
  Wire.write(dir >= 0 ? 0x01 : 0x00);   // ARG1：bit0=SEEKUP（bit1=WRAP 不开，到头就停）
  Wire.write(0x00);                     // ARG2：阈值 0
  if (Wire.endTransmission() != 0) return;
  waitCts(2500);                        // AM 搜台比 FM 慢

  int f = s_khz, r = 0, sn = 0;
  if (readTuneStatus(&f, &r, &sn)) { s_khz = f; s_applied = f; }
}

// 1.46.39：主动探一次（见 fm.h 的说明）
void fmProbe(int* khz, int* rssi, int* snr, int* muteReg, int* volReg, int* ready) {
  if (ready) *ready = s_ready ? 1 : 0;
  if (!s_ready) {
    if (khz) *khz = 0;
    if (rssi) *rssi = -1;
    if (snr) *snr = -1;
    if (muteReg) *muteReg = -1;
    if (volReg) *volReg = -1;
    return;
  }
  int f = 0, r = -1, n = -1;
  const bool ok = readTuneStatus(&f, &r, &n);
  if (khz)  *khz  = ok ? f : 0;
  if (rssi) *rssi = ok ? r : -1;
  if (snr)  *snr  = ok ? n : -1;
  uint16_t mv = 0xFFFF, vv = 0xFFFF;
  if (!getProp(PROP_RX_HARD_MUTE, &mv)) mv = 0xFFFF;
  if (!getProp(PROP_RX_VOLUME,    &vv)) vv = 0xFFFF;
  if (muteReg) *muteReg = (mv == 0xFFFF) ? -1 : (int)mv;
  if (volReg)  *volReg  = (vv == 0xFFFF) ? -1 : (int)vv;
}

void fmSetVolume(int v) {
  // 1.44.0：**不再换算**。原厂就是 rx.setVolume(volume)，v 直接是 0..63。
  //
  //   历史坑（1.40.0 修过一次，这里记着别再犯）：1.39.0 把界面音量改成 10 档后，
  //   这条路直接 setProp(PROP_RX_VOLUME, v) 把「档位」当寄存器值写，
  //   于是「音量 4」= 4/63 ≈ 6%，小喇叭上基本听不见 —— 症状就是「收音机没声音」，
  //   而屏上音量明明不是 0，看着像硬件坏了。
  //   现在上下限就是芯片的上下限，两边是同一个域，怎么算都不会错。
  if (v < FVOL_MIN) v = FVOL_MIN;
  if (v > FVOL_MAX) v = FVOL_MAX;
  s_vol = v;
  s_volLost = false;               // 新目标：之前那次丢没丢都不重要了
  if (!s_ready) return;
  s_volLost = !setProp(PROP_RX_VOLUME, (uint16_t)v);
  if (s_volLost) {
    s_volLostAt = millis();
    Serial.printf("[fm] 音量 %d 写入失败（I2C 没回 CTS），1 秒后补写\n", v);
  }
}

void fmMute(bool on) {
  s_mute = on;
  if (!s_ready) return;
  // 1.22.0：硬静音有两个来源 —— 用户按的静音、和静噪判定。两个是或的关系，
  //   否则「静噪压着时用户按取消静音」会把沙沙声放出来，白做了静噪。
  setProp(PROP_RX_HARD_MUTE, (on || s_sqMute) ? 0x0003 : 0x0000);
}

// ---- 1.22.0：静噪状态查询 ----
bool fmSquelched() { return s_sqMute; }
int  fmRawRssi()   { return s_sqRawRssi; }
int  fmRawSnr()    { return s_sqRawSnr; }

// ============================================================
//  1.38.0 自动搜台
//
//  做法：用芯片自带的 SEEK（FM_SEEK_START / AM_SEEK_START）一格格跳，
//  比逐个频率 TUNE 快十几倍（一个 SEEK 直接落到下一个强台，中间弱台根本不碰）。
//
//  为什么不用「逐个频率 TUNE + 读 RSSI」：
//    FM 64~108MHz 是 440 个 0.1MHz 频道，每调一个要等 CTS + 稳定 ≈ 60ms，
//    光调频就要 26 秒，加上中波/长波/短波要一分钟 —— 用户会以为死机。
//    SEEK 一次 0.3~0.8 秒，40 个台也就 15 秒左右，而且期间界面能刷新。
//
//  为什么不阻塞：做成状态机，主循环每次 fmLoop() 只推进一步。
//  搜台期间界面照常刷，屏上能显示「正在搜台 45%」。
// ============================================================
static int      s_scanN = 0;
static int      s_scanKhz[FM_SCAN_MAX];
static uint8_t  s_scanRssi[FM_SCAN_MAX];
static int      s_scanBand = 0;          // 当前搜到哪个波段（0=长波 1=中波 2=调频 3=短波）
static int      s_scanCur = 0;           // 本波段已经搜到哪个频率
static bool     s_scanBusy = false;
static uint16_t s_scanStall = 0;      // 1.46.42：SEEK 不前进的连续轮数
static uint8_t  s_scanModeFail = 0;   // 1.46.42：切波段连续失败次数
// 1.46.59（P0）：本波段是不是已经"定位到起点"了。
//   SEEK 本身就是"从当前频率往上找下一个"，所以每个波段只需要开头 TUNE 一次，
//   之后连着发 SEEK 就行。原来每轮都 TUNE 一次，芯片还在调谐就收到 SEEK
//   ⇒ 回 ERR ⇒ 读回原频率 ⇒ 整个波段当场判完（真机 0.9 秒搜完全部 4 个波段）。
static bool     s_scanStarted = false;
static bool     s_scanSaved = false;     // 本轮搜过台、还没存盘
static uint32_t s_scanNextAt = 0;

// 搜台顺序：先调频（最常用、台最多），再中波，再长波，最后短波。
//   kBands 的存储顺序是 长波/中波/调频/短波（环的顺序），搜的时候单独排。
static const int kScanOrder[4] = { RADIO_FM, RADIO_MW, RADIO_LW, RADIO_SW };

static void scanAdd(int khz, int rssi) {
  if (s_scanN >= FM_SCAN_MAX) return;
  for (int i = 0; i < s_scanN; i++) if (s_scanKhz[i] == khz) return;  // 去重
  s_scanKhz[s_scanN] = khz;
  s_scanRssi[s_scanN] = (uint8_t)(rssi < 0 ? 0 : (rssi > 100 ? 100 : rssi));
  s_scanN++;
  s_scanSaved = false;
}

// 排序：同波段内按信号降序。波段顺序由 kScanOrder 决定，用「波段权重」表达。
static int scanBandRank(int khz) {
  const int b = radioBand(khz);
  for (int i = 0; i < 4; i++) if (kScanOrder[i] == b) return i;
  return 9;
}

static void scanSort() {
  // 插入排序：最多 64 个，简单可靠
  for (int i = 1; i < s_scanN; i++) {
    const int k = s_scanKhz[i];
    const int r = s_scanRssi[i];
    const int br = scanBandRank(k);
    int j = i - 1;
    while (j >= 0) {
      const int br2 = scanBandRank(s_scanKhz[j]);
      const bool after = (br2 > br) || (br2 == br && s_scanRssi[j] < r);
      if (!after) break;
      s_scanKhz[j + 1] = s_scanKhz[j];
      s_scanRssi[j + 1] = s_scanRssi[j];
      j--;
    }
    s_scanKhz[j + 1] = k;
    s_scanRssi[j + 1] = (uint8_t)r;
  }
}

// 1.46.37：搜台开始时刻 —— 用来兜「s_scanBusy 被卡住」这个无底洞
static uint32_t s_scanAt = 0;

// 1.46.42：开搜之前先把旧台表备份一份。
//   搜台第一句就 `s_scanN = 0`，一旦搜到一半被 abort（换模式 / 网络回来），
//   旧表就永久没了 —— 真机上「台表 1 个 → 0 个」就是这么来的。
static int  s_bakN = 0;
static int  s_bakKhz[FM_SCAN_MAX];
static int  s_bakRssi[FM_SCAN_MAX];
static void scanBackup() {
  s_bakN = s_scanN;
  for (int i = 0; i < s_scanN && i < FM_SCAN_MAX; i++) {
    s_bakKhz[i]  = s_scanKhz[i];
    s_bakRssi[i] = s_scanRssi[i];
  }
}
// 新表为空、而备份里有东西 → 恢复备份（宁可留着旧表，也别变成 0 个台）
static void scanRestoreIfEmpty() {
  if (s_scanN > 0 || s_bakN <= 0) return;
  s_scanN = s_bakN;
  for (int i = 0; i < s_bakN && i < FM_SCAN_MAX; i++) {
    s_scanKhz[i]  = s_bakKhz[i];
    s_scanRssi[i] = s_bakRssi[i];
  }
  Serial.printf("[fm] 搜到 0 个，恢复上次的 %d 个台\n", s_scanN);
  evlog("fm", "搜到0个，恢复旧台表 %d 个", s_scanN);
}

// 1.46.45（P0）：搜台收尾 —— 必须把目标频率真正写回芯片。
//   搜台期间的调台是 scanTick 直接 tuneAmRaw()/tuneFmRaw()，**不经过 applyTune**，
//   所以 s_applied 还停在搜台前那个频率上。搜完如果只改 s_khz，
//   `s_khz != s_applied` 可能不成立 ⇒ applyTune 不执行 ⇒ 芯片留在搜台的
//   最后一个频率（中波空频道）⇒ 一点声音都没有。
//   顺带把还在跑的 SEEK 打断：不打断它，频率会自己一路滑下去。
static void scanFinishTune() {
  int target = s_khz;
  if (s_scanN > 0) target = s_scanKhz[0];      // 台表第一个（信号最好的）
  else             target = 98000;             // 一个台都没有 → FM 98.0 出底噪
  s_khz      = target;
  s_applied  = -1;                             // 哨兵：逼 applyTune 一定执行
  s_changedAt = millis();
  // !! 这里**不能**预先设 s_am：applyTune 靠  才知道要不要 switchMode。
  //   我一开始写了 s_am = ...，结果搜台结束时芯片还在 AM 模式，
  //   applyTune 以为不用切，直接发了 FM_TUNE_FREQ ⇒ 芯片忽略 + 之后
  //   readTuneStatus 全失败（/api/fmprobe 读到 khz=0 rssi=-1）。
  applyTune();                                 // 内部会自己切模式，并打断 SEEK
  A.fmFreq  = s_khz;
  A.fmRssi  = fmRssi();
  A.dirty   = true;
  Serial.printf("[fm] 收尾：芯片调到 %d kHz（%s）\n", s_khz, radioBandCode(s_khz));
  evlog("fm", "搜台收尾 → 调到 %d kHz", s_khz);
}

// 1.46.42：进收音后要「延后」才自动搜台 —— 避开开机 WiFi 抢连的那 3 秒窗口
static uint32_t s_scanWantAt = 0;
void fmScanWantSoon(uint32_t delayMs) { s_scanWantAt = millis() + delayMs; }
bool fmScanWanted() { return s_scanWantAt != 0; }
// 由 fmLoop 调用：到点就开搜
static void scanWantTick(uint32_t now) {
  if (!s_scanWantAt) return;
  if ((int32_t)(now - s_scanWantAt) < 0) return;
  s_scanWantAt = 0;
  if (!s_scanBusy) fmScanStart();
}

// 中断搜台：只清「正在搜」的状态，**保留已经搜到的台**（半份表也比没有强）
void fmScanAbort() {
  const bool wasBusy = s_scanBusy;
  s_scanStarted = false;      // 1.46.47：已经搜完的不用再存一次盘
  if (wasBusy) Serial.printf("[fm] 搜台中途中止（已搜到 %d 个）\n", s_scanN);
  s_scanBusy = false;
  s_scanSayPending = false;
  s_scanWantAt = 0;                 // 1.46.42：排队的自动搜台也一起取消
  // 1.46.42：一个都没搜到就把旧表还回来 —— 不然用户从此只有 0 个台
  scanRestoreIfEmpty();
  if (wasBusy && s_scanN > 0) { scanSort(); fmScanSave(); }
  // 1.46.45（P0）：半途退出时芯片停在搜到一半的频点上（多半是空频道），
  //   必须调回台表第一个台，否则用户切回收音听到的就是"没声音"。
  if (s_ready) scanFinishTune();
}

int  fmScanCount()      { return s_scanN; }
int  fmScanFreq(int i)  { return (i >= 0 && i < s_scanN) ? s_scanKhz[i] : 0; }
int  fmScanRssi(int i)  { return (i >= 0 && i < s_scanN) ? s_scanRssi[i] : 0; }
bool fmScanBusy()       { return s_scanBusy; }
// 1.46.42：进度要**连续**动，不能按波段整块跳。
//   原来 `s_scanBand * 100 / 4` —— FM 是第一个波段，整个 FM 搜完之前
//   永远是 0%，用户盯着 "正在搜台 0%" 几十秒，只能以为死机了。
//   现在：波段占 25%，波段内按「当前频率在本波段的相对位置」插值。
int  fmScanProgress() {
  if (!s_scanBusy) return 100;
  const int band = kScanOrder[s_scanBand & 3];
  const int lo = radioBandLo(band), hi = radioBandHi(band);
  int within = 0;
  if (hi > lo && s_scanCur > lo) {
    const long span = (long)(hi - lo);
    within = (int)(((long)(s_scanCur - lo) * 100L) / span);
    if (within < 0) within = 0;
    if (within > 100) within = 100;
  }
  int p = (s_scanBand * 100 + within) / 4;
  if (p < 0) p = 0;
  if (p > 99) p = 99;            // 100 只留给「真的搜完」
  return p;
}

void fmScanStart() {
  if (!s_ready) { Serial.println("[fm] 搜台被拒：芯片没就绪"); return; }
  if (s_scanBusy) {
    // 1.46.37：以前这里一声不响就 return，卡住之后再也搜不了，用户只看到「搜不到」。
    //   现在带 120 秒超时：一次搜台最多十几秒，超过这个数一定是卡住了，强制重来。
    if (s_scanAt && millis() - s_scanAt < 120000u) {
      Serial.println("[fm] 搜台被拒：上一次还在搜");
      return;
    }
    Serial.println("[fm] 上一次搜台卡住了，强制重来");
    fmScanAbort();
  }
  scanBackup();                     // 1.46.42：先把旧表留一份
  s_scanN = 0;
  s_scanBand = 0;
  s_scanCur = radioBandLo(kScanOrder[0]);
  s_scanBusy = true;
  s_scanStarted = false;
  s_scanAt = millis();
  s_scanNextAt = millis();
  s_scanStall = 0;
  s_scanModeFail = 0;
  s_scanWantAt = 0;
  Serial.println("[fm] 开始自动搜台（调频 → 中波 → 长波 → 短波）");

  // 1.40.1：开口说一声「正在搜台，请稍等」。
  //
  //   搜台要十几秒，屏上虽然写着「正在搜台 45%」，但老人转旋钮时看的是旋钮不是屏。
  //   给一声提示，他就知道"机器在干活"而不是"又坏了"，不会中途关机。
  //
  //   会不会打断节目？不会 —— 走到这里一定是收音模式（网络流已 audioStop），
  //   而收音的声音是芯片走 3.5mm 进功放的，I2S 是另一路，两路在功放输入端叠加，
  //   所以听起来就是"广播里有人在说正在搜台"。
  //   如果正在念别的（比如电量低），voicePlay 会自己返回 false，不插队。
  if (voicePlay(VO_SCAN)) {
    s_scanSayPending = false;
  } else {
    s_scanSayPending = true;         // 指令槽被占（开机时是 audioStop）：记着，稍后补
    s_scanSayAt = millis();
  }
}

// 在台表里走一格：dir>0 往后，dir<0 往前，做首尾环绕
int fmScanStep(int fromKhz, int dir) {
  if (s_scanN <= 0) return fromKhz;
  // 找到当前在表里的位置（允许「不在表里」→ 落到最近的）
  int best = 0, bd = 0x7FFFFFFF;
  for (int i = 0; i < s_scanN; i++) {
    const int d = iabs(s_scanKhz[i] - fromKhz);
    if (d < bd) { bd = d; best = i; }
  }
  int i = best;
  if (bd != 0) {                              // 不在表里：就近落地，不额外走一格
    return s_scanKhz[i];
  }
  i += (dir >= 0 ? 1 : -1);
  if (i >= s_scanN) i = 0;
  if (i < 0) i = s_scanN - 1;
  return s_scanKhz[i];
}

// 1.38.0：搜台时的「够不够格」判据 —— 只收信号明显高于底噪的，
//   避免把沙沙声、电源噪声也列成台（那些台点进去只有噪音）。
// 1.46.47：门槛从 25 降到 12。
//   25 大致等于芯片 RSSI >= 23，室内短天线根本过不了 —— 用户机器实测
//   98.0MHz 只有 RSSI 35，周边弱台全被拒，看起来就是「收频段搜不到」。
//   FM 已经不静噪了，弱台收进来让用户自己转着听，比一个都搜不到强。
//   中波/短波那边沙沙响，门槛保持高一点（否则全是噪声台）。
static bool s_squelchThreshold(int pct) { return pct >= 12; }

// 每次 fmLoop 调一次；搜完自己退出
static void scanTick() {
  if (!s_scanBusy) return;
  const uint32_t now = millis();

  // 1.40.1：补念「正在搜台」。
  //   放在 s_scanNextAt 判据**之前** —— 这一行不该被搜台的节流挡住，
  //   否则每轮要等 40~400ms、还要跟 SEEK 抢，念出来的时机就飘了。
  if (s_scanSayPending) {
    if (voicePlay(VO_SCAN))            s_scanSayPending = false;
    else if (now - s_scanSayAt > 6000) s_scanSayPending = false;   // 太久了就不念了
  }

  if (now < s_scanNextAt) return;
  if (!s_ready) { s_scanBusy = false; return; }

  // 这一轮搜的波段
  const int band = kScanOrder[s_scanBand];
  const int lo = radioBandLo(band), hi = radioBandHi(band);

  if (s_scanCur > hi) {                       // 本波段搜完，换下一个
    s_scanBand++;
    if (s_scanBand >= 4) {
      s_scanBusy = false;
      scanRestoreIfEmpty();           // 1.46.42：全空就把旧表还回来
      scanSort();
      fmScanSave();
      Serial.printf("[fm] 搜台完成，共 %d 个台\n", s_scanN);
      // 1.46.45（P0）：这里原来只改 s_khz，芯片其实没被调过去（见 scanFinishTune）
      scanFinishTune();
      return;
    }
    s_scanCur = radioBandLo(kScanOrder[s_scanBand]);
    s_scanStarted = false;            // 1.46.59：新波段要重新定位一次
    s_scanNextAt = now + 60;
    return;
  }

  // 切到本波段（AM↔FM 要 POWER_DOWN/UP，约 0.3~0.5 秒）
  const bool am = (band != RADIO_FM);
  if (am != s_am) {
    // 1.46.42：switchMode 失败时 s_am 不会更新，原来会**每轮重试 POWER_UP** ——
    //   一次 0.4 秒、无限循环，进度永远 0%。连失败 3 次就认输，结束搜台。
    switchMode(am);
    if (s_am != am) {
      if (++s_scanModeFail >= 3) {
        Serial.printf("[fm] 切到 %s 连续失败 %d 次 → 结束搜台\n",
                      am ? "AM" : "FM", s_scanModeFail);
        evlog("fm", "切波段失败 3 次，结束搜台");
        s_scanBusy = false;
        scanRestoreIfEmpty();
        if (s_scanN > 0) { scanSort(); fmScanSave(); }
        scanFinishTune();          // 1.46.45：同上，别把芯片丢在半路
      } else {
        s_scanNextAt = now + 400;
      }
      return;
    }
    s_scanModeFail = 0;
    s_scanStarted = false;            // 1.46.59：POWER_UP 之后芯片在默认频率上
    s_scanNextAt = now + 400;
    return;
  }

  // ---- 1.46.59（P0）：每个波段只在**开头**定位一次，之后连续 SEEK ----
  //
  //   这个坑 1.39.0 踩过、1.46.42 重构时又回来了，症状是「整个波段一个台都搜不到」：
  //   tuneFmRaw() 里的 waitCts(60) 只保证"命令被收下"，不保证锁定；60ms 后
  //   SEEK_START 发下去，芯片还在调谐 ⇒ 回 ERR / 被忽略 ⇒ readTuneStatus()
  //   读回的还是刚 TUNE 的那个频率 ⇒ `f > s_scanCur` 不成立 ⇒ 走 else 直接
  //   `s_scanCur = hi + 1` ⇒ 本波段"搜完了"。4 个波段加起来只要 0.9 秒。
  //   用户看到的就是「只有一个台（还是上次存的残废表），我别的收音机能搜到好几个」。
  if (!s_scanStarted) {
    if (am) tuneAmRaw(s_scanCur); else tuneFmRaw(s_scanCur / 10);
    waitStc(am ? 900 : 450);          // 等真锁定（STC），不是等 CTS
    { int tf = 0, tr = 0, tn = 0; readTuneStatus(&tf, &tr, &tn); }   // 顺手清掉 STC
    s_scanStarted = true;
    s_scanNextAt  = now + 30;
    return;
  }

  // 发 SEEK：一次跳到下一个强台
  Wire.beginTransmission(s_addr);
  Wire.write(am ? CMD_AM_SEEK_START : CMD_FM_SEEK_START);
  Wire.write(0x01);                 // ARG1：bit0=SEEKUP（向上搜）
  Wire.write(0x00);                 // ARG2：阈值 0
  Wire.endTransmission();
  waitCts(3000);                    // 先等命令被收下
  waitStc(3000);                    // 1.46.59：再等真的搜完（AM 比 FM 慢）

  int f = s_scanCur, r = 0, sn = 0;
  const int before = s_scanCur;
  // 1.46.59：`f < hi` 而不是 `<= hi`。SEEK 到波段顶端还没找到台时，芯片会停在
  //   顶端（FM 就是 108.0MHz）并置 BL 位 —— `<= hi` 会把顶端当成一个"台"记进表里。
  if (readTuneStatus(&f, &r, &sn) && f > s_scanCur && f < hi) {
    // 芯片给的 0..127；换算成 0..100 好排序
    int pct;
    if (am) pct = (r - 4) * 100 / 50; else pct = (r - 8) * 100 / 60;
    if (s_squelchThreshold(pct) || r >= (am ? SQUELCH_AM_RSSI : SQUELCH_FM_RSSI))
      scanAdd(f, pct);
    Serial.printf("[sw] 命中 %s %d kHz rssi=%d snr=%d\n",
                  am ? "AM" : "FM", f, r, sn);   // 1.46.60 临时诊断
    s_scanCur = f;
    s_scanStall = 0;
  } else if (f >= hi) {
    s_scanCur = hi + 1;             // 真到头了（芯片停在波段顶端），本波段结束
    s_scanStall = 0;
  } else {
    // 既没前进、也没到顶 = 这条 SEEK 被芯片拒了（ERR / 还在调谐）。
    // 1.39.0 的教训：别一失败就判"本波段搜完了"，重发几轮再认输。
    if (++s_scanStall >= 6) {
      Serial.printf("[fm] %s 波段 SEEK 连续 %d 次没前进 → 跳过本波段\n",
                    am ? "AM" : "FM", (int)s_scanStall);
      s_scanCur = hi + 1;
      s_scanStall = 0;
    }
  }
  (void)before;
  s_scanNextAt = now + 40;
}

// ---- 搜台结果存盘：/fm_scan.txt，一行一个「频率,信号」----
//   为什么存：开机不能每次都搜（一次十几秒），老人会以为机器坏了。
//   存下来，开机直接能用；想重搜在 App 里点一下。
void fmScanSave() {
  if (s_scanN <= 0) return;
  File f = LittleFS.open("/fm_scan.txt", "w");
  if (!f) { Serial.println("[fm] 搜台结果存盘失败"); return; }
  for (int i = 0; i < s_scanN; i++) f.printf("%d,%d\n", s_scanKhz[i], s_scanRssi[i]);
  f.close();
  s_scanSaved = true;
  Serial.printf("[fm] 搜台结果已存盘（%d 个台）\n", s_scanN);
}

void fmScanLoad() {
  if (!LittleFS.exists("/fm_scan.txt")) return;
  File f = LittleFS.open("/fm_scan.txt", "r");
  if (!f) return;
  s_scanN = 0;
  while (f.available() && s_scanN < FM_SCAN_MAX) {
    const String ln = f.readStringUntil('\n');
    const int c = ln.indexOf(',');
    if (c <= 0) continue;
    const int k = ln.substring(0, c).toInt();
    const int r = ln.substring(c + 1).toInt();
    if (k > 0) scanAdd(k, r);
  }
  f.close();
  if (s_scanN > 0) {
    scanSort();
    Serial.printf("[fm] 读回上次搜的 %d 个台（第一个 %d kHz）\n", s_scanN, s_scanKhz[0]);
  }
}

void fmLoop() {
  scanWantTick(millis());          // 1.46.42：到点才真正开搜（见 fmScanWantSoon）
  if (!s_ready) return;
  const uint32_t now = millis();

  // 1.38.0：搜台期间独占 I2C —— 先把这一轮搜台推进一步，然后直接返回，
  //   免得调台轮询和搜台抢总线（抢起来两边都失败，就是「搜出来的台不准」）。
  if (s_scanBusy) { scanTick(); return; }

  // 1.37.0 诊断：每 10 秒报一次「这 10 秒真往芯片写了几次频率」。
  //   只在数字变了才打，不会刷屏。
  {
    static uint32_t diagAt = 0, tuneSeen = 0;
    if (now - diagAt > 10000) {
      diagAt = now;
      if (s_tuneN != tuneSeen) {
        Serial.printf("[fm] 10s 内调台 %lu 次（目标 %d kHz / %s）\n",
                      (unsigned long)(s_tuneN - tuneSeen), s_khz, radioBandCode(s_khz));
        tuneSeen = s_tuneN;
      }
    }
  }

  // ① 追目标：停手 160ms 才真调台（连转时不会把 I2C 打满、也不会一路「滋啦」）
  if (s_khz != s_applied && now - s_changedAt >= 160) applyTune();

  // ①-b 1.46.65：音量写失败就补写，别让用户"拧到最大却比原来还小"
  if (s_volLost && now - s_volLostAt >= 1000) {
    s_volLostAt = now;
    if (setProp(PROP_RX_VOLUME, (uint16_t)s_vol)) {
      s_volLost = false;
      Serial.printf("[fm] 音量 %d 已补写进芯片\n", s_vol);
    }
  }

  // ② 刷信号：1.5 秒一次就够，别占 I2C
  if (now - s_lastPoll < 1500) return;
  s_lastPoll = now;

  // 1.37.0：手还在转、或芯片还没跟上时，TUNE_STATUS 读到的是**上一个台**的东西。
  //   这种时候读出来的频率/信号全是过期的，拿它做什么都会把界面搞乱。
  if (s_khz != s_applied) return;

  int f = s_khz, r = 0, sn = 0;
  if (!readTuneStatus(&f, &r, &sn)) return;

  // ===== 1.37.0 最重要的一处 =====
  // 这里原来有一行 `if (s_khz == s_applied) s_khz = f;` —— 把芯片回读的频率**写回目标值**。
  //   那一行是「调好的台自己跳回去、数字跟着抖」的**直接原因**，已经删掉。
  //
  //   为什么必须删：s_khz 是「用户想去哪儿」的唯一真相，芯片回读是「芯片现在在哪」。
  //   两者互相覆盖就会成环 ——
  //     读到旧值 → s_khz 变成旧值 → s_khz != s_applied
  //     → 160ms 后 applyTune 把旧值写进芯片 → 芯片真的被拉回去 → 1.5s 后再读到旧值…
  //   于是频率永远在两个值之间来回，界面数字一秒跳一次，听感就是「频道乱窜」。
  //   更糟的是回读值一旦落进别的波段区间，还会触发 AM↔FM 反复切模式
  //   （就是日志里那一串「芯片模式 → AM / → FM」）。
  //
  //   回读现在只用于诊断：偏差超过一个步进就记一笔，**不改任何状态**。
  {
    static uint32_t saidAt = 0;
    const int step = radioBandStep(radioBand(s_khz));
    if (f != s_khz && (f - s_khz > step || s_khz - f > step) && now - saidAt > 8000) {
      saidAt = now;
      Serial.printf("[fm] 芯片回读 %d kHz ≠ 目标 %d kHz（差 %d），按目标显示、不覆盖\n",
                    f, s_khz, f - s_khz);
    }
  }

  // ---- 1.22.0：静噪 ----
  // 门限用芯片原始值比（r / sn），不用换算后的 0~100 —— 少一次换算少一处误差。
  s_sqRawRssi = r;
  s_sqRawSnr  = sn;
  {
    // A.squelch = 0 表示关闭静噪；>0 时它是个 0..100 的"灵敏度旋钮"：
    //   值越大 = 门限越高 = 越容易静音（只留强台）
    // 映射到芯片原始域：0..100 → 0..(门限上限)。
    const int lim_r = s_am ? SQUELCH_AM_RSSI : SQUELCH_FM_RSSI;
    const int lim_s = s_am ? SQUELCH_AM_SNR  : SQUELCH_FM_SNR;
    // 1.46.41：**FM 一律不静噪**。
    //   真机实测：98.0MHz 的 rssi=17 / snr=1，squelch=75 时门限 17.5/7，
    //   正好把它判成「空频道」→ 芯片硬静音 → 用户听到「没声音、也没噪音」，
    //   而屏上还写着在播。FM 空频道本来就是沙沙，用户听得出来；
    //   静噪一旦误判却是完全无声，比沙沙难查得多。AM/SW 保留（那边沙沙又响又吵）。
    bool want = false;
    if (A.squelch > 0 && s_am) {
      // A.squelch=1 → 几乎不静音（门限贴着 0）；=100 → 门限顶到 lim 的 3 倍
      // 1.46.39：FM 的门限**不能再放宽到 3 倍**。
      //   原来 squelch=75 时 FM 的 RSSI 门限 = 10 + 0.75*10*2 = 25，
      //   室内/郊区的正常台（RSSI 18~25）会被整片判成「空频道」→ 硬静音
      //   → 用户听到「没声音、也没噪音」，而屏上还写着「广播 FM 98.0」，看
      //   起来像机器坏了。AM/SW 空频道是真的一直沙沙响，那边保留 3 倍。
      const int mul = s_am ? 2 : 1;      // 100% 时：AM 3 倍门限 / FM 2 倍门限
      const int thrR = lim_r + (A.squelch * lim_r * mul) / 100;
      const int thrS = lim_s + (A.squelch * lim_s * mul) / 100;
      want = (r < thrR) && (sn < thrS);   // 两个都低才算"空频道"
    }
    // 抖动保护：判定要稳定 SQUELCH_HOLD_MS 才真的动
    if (want != s_sqWant) { s_sqWant = want; s_sqSince = now; }
    else if (s_sqMute != s_sqWant && now - s_sqSince >= SQUELCH_HOLD_MS) {
      s_sqMute = s_sqWant;
      // 静噪是"没有台"，不是用户按了静音 —— 所以只压芯片硬静音，
      // 不碰 A.muted（顶栏不该显示「已静音」把用户吓到）
      if (s_ready) setProp(PROP_RX_HARD_MUTE,
                           (s_sqMute || s_mute) ? 0x0003 : 0x0000);
      Serial.printf("[fm] 静噪 %s（RSSI=%d SNR=%d 门限 %s）\n",
                    s_sqMute ? "开" : "关", r, sn, s_am ? "AM" : "FM");
    }
  }

  // 芯片 RSSI 大概是 0~75：-20dBuV 到 +60dBuV。换算成顶栏那四格用的 0~100
  // 中波/短波整体电平低一档，曲线放宽一点，不然弱台永远零格
  int pct;
  if (s_am) pct = (r - 4) * 100 / 50;
  else      pct = (r - 8) * 100 / 60;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  s_rssi = pct;
  A.rssi = -(100 - pct) / 1.6;              // 顺手给顶栏一个负 dBm 的样子
  if (A.rssi < -95) A.rssi = -95;
}

void fmDiag() {
  Serial.println("[fm] ---- 自检 ----");
  Serial.printf("[fm] 引脚 SDA=%d SCL=%d RST=%d EN=%d 实际地址=0x%02X（配置 0x%02X）\n",
                PIN_FM_SDA, PIN_FM_SCL, PIN_FM_RST, PIN_FM_EN,
                s_addr, (unsigned)FM_I2C_ADDR);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[fm]   总线上有设备: 0x%02X\n", a);
      found++;
    }
  }
  if (!found) Serial.println("[fm]   整条总线是空的：查 SDA/SCL 有没有接反、模块有没有供电");
  Serial.printf("[fm] ready=%d am=%d khz=%d(%s) rssi=%d\n",
                s_ready ? 1 : 0, s_am ? 1 : 0, s_khz, radioBandCode(s_khz), s_rssi);
  // 把四个波段的边界打出来，对不上实物时一眼就能看出是不是波段表填错
  for (int b = 0; b < RADIO_BANDS; b++)
    Serial.printf("[fm]   波段%d %s %d~%d kHz 步进%d\n",
                  b, b == RADIO_LW ? "长波" : b == RADIO_MW ? "中波"
                   : b == RADIO_FM ? "调频" : "短波",
                  kBands[b].lo, kBands[b].hi, kBands[b].step);
}

#else   // FM_ENABLE == 0：全部空实现，保证其它文件照常编译

bool fmBegin() { return false; }
bool fmReady() { return false; }
void fmTune(int) {}
int  fmFreq() { return RADIO_START_KHZ; }
int  fmRssi() { return 0; }
void fmSeek(int) {}
void fmSetVolume(int) {}
void fmMute(bool) {}
void fmLoop() {}
void fmDiag() { Serial.println("[fm] 未启用（FM_ENABLE=0）"); }

int radioBandLo(int)   { return RADIO_LW_LO; }
int radioBandHi(int)   { return RADIO_LW_HI; }
int radioBandStep(int) { return RADIO_LW_STEP; }
int radioBand(int khz) { return khz >= RADIO_FM_LO ? RADIO_FM : RADIO_LW; }
const char* radioBandCode(int khz) { return khz >= RADIO_FM_LO ? "FM" : "MW"; }
int radioSnap(int khz) { return khz; }
int radioGear(int, int) { return 1; }
int radioWalk(int khz, int clicks, int gear) { (void)clicks; (void)gear; return khz; }

#endif
