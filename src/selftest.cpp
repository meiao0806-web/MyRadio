// ============================================================
//  selftest.cpp —— 硬件自检固件（只在 -DHWTEST=1 时编译进去）
//
//  1.22.0：加了「假总线识别」和「0x63 备用地址」——
//  这两个都是真机第一轮自检炸出来的：
//
//  ① 假总线
//     SDA=1/SCL=2（编码器那两条）上，0x08~0x11 连续 10 个地址全部"应答"。
//     那不是总线，那是两根浮空输入脚：上拉不足时读到的一直是 0xFF，
//     Wire.endTransmission() 就全返回 0（成功）。
//     判据：一对脚上应答地址数 > 5 ⇒ 判为浮空/短路，直接跳过，别拿它当结论。
//
//  ② 0x63
//     SDA=18/SCL=17 上有一个真设备，但地址是 **0x63**，不是 0x11。
//     SI473x 的 I2C 地址由 SEN 脚电平决定（AN332 第 4 章）：
//        SEN = 低 → 0x11        SEN = 高 → 0x63
//     这块板子把 SEN 拉高了。所以固件里写死 0x11 是连不上的 ——
//     这解释了"收音一直没有任何反应"，也说明厂商原理图那行 I2C 地址是给 SEN=低的版本。
//
//  阶段顺序刻意设计成"能自证的在前"：
//    1  背光候选脚逐个拉高（累计）—— 看屏有没有微光，记第几个
//    2  I2C 扫 14 对候选脚 + 假总线过滤 + GET_CHIP_ID 确认 + 0x11/0x63 都试
//    3  用当前 TFT 配置初始化屏，把阶段 2 结果画在屏上
//    4  I2S 逐组出声（4 秒 1kHz 正弦），听哪组响
//
//  安全约束（改候选表前必读）：
//    禁止碰：26~32 flash、33~37 八线 PSRAM 数据线、19/20 USB、45/46 strapping
//    已占用：1,2 编码器 / 4 电池ADC / 10-14 屏幕SPI / 15 电源自锁 /
//            16 SI4732复位 / 21 旋钮键 / 38 背光
// ============================================================
#include "config.h"

#if HWTEST

#include <Arduino.h>
#include <math.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <SPI.h>          // 阶段 3 的裸 ST7789 驱动要用（引脚运行时可改）

#include "ui.h"

// ------------------------------------------------------------
//  阶段 1：背光候选（累计点亮）
// ------------------------------------------------------------
// 背光候选（1.24.0 扩到 10 个）
//
// 为什么不只试 IO38：IO38 这个值是从 **ATS-MINI**（并口屏那台）抄来的，
// 而这台 abdef2022 v2.2 是 **ST7789 SPI 屏 + 拨轮编码器 + PCM5102**，
// 引脚表整个不同。同理 10~14 那组 SPI 引脚也是抄错的产物 —— 所以 10 组全不亮。
//
// 候选排除了绝对不能让开的脚：
//   26~32 flash、33~37 八线 PSRAM、19/20 USB、45/46 strapping、
//   1/2 编码器、4 电池ADC、15 电源自锁、16 SI4732复位、17/18 I2C、21 旋钮键
// 1.27.1：**IO38 打头**。依据是硬的：IO38=HIGH 时屏上有微光，换到 IO42 后全黑，
// 说明 38 才是背光（42 那次数错了）。42 保留在后面，别删。
// 1.27.3：**IO47 排第一**。依据：1.24.0 那轮你报"有微光闪烁"时，
// 串口正好停在 IO47 的高/低两组上 —— 那是最接近"真实观测"的证据。
// 38 / 42 都试过当 TFT_BL，屏都不亮，所以往后排。
// 1.29.0：这一版**不再让人数闪几下**，改用「全开」策略。
//   ��因：原厂固件刚被确认能让这块屏正常显示 ⇒ 硬件通路 100% 没问题，
//   唯一变量就是我们的引脚/参数。既然屏是好的，只要背光和 SPI 里有一样错，
//   屏幕就是全黑；那就不该再把判断成本压在用户身上去数闪几下。
//   做法：把 10~14 以外的所有可疑脚**全部拉高**（若其中有低有效的，后面阶段
//   再单独扫低有效），保证背光一定亮着；然后只扫 SPI。看到白/红/绿 = 全对。
static const uint8_t kBlPins[] = { 38, 47, 48, 42, 3, 9, 0, 5, 6, 7, 39, 40, 41 };
static const int kBlN = (int)(sizeof(kBlPins) / sizeof(kBlPins[0]));

