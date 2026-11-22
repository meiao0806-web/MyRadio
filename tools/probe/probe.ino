// ============================================================
//  probe.ino —— 屏幕引脚探测器（独立的 Arduino 小程序，不进主工程）
//
//  用途：资料包里没有原理图，不知道屏幕的 SPI 引脚。
//        把本文件用 Arduino IDE 单独烧进去，它会逐个组合试，
//        屏幕亮起纯色时，把串口打印的编号记下来。
//
//  用法：
//    1. Arduino IDE 选 ESP32S3 Dev Module，16MB Flash
//    2. 烧录本程序
//    3. 串口 115200，看提示；屏幕每 1.5 秒换一个组合
//    4. 哪个编号能让屏幕整屏变红/绿/蓝，就把它填进 src/config.h
//       对应的 -DTFT_MOSI / -DTFT_SCLK / -DTFT_CS / -DTFT_DC / -DTFT_RST
//
//  注意：CS/DC/RST 猜错不会烧硬件，只是没反应。
// ============================================================
#include <TFT_eSPI.h>   // 需要在 Arduino 库里装好；本程序用软 SPI 方式逐个试

// 候选组合：常见 ESP32-S3 320x170 板（含 T-Display S3 / 各类 DIY 板）
struct Combo { int mosi, sclk, cs, dc, rst, bl; };
static const Combo kCombos[] = {
  { 11, 12, 10, 13, 14, 38 },
  { 11, 12, 10,  9, 14, 38 },
  {  3,  5,  6,  7, 15, 38 },
  {  5,  6,  7,  4,  8, 38 },
  { 41, 42, 40, 39, 38, 37 },
  { 35, 36, 34, 33, 47, 48 },
  { 15, 16, 17, 18, 21, 38 },
  { 45, 46, 44, 43, 42, 41 },
};
static const int kCombosN = sizeof(kCombos) / sizeof(kCombos[0]);

// 用最原始的位操作点屏：只画满屏色块，验证连线对不对
static inline void wcmd(int cs, int dc, uint8_t c) {
  digitalWrite(dc, LOW); digitalWrite(cs, LOW);
  shiftOut(kCombos[0].mosi, kCombos[0].sclk, MSBFIRST, c);
  digitalWrite(cs, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== 屏幕引脚探测 ===");
  Serial.printf("共 %d 组候选，每组 1.5 秒，看屏幕有没有整屏变色\n", kCombosN);

  for (int i = 0; i < kCombosN; i++) {
    const Combo& c = kCombos[i];
    Serial.printf("[%d] MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d\n",
                  i, c.mosi, c.sclk, c.cs, c.dc, c.rst, c.bl);

    pinMode(c.cs, OUTPUT); pinMode(c.dc, OUTPUT); pinMode(c.rst, OUTPUT);
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
    digitalWrite(c.cs, HIGH);

    // 硬复位
    digitalWrite(c.rst, LOW);  delay(20);
    digitalWrite(c.rst, HIGH); delay(120);

    // ST7789 初始化（最小集）
    digitalWrite(c.cs, LOW); digitalWrite(c.dc, LOW);
    uint8_t init[][2] = {
      {0x01, 0}, {0x11, 0}, {0x3A, 1}, {0x36, 1}, {0x21, 0},
      {0x13, 0}, {0x29, 0}, {0x00, 0}
    };
    for (int k = 0; k < 8; k++) {
      digitalWrite(c.dc, LOW);
      shiftOut(c.mosi, c.sclk, MSBFIRST, init[k][0]);
      if (init[k][1]) {
        digitalWrite(c.dc, HIGH);
        shiftOut(c.mosi, c.sclk, MSBFIRST, init[k][0] == 0x3A ? 0x55 : 0x00);
      }
      if (init[k][0] == 0x11) delay(120);
    }
    digitalWrite(c.cs, HIGH);

    // 刷满屏：红 → 绿 → 蓝
    const uint8_t colors[3][2] = { {0xF8, 0x00}, {0x07, 0xE0}, {0x00, 0x1F} };
    for (int ci = 0; ci < 3; ci++) {
      digitalWrite(c.cs, LOW); digitalWrite(c.dc, LOW);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x2A);      // 列地址
      digitalWrite(c.dc, HIGH);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0xCF);

      digitalWrite(c.dc, LOW);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x2B);      // 行地址
      digitalWrite(c.dc, HIGH);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x00);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0xA9);

      digitalWrite(c.dc, LOW);
      shiftOut(c.mosi, c.sclk, MSBFIRST, 0x2C);      // 写显存
      digitalWrite(c.dc, HIGH);
      for (uint32_t p = 0; p < 320UL * 170UL; p++) {
        shiftOut(c.mosi, c.sclk, MSBFIRST, colors[ci][0]);
        shiftOut(c.mosi, c.sclk, MSBFIRST, colors[ci][1]);
      }
      digitalWrite(c.cs, HIGH);
      delay(250);
    }
    Serial.printf("[%d] 试完\n", i);
    delay(600);
  }
  Serial.println("=== 结束：屏幕从头到尾没亮 → 检查供电/背光/排线 ===");
}

void loop() { delay(1000); }
