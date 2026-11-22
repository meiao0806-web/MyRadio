// ============================================================
//  health.h —— 每台一份「体检档案」（1.17.0）
//
//  要解决的事：台表里 290 个台，哪些是真的能听？
//  屏上永远看着「播放中」，可有的台其实：
//    · 地址死了（连 20 秒都没解码出东西）
//    · 连上了、数据也在流，但解出来全是静音（蜻蜓 m4a 被标成 mp3 那种）
//  这两种都得让 App 能一眼看见，所以固件这边做三件事：
//
//   1. 实时电平：从解码后的 PCM 里取峰值（音量之前），
//      所以静音/音量 0 也照样能判断「这台到底有没有声音」。
//   2. 每次真播一台就判一次：有声 = 正常；连不上 = 坏；
//      有数据 12 秒不吭声 = 可疑。结果写进档案。
//   3. 档案按**地址指纹**存（不按台序号），所以 App 里拖排序、
//      增删台都不会把「这台坏过」记到别人头上。落盘 /health.bin。
//
//  档案只记「测过的」，没测过的是 HL_UNK —— App 那头显示成灰色。
// ============================================================
#pragma once
#include <Arduino.h>

enum HlState : uint8_t {
  HL_UNK = 0,   // 没测过
  HL_OK  = 1,   // 出过声
  HL_BAD = 2,   // 连不上 / 老掉线
  HL_DIM = 3    // 有数据流，但一直没声音
};

void healthBegin();     // setup 里、storeLoad() 之后
void healthTick();      // loop 里调：合并写盘（8 秒）

// ---- 由 audio.cpp 在播放任务里调 ----
void    healthStart(int idx);                     // 开始试某一台
void    healthHold(bool on);                      // 暂停期间别判「没声音」
void    healthStop();                             // 停了（不在播了）
void    healthMeta();                             // 收到元数据（流活着）
void    healthSamples(const int16_t* buf, uint16_t n);  // 解码出的 PCM：累峰值
uint8_t healthTakePeak();                         // 取走并清空峰值（0..100）
void    healthLevel(uint8_t pk);                  // 每毫秒一次：平滑电平 + 判定
void    healthOk();                               // 判定：真的出声了
void    healthFail(const char* why);              // 判定：连不上 / 老掉线
void    healthSilent();                           // 判定：有数据没声音

// ---- 给 api.cpp / net.cpp 拼 JSON ----
uint8_t     healthState(int idx);                 // HlState
// 1.46.6：台地址 → 指纹。停播台账（offair.cpp）必须用**同一份**哈希，
//   两边各写一份 FNV 迟早会漂（换种子/换字段就再也对不上）。
uint32_t    healthKeyOf(int idx);
uint8_t     healthPeak();                         // 当前电平 0..100（实时）
int         healthSession();                      // 正在试的台号，-1 = 没在试
// one < 0：整表摘要（每台状态 + 失败次数 + 计数 + 当前会话）
// one >= 0：单台详情（App 的问题详情弹层用）
void        healthJson(String& o, int one = -1);