// ------------------------------------------------------------
//  阶段 2：I2C 候选（SDA, SCL）
//    18/17 第一优先：原版 ATS-MINI 原理图 SCL=IO17 SDA=IO18 + LilyGO T-Display-S3 默认
// ------------------------------------------------------------
static const uint8_t kI2cPairs[][2] = {
    { 18, 17 }, { 17, 18 },
    { 43, 44 }, { 44, 43 },
    {  8,  9 }, {  9,  8 },
    {  5,  6 }, {  6,  5 },
    { 47, 48 }, { 48, 47 },
    {  1,  2 }, {  2,  1 },
    { 10, 11 }, { 11, 10 },
};
static const int kI2cN = (int)(sizeof(kI2cPairs) / sizeof(kI2cPairs[0]));

// SI473x 的两个可能地址（由 SEN 脚电平决定）
static const uint8_t kSiAddr[2] = { 0x11, 0x63 };

// 一对脚上最多允许几个应答 —— 超过就是浮空总线
#define I2C_MAX_REAL_DEVICES 5

// ------------------------------------------------------------
//  阶段 3 用：最小 ST7789 驱动（引脚运行时可变，不经过 TFT_eSPI）
//
//  为什么不用 TFT_eSPI：它的引脚全是编译期宏，运行时改不了，扫不了候选。
//  这里只做「能出整屏颜色」这一件事 —— 判据就是屏有没有变红/变绿，
//  不需要字库、不需要旋转、不需要中文，可靠性最高。
// ------------------------------------------------------------
static uint8_t s_scrCs = 10, s_scrDc = 13;
static int8_t  s_scrRst = 14;

static void scrCmd(uint8_t c) {
  digitalWrite(s_scrDc, LOW);
  if (s_scrCs >= 0) digitalWrite(s_scrCs, LOW);
  SPI.transfer(c);
  if (s_scrCs >= 0) digitalWrite(s_scrCs, HIGH);
}

static void scrData(const uint8_t* d, int n) {
  digitalWrite(s_scrDc, HIGH);
  if (s_scrCs >= 0) digitalWrite(s_scrCs, LOW);
  for (int i = 0; i < n; i++) SPI.transfer(d[i]);
  if (s_scrCs >= 0) digitalWrite(s_scrCs, HIGH);
}

static void scrCmd1(uint8_t c, uint8_t v) {
  scrCmd(c);
  scrData(&v, 1);
}

// 整屏填一个 16 位色（RGB565）。屏是 170x320（ST7789 竖装）。
static void scrFill(uint16_t color) {
  // CASET 0..169
  scrCmd(0x2A);
  { uint8_t b[4] = { 0x00, 0x00, 0x00, 0xA9 }; scrData(b, 4); }
  // RASET 0..319
  scrCmd(0x2B);
  { uint8_t b[4] = { 0x00, 0x00, 0x01, 0x3F }; scrData(b, 4); }
  // RAMWR + 推像素
  scrCmd(0x2C);
  digitalWrite(s_scrDc, HIGH);
  if (s_scrCs >= 0) digitalWrite(s_scrCs, LOW);
  const uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)(color & 0xFF);
  for (int i = 0; i < 170 * 320; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  if (s_scrCs >= 0) digitalWrite(s_scrCs, HIGH);
}

// 按一组引脚初始化并点亮；返回 false = 这一组装不上（IO 非法）
static bool scrTry(uint8_t mosi, uint8_t sclk, uint8_t cs, uint8_t dc, int8_t rst) {
  s_scrCs = cs; s_scrDc = dc; s_scrRst = rst;

  if (cs >= 0)  { pinMode(cs, OUTPUT);  digitalWrite(cs, HIGH); }
  pinMode(dc, OUTPUT); digitalWrite(dc, HIGH);
  if (rst >= 0) {
    pinMode(rst, OUTPUT);
    digitalWrite(rst, HIGH); delay(5);
    digitalWrite(rst, LOW);  delay(20);
    digitalWrite(rst, HIGH); delay(120);
  }

  // SCLK / MOSI 交给 SPI 换绑（core 2.0.17 支持运行时改）
  SPI.end();
  delay(2);
  // core 2.0.17 的 SPI.begin() 返回 void（新 core 才回 bool），所以不能判返回值
  SPI.begin(sclk, -1, mosi, -1);
  SPI.setFrequency(27000000);   // 1.29.0：40MHz 对 21pin 长排线偏高，先降到 27MHz
  delay(2);

  scrCmd(0x01); delay(150);          // SWRESET
  scrCmd(0x11); delay(150);          // SLPOUT
  scrCmd1(0x3A, 0x55);               // COLMOD: 16bit
  scrCmd1(0x36, 0x00);               // MADCTL: 竖屏 170x320
  scrCmd(0x21);                      // INVON（这块屏是反显，不开会全是负片）
  scrCmd(0x13);                      // NORON
  scrCmd(0x29); delay(20);           // DISPON
  return true;
}

// 屏幕候选：{MOSI, SCLK, CS, DC, RST}
//   按「证据强度」排：
//     1    我们现在的配置（最可能）
//     2    总线反接
//     3/4  没有独立复位（RST 接 3V3 的板子常见）
//     5/6  DC 与 MOSI / SCLK 互换（抄错引脚的典型症状）
//     7/8  高脚总线
//     9/10 CS 换脚
struct ScrCfg { int8_t mosi, sclk, cs, dc, rst; };
static const ScrCfg kScr[] = {
  // ---- 第一批：沿用 ATS-MINI 原理图的 CS=6 / DC=7 / RST=16 ----
  //   这是最可能的一组：这块板是在 ATS-MINI 上改屏（并口 → SPI），
  //   CS/DC/RST/BL 四个脚很可能原样保留，只换了 MOSI/SCLK。
  //   !! 1.23~1.27.5 的候选里 CS 一直是 10、DC 一直是 13，从没试过 6/7。
  { 11, 12,  6,  7, 16 },   //  1  MOSI11 SCLK12 CS6 DC7 RST16
  { 12, 11,  6,  7, 16 },   //  2  总线反接
  { 11, 12,  6,  7, -1 },   //  3  无独立复位
  { 12, 11,  6,  7, -1 },   //  4
  { 13, 14,  6,  7, 16 },   //  5  MOSI13 SCLK14
  { 14, 13,  6,  7, -1 },   //  6
  { 11, 12,  6, 13, 16 },   //  7  DC 在 13
  { 11, 12, 10,  7, 16 },   //  8  CS 在 10、DC 在 7

  // ---- 第二批：原来那组（CS=10 / DC=13），留作对照 ----
  { 11, 12, 10, 13, 14 },   //  9  工程原配置
  { 12, 11, 10, 13, 14 },   // 10
  { 11, 12, 10, 13, -1 },   // 11
  { 13, 12, 10, 11, 14 },   // 12  DC 与 MOSI 互换

  // ---- 第三批：其它常见接法 ----
  { 13, 14,  6,  7, -1 },   // 13
  { 11, 12, 16,  7, -1 },   // 14  CS 在 16
  {  5,  6,  7,  8, -1 },   // 15  低脚一组
  { 11, 12,  5,  6, -1 },   // 16  CS/DC 用 5/6
};
static const int kScrN = (int)(sizeof(kScr) / sizeof(kScr[0]));

// ============================================================
//  阶段 3（1.31.0 全新）：**读屏 ID** 来找 SPI 引脚 —— 全自动，不用看屏幕
//
//  为什么这次能确定：
//    ST7789 支持 RDDID (0x04) 命令，会回 4 个字节（典型 0x00 0x52 0x78 0x29）。
//    21pin 模组的 SDA 脚是**双向**的（datasheet: "SPI interface input/output pin"），
//    所以完全可以用软件 SPI：写的时候 SDA=OUT，读的时候 SDA=IN。
//
//    于是判据变成：把 {SCLK, SDA, DC, CS} 的所有排列都试一遍，
//    **哪一组的回读不是 0x00/0xFF，那一组就是真的引脚** —— 不需要人眼、不需要
//    背光、不需要知道屏在显示什么。这是唯一一个「硬件不配合也能自证」的判据。
//
//    候选池已排除全部已知功能脚：
//      0=BOOT 1/2=编码器 4=电池ADC 15=电源自锁 16=SI4732复位 17/18=I2C
//      19/20=USB 21=旋钮键 26~37=flash/PSRAM 43/44=UART0 45/46=strapping
//      22~25 在 S3 上不存在
// ============================================================
static const uint8_t kCand[] = {
  3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 38, 39, 40, 41, 42, 47, 48
};
static const int kCandN = (int)(sizeof(kCand) / sizeof(kCand[0]));

// 软件 SPI：写一字节（mode 0，先高后低，上升沿采样）
static inline void bbWrite(uint8_t sclk, uint8_t sda, uint8_t v) {
  for (int b = 7; b >= 0; b--) {
    digitalWrite(sda, (v >> b) & 1);
    digitalWrite(sclk, HIGH);
    digitalWrite(sclk, LOW);
  }
}

// 软件 SPI：读一字节（SDA 先切 INPUT）
static inline uint8_t bbRead(uint8_t sclk, uint8_t sda) {
  uint8_t v = 0;
  pinMode(sda, INPUT);
  for (int b = 0; b < 8; b++) {
    digitalWrite(sclk, HIGH);
    v = (uint8_t)((v << 1) | (digitalRead(sda) & 1));
    digitalWrite(sclk, LOW);
  }
  return v;
}

// 用一组引脚试读 ST7789 的 ID。返回 true = 读到可信 ID
static bool tryReadId(uint8_t sclk, uint8_t sda, uint8_t dc, uint8_t cs,
                      uint8_t out[4]) {
  pinMode(cs, OUTPUT);   digitalWrite(cs, HIGH);
  pinMode(sclk, OUTPUT); digitalWrite(sclk, LOW);
  pinMode(dc, OUTPUT);   digitalWrite(dc, LOW);
  pinMode(sda, OUTPUT);  digitalWrite(sda, HIGH);
  delayMicroseconds(20);

  digitalWrite(cs, LOW);
  digitalWrite(dc, LOW);            // 命令
  bbWrite(sclk, sda, 0x04);         // RDDID
  digitalWrite(dc, HIGH);           // 数据
  // RDDID 前有一个 dummy 字节，然后才是 ID
  out[0] = bbRead(sclk, sda);
  out[1] = bbRead(sclk, sda);
  out[2] = bbRead(sclk, sda);
  out[3] = bbRead(sclk, sda);
  digitalWrite(cs, HIGH);

  // 全部释放成高阻，别干扰后面的组合
  pinMode(sda, INPUT); pinMode(sclk, INPUT); pinMode(dc, INPUT); pinMode(cs, INPUT);

  // 判据：不能是「全 0」「全 FF」，也不能 4 个字节完全一样（那是浮空线）
  bool allZ = (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
  bool allF = (out[0] == 0xFF && out[1] == 0xFF && out[2] == 0xFF && out[3] == 0xFF);
  bool same = (out[0] == out[1] && out[1] == out[2] && out[2] == out[3]);
  return !(allZ || allF || same);
}

static void scanPanelPins() {
  Serial.println();
  Serial.println("=== 阶段 3/4  读屏 ID 找 SPI 引脚（全自动，不用看屏）===");
  Serial.printf("    候选 %d 个脚，穷举 {SCLK,SDA,DC,CS} 四元组，共 %d 组\n",
                kCandN, kCandN * (kCandN - 1) * (kCandN - 2) * (kCandN - 3));
  Serial.println("    判据：RDDID 回读不是全 00 / 全 FF / 四字节相同 → 就是这组");

  int hits = 0;
  unsigned long total = 0;

  for (int a = 0; a < kCandN; a++) {
    for (int b = 0; b < kCandN; b++) {
      if (b == a) continue;
      for (int c = 0; c < kCandN; c++) {
        if (c == a || c == b) continue;
        for (int d = 0; d < kCandN; d++) {
          if (d == a || d == b || d == c) continue;
          total++;
          uint8_t r[4] = {0, 0, 0, 0};
          if (tryReadId(kCand[a], kCand[b], kCand[c], kCand[d], r)) {
            hits++;
            Serial.printf("    ★ 命中 SCLK=%d SDA=%d DC=%d CS=%d  →  ID %02X %02X %02X %02X\n",
                          kCand[a], kCand[b], kCand[c], kCand[d],
                          r[0], r[1], r[2], r[3]);
            if (hits >= 12) {
              Serial.println("    （命中够多了，停）");
              goto done;
            }
          }
        }
      }
    }
  }
done:
  Serial.printf("    试了 %lu 组，命中 %d 组\n", total, hits);
  if (hits == 0) {
    Serial.println("    !! 一组都没命中 —— 说明屏不是 4 线 SPI 模式，");
    Serial.println("       或者 IM1/IM2 被硬件接成了并口(8080)，或者 RDDID 被禁用。");
  }
  Serial.println();
}

// ------------------------------------------------------------
//  阶段 4：I2S 候选 {BCK, WS(LCK), DIN}
// ------------------------------------------------------------
static const uint8_t kPinSets[][3] = {
    {  5,  6,  7 },   //  1
    {  6,  7,  8 },   //  2
    {  7,  8,  9 },   //  3
    {  8,  9,  5 },   //  4
    { 16, 17, 18 },   //  5
    { 17, 18, 16 },   //  6
    { 18, 16, 17 },   //  7
    { 39, 40, 41 },   //  8 本工程原来填的（已被反汇编证伪，留作对照）
    { 40, 41, 42 },   //  9
    { 41, 42, 40 },   // 10
    { 42, 39, 40 },   // 11
    { 47, 48, 39 },   // 12
};
static const int kPinSetN = (int)(sizeof(kPinSets) / sizeof(kPinSets[0]));

// ------------------------------------------------------------
//  正弦波
// ------------------------------------------------------------
#define TONE_RATE   44100
#define TONE_AMP    7000

static int s_phase = 0;

static void fillTone(int16_t* buf, int frames) {
  for (int i = 0; i < frames; i++) {
    const int16_t v = (int16_t)(TONE_AMP * sinf(2.0f * (float)M_PI * s_phase / TONE_RATE));
    buf[i * 2]     = v;
    buf[i * 2 + 1] = v;
    s_phase++;
    if (s_phase >= TONE_RATE) s_phase -= TONE_RATE;
  }
}

// 驱动只装一次。装不上就换另一个端口试。
//   为什么不能每组 install/uninstall：core 2.0.17 上反复装会把驱动留在
//   "半装"状态，后面一律返回 0x103(ESP_ERR_INVALID_STATE)，24 组全废。
//   实测过一次，就是这么全红的。
static i2s_port_t s_port   = I2S_NUM_0;
static bool       s_i2sOk  = false;

static bool i2sEnsureDriver() {
  if (s_i2sOk) return true;

  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = TONE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = 6;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  for (int p = 0; p < 2; p++) {
    const i2s_port_t port = (p == 0) ? I2S_NUM_0 : I2S_NUM_1;
    esp_err_t e = i2s_driver_install(port, &cfg, 0, nullptr);
    if (e == ESP_OK) {
      s_port  = port;
      s_i2sOk = true;
      Serial.printf("   [i2s] 驱动装在 I2S_NUM_%d\n", (int)p);
      return true;
    }
    Serial.printf("   [i2s] I2S_NUM_%d install 失败 0x%X\n", (int)p, e);
  }
  return false;
}

static bool playTone(int bck, int ws, int dout, int ms) {
  if (!i2sEnsureDriver()) return false;

  // 只换引脚，不重装驱动
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = bck;
  pins.ws_io_num    = ws;
  pins.data_out_num = dout;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  esp_err_t e = i2s_set_pin(s_port, &pins);
  if (e != ESP_OK) {
    Serial.printf("   [i2s] set_pin 失败 0x%X（这对脚装不上）\n", e);
    return false;
  }
  i2s_zero_dma_buffer(s_port);

  static int16_t buf[256 * 2];
  const uint32_t t0 = millis();
  size_t written = 0;
  while (millis() - t0 < (uint32_t)ms) {
    fillTone(buf, 256);
    i2s_write(s_port, buf, sizeof(buf), &written, pdMS_TO_TICKS(200));
  }
  return true;
}

// ------------------------------------------------------------
//  扫一对脚：返回应答的地址个数，把命中的地址填进 found[]
// ------------------------------------------------------------
static int i2cScanPair(uint8_t sda, uint8_t scl, uint8_t* found, int cap) {
  Wire.end();
  delay(5);
  if (!Wire.begin(sda, scl, 100000)) {
    Wire.end();
    return -1;                      // 这对脚装不上（IO 非法或被占）
  }
  delay(5);

  int n = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (n < cap) found[n] = a;
      n++;
    }
  }
  Wire.end();
  delay(2);
  return n;
}

// ------------------------------------------------------------
//  对指定地址发 GET_CHIP_ID(0x01,0x00)，回应 9 字节。
//  首字节 bit7 = CTS，回 CTS 说明这是真芯片而不是浮空应答。
// ------------------------------------------------------------
static bool siChipId(uint8_t sda, uint8_t scl, uint8_t addr, bool verbose) {
  Wire.end();
  delay(5);
  if (!Wire.begin(sda, scl, 100000)) { Wire.end(); return false; }
  delay(5);

  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0x01);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) { Wire.end(); return false; }
  delay(40);

  uint8_t n = Wire.requestFrom(addr, (uint8_t)9);
  uint8_t r[9] = {0};
  for (uint8_t i = 0; i < n && i < 9; i++) r[i] = Wire.read();
  Wire.end();
  delay(2);

  if (verbose) {
    Serial.printf("      addr 0x%02X 回 %d 字节: ", addr, n);
    for (uint8_t i = 0; i < n; i++) Serial.printf("%02X ", r[i]);
    Serial.println();
  }
  if (n < 2) return false;
  if ((r[0] & 0x80) == 0) return false;            // 没有 CTS = 不是真芯片

  if (verbose && n >= 7) {
    Serial.printf("      ★ 芯片活着：part=0x%02X%02X  fw=%d.%d  comp=%d.%d\n",
                  r[1], r[2], r[3], r[4], r[5], r[6]);
  }
  return true;
}

// ------------------------------------------------------------
//  阶段 0：GPIO 指纹（全自动，不依赖眼睛、不依赖数闪几下）
//
//  为什么加这个：前面十几轮都在"扫引脚 + 靠人眼看闪几下"，反复被假象骗
//  （IO47 高/低两档都"亮"，物理上不可能是背光灯）。问题在于：
//  **我们一直在猜哪些脚接在屏上。** 这一阶段改用电气特性直接观察。
//
//  每个 IO 做四件事：
//    ① 高阻读（io）—— 空脚通常漂成 1
//    ② 内部上拉读（pu）—— 外面有下拉电阻、或被外部拉低 → 读到 0
//    ③ 内部下拉读（pd）—— 外面有上拉电阻（LCD 模块的 CS/DC/RST 常带 10k 上拉）→ 读到 1
//    ④ 从"驱动低"切到"开上拉"，数多久变高（rt）—— 走线越长/容性负载越大，越慢
//  三项叠加就能把"接了线的脚"和"空脚"分开，全程不需要人参与。
static void gpioFingerprint() {
  // 这些脚绝不能驱动：0=BOOT 键  15=整板电源自锁  16=SI4732 复位  45/46=strapping
  const int noDrive[] = { 0, 15, 16, 45, 46 };
  const int noDriveN = sizeof(noDrive) / sizeof(noDrive[0]);

  Serial.println("=== 阶段 0  GPIO 指纹（全自动，不需要你参与）===");
  Serial.println("    列：io=高阻读  pu=开上拉读  pd=开下拉读  rt=上升时间(us)");
  Serial.println("    ★ 判读：pu=0 或 pd=1 说明**外面接了东西**（上拉/下拉）；rt 大 = 长走线/大负载");
  Serial.println("    ★ 空脚一般是 io=1 pu=1 pd=0 rt 很小");
  Serial.println();
  Serial.println("  IO  io pu pd  rt    判读");

  for (int p = 0; p <= 48; p++) {
    if (p == 19 || p == 20) continue;                 // USB D-/D+
    if (p >= 26 && p <= 37) continue;                 // flash / PSRAM，绝对不能碰
    if (p == 43 || p == 44) continue;                 // UART0 TXD/RXD

    bool canDrive = true;
    for (int k = 0; k < noDriveN; k++) if (noDrive[k] == p) canDrive = false;

    pinMode(p, INPUT);
    delayMicroseconds(80);
    int r0 = digitalRead(p);

    pinMode(p, INPUT_PULLUP);
    delayMicroseconds(400);
    int r1 = digitalRead(p);

    pinMode(p, INPUT_PULLDOWN);
    delayMicroseconds(400);
    int r2 = digitalRead(p);

    unsigned dt = 999;
    if (canDrive) {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);
      delayMicroseconds(500);
      pinMode(p, INPUT_PULLUP);
      unsigned long t0 = micros();
      dt = 300;
      while (micros() - t0 < 300) {
        if (digitalRead(p)) { dt = (unsigned)(micros() - t0); break; }
      }
    }
    pinMode(p, INPUT);                                 // 立刻回高阻，别干扰后面的阶段

    const char* note = "";
    if (r1 == 0 && r2 == 0) note = "外接下拉 / 被别的东西拉低";
    else if (r1 == 1 && r2 == 1) note = "★外接上拉（模块上常是 CS/DC/RST）";
    else if (r2 == 1) note = "偏上拉（弱）";
    else if (r1 == 0) note = "偏下拉（弱）";

    char dtbuf[8];
    if (canDrive) snprintf(dtbuf, sizeof(dtbuf), "%u", dt);
    else          snprintf(dtbuf, sizeof(dtbuf), "-");
    Serial.printf("  %-3d %d  %d  %d  %-5s %s\n", p, r0, r1, r2, dtbuf, note);
  }
  Serial.println();
  Serial.println("  （这张表整段发我，我据此定屏幕引脚，不用你再数闪几下）");
  Serial.println();
}

// ------------------------------------------------------------
void selftestRun() {
  Serial.println();
  Serial.println("################ 硬件自检 1.22.0 ################");
  Serial.printf("固件 %s %s\n", FW_NAME, FW_VER);
  Serial.println();
  Serial.println("--- 工程里现在写的引脚（= 待验证的假设）---");
  Serial.println("屏幕 SPI : CS=10 DC=13 RST=14 BL=38 MOSI=11 SCLK=12");
  Serial.printf ("SI4732   : SDA=%d SCL=%d RST=%d EN=%d MUTE=%d addr=0x%02X\n",
                 PIN_FM_SDA, PIN_FM_SCL, PIN_FM_RST, PIN_FM_EN,
                 PIN_FM_AMP_MUTE, FM_I2C_ADDR);
  Serial.printf ("电池 ADC : IO%d（BATT_DIV=%.2f，原厂 batfactor=1.71）\n",
                 PIN_BATT_ADC, (double)BATT_DIV);
  Serial.printf ("编码器   : A=%d B=%d SW=%d\n", PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
  Serial.println();

  // 电源自锁 + 收音芯片复位必须先放开
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);                    // 整板外设电源自锁
  pinMode(16, OUTPUT);
  digitalWrite(16, HIGH);                    // SI4732 复位释放（高=工作）

  // ---- 阶段 0：GPIO 指纹（先跑，全程自动）----
  gpioFingerprint();

  // ---- 阶段 1：背光全开（不再数闪几下）----
  Serial.println("=== 阶段 1  背光全开：所有候选脚一起拉高 ===");
  for (int i = 0; i < kBlN; i++) {
    pinMode(kBlPins[i], OUTPUT);
    digitalWrite(kBlPins[i], HIGH);
  }
  Serial.print("    已拉高：");
  for (int i = 0; i < kBlN; i++) Serial.printf("IO%d ", kBlPins[i]);
  Serial.println();
  Serial.println("    此时屏若「整片发白/发灰」= 背光已通（面板未初始化时就是一片亮灰）");
  Serial.println();
  delay(1500);

  // ---- 阶段 2：I2C ----
  Serial.println("=== 阶段 2/4  I2C 找 SI4732（不需要你参与）===");
  int    hitPair = -1;
  uint8_t hitAddr = 0;
  for (int i = 0; i < kI2cN; i++) {
    const uint8_t sda = kI2cPairs[i][0], scl = kI2cPairs[i][1];
    Serial.printf("    [I2C %d/%d] SDA=%d SCL=%d ...\n", i + 1, kI2cN, sda, scl);

    uint8_t found[32];
    const int n = i2cScanPair(sda, scl, found, 32);
    if (n < 0) { Serial.println("      这对脚装不上（IO 不可用），跳过"); continue; }
    if (n == 0) { Serial.println("      无应答"); continue; }

    // ① 假总线过滤：一堆地址全应答 = 浮空输入脚，不是真总线
    if (n > I2C_MAX_REAL_DEVICES) {
      Serial.printf("      !! %d 个地址全应答 → 判为浮空/假总线，跳过\n", n);
      continue;
    }

    Serial.printf("      应答 %d 个: ", n);
    for (int k = 0; k < n && k < 32; k++) Serial.printf("0x%02X ", found[k]);
    Serial.println();

    // ② 对两个可能的 SI473x 地址都发 GET_CHIP_ID 确认
    for (int t = 0; t < 2; t++) {
      const uint8_t a = kSiAddr[t];
      bool listed = false;
      for (int k = 0; k < n && k < 32; k++) if (found[k] == a) listed = true;
      if (!listed) continue;
      if (siChipId(sda, scl, a, true)) {
        hitPair = i;
        hitAddr = a;
        Serial.printf("    ★★★ 确认 SI473x：SDA=%d SCL=%d addr=0x%02X\n", sda, scl, a);
        break;
      } else {
        Serial.printf("      0x%02X 有应答但 GET_CHIP_ID 不是 CTS → 不是 SI473x（或是别的东西）\n", a);
      }
    }
    if (hitPair >= 0) break;
  }

  if (hitPair < 0) {
    Serial.println("    !! 14 对候选都没扫到确认的 SI473x。可能：芯片没供电 / 复位脚不在 16 /");
    Serial.println("       实际接法不在这 14 对里。收音功能先禁用，网络电台不受影响。");
  }
  Serial.println();

  // ---- 阶段 3：读屏 ID 找 SPI 引脚（全自动）----
  scanPanelPins();

  // ---- 阶段 4：I2S ----
  Serial.println("=== 阶段 4/4  I2S（每组 4 秒 1kHz 正弦）===");
  Serial.println("    听到连续的「嘟——」，记下组号。");
  Serial.println();
  for (int round = 1; round <= 2; round++) {
    for (int i = 0; i < kPinSetN; i++) {
      const int bck = kPinSets[i][0], ws = kPinSets[i][1], dout = kPinSets[i][2];
      Serial.printf("[hw] 第 %d 轮  组 %d/%d : BCK=%d  WS=%d  DIN=%d\n",
                    round, i + 1, kPinSetN, bck, ws, dout);
      char line[40];
      snprintf(line, sizeof(line), "I2S %d/%d", i + 1, kPinSetN);
      uiSplash("硬件自检", line);
      playTone(bck, ws, dout, 4000);
      delay(500);
    }
    Serial.println("[hw] —— 一轮结束 ——");
  }

  // 自检完把 I2S 驱动卸掉，免得占着端口影响后面 audioI2S 起播
  if (s_i2sOk) { i2s_driver_uninstall(s_port); s_i2sOk = false; }

  // ---- 收尾 ----
  Serial.println();
  Serial.println("################ 自检完成 ################");
  if (hitPair >= 0) {
    Serial.printf("I2C : SDA=%d SCL=%d addr=0x%02X（已确认，含芯片 ID 应答）\n",
                  kI2cPairs[hitPair][0], kI2cPairs[hitPair][1], hitAddr);
    if (hitAddr != 0x11)
      Serial.printf("      !! 注意：地址是 0x%02X 而不是 0x11 —— SEN 脚被拉高了，\n"
                    "         固件里的 FM_I2C_ADDR 要改成 0x%02X（或运行时两个都试）。\n",
                    hitAddr, hitAddr);
  } else {
    Serial.println("I2C : 未找到");
  }
  Serial.println("背光: 记下第几个 IO 之后屏有微光");
  Serial.println("屏幕: 记下哪一组之后屏出现颜色（红/绿）");
  Serial.println("I2S : 记下哪一组 / 第几轮有「嘟」");
  Serial.println();
  Serial.println("把这三行告诉我，我改 config.h 出正式版。");
  // 1.26.1：跑完不是进主固件，而是**自动从头再来**。
  //   理由：阶段 1 只跑一次，用户一分神错过就没了，只能重刷固件 —— 太浪费时间。
  //   自检固件（-DHWTEST=1）本来就是拿来反复看的，所以让它一直循环，
  //   什么时候看都能等下一轮。要退出就刷正式固件。
  Serial.println("[hw] 10 秒后重新开始自检（本固件会一直循环，直到你刷正式固件）");
  Serial.println();
  delay(10000);
  ESP.restart();
}

#endif  // HWTEST
