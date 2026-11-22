// ============================================================
//  fm.h —— Si4732 全波段收音芯片（I2C 控制）
//
//  1.10.0 起不再只是 FM：一个「kHz 频率」就能走完
//     长波 153~279 → 中波 531~1602 → 调频 64.0~108.0MHz → 短波 2300~26100 kHz →绕回长波
//  芯片内部要在 AM(FUNC=1) / FM(FUNC=0) 两种模式之间切，由 fm.cpp 自己管，
//  外面（旋钮/界面/接口）只看见一个频率，**不需要任何「切波段」操作**。
//
//  AM/SW/LW 不用灌 patch：SI4732-A10 与 SI4735-D60 的 AM_SW_LW 固件同源
//  （AN332 第 2 页 Table 1），只有 SSB 才要 patch。
//
//  音频不走 ESP32：模块耳机口 → 3.5mm 线 → 功放 LINE IN → 喇叭，
//  ESP32 只通过 I2C 告诉芯片「调到多少、音量多少、静不静音」。
// ============================================================
#pragma once
#include <Arduino.h>

// 初始化 I2C 并把芯片拉起来。失败返回 false（没接模块 / 接线错 / 地址不对），
// 调用方据此决定要不要启用收音 —— 失败时整个收音功能静默关闭，网络功能不受影响。
bool fmBegin();

bool fmReady();

// 设定目标频率（kHz）。153..279 长波 / 531..1602 中波 / 64000..108000 调频 /
// 2300..26100 短波；落在中间的空白频段会自动吸到最近的波段边。
// 只记目标、不动 I2C —— 转旋钮时连续调用不会卡；fmLoop() 在停手 160ms 后才真调台。
void fmTune(int khz);

// 当前目标频率（kHz）。屏幕/接口显示的就是这个，转手就变。
int  fmFreq();

// 信号强度 0..100（由芯片 RSSI 换算，用于顶栏信号柱和 App）
int  fmRssi();

// 搜台：dir=+1 向上、-1 向下；找不到就停在原位
void fmSeek(int dir);

// 音量 0..100（内部映射到芯片的 0..63 档）
void fmSetVolume(int v);

void fmMute(bool on);

// 主循环里偶尔调一次：追目标频率（停手 160ms 后真调台）+ 刷新信号缓存 + 判静噪
void fmLoop();

// ---- 1.22.0：静噪 ----
// true 表示「当前因为没信号被静噪压着」（不是用户按了静音）。
// 界面可以据此显示「无信号」，而不是显示「已静音」。
bool fmSquelched();
// 最近一次读到的芯片原始 RSSI / SNR（0~127），诊断页和静噪门限都用它
int  fmRawRssi();
int  fmRawSnr();

// 串口自检：扫描 I2C 总线、打印 POWER_UP 应答、调一次台看 RSSI。
void fmDiag();

// 1.46.39：主动探一次芯片，把「现在的真实状态」读出来（不换台、不动音量、不出声）。
//   排查「收音没声音」全靠它：读不到 = I2C 挂了；RSSI 低 = 没信号；
//   HARD_MUTE=3 = 被静音（静噪 / fmMute 没解开）；VOL=0 = 音量为零。
//   任何一个指针传 nullptr 都行，只读关心的那几项。
void fmProbe(int* khz, int* rssi, int* snr, int* muteReg, int* volReg, int* ready);

// ---------------- 波段环（1.10.0）----------------
// 环的顺序就是「旋钮顺时针」的顺序：长波 → 中波 → 调频 → 短波 → 长波。
// 中波/调频相邻是有意的：断网后最常听的就是这两个，一下就到，不用跨过 24MHz 的短波。
enum : uint8_t { RADIO_LW = 0, RADIO_MW = 1, RADIO_FM = 2, RADIO_SW = 3, RADIO_BANDS = 4 };

int  radioBand(int khz);          // 这个频率属于哪个波段（空档 → 最近的波段）
int  radioBandLo(int band);       // 波段下边界（kHz）
int  radioBandHi(int band);       // 波段上边界（kHz）
int  radioBandStep(int band);     // 这个波段「一个频道」多宽（kHz）
const char* radioBandCode(int khz);   // "LW" / "MW" / "FM" / "SW"（接口给手机用）
int  radioSnap(int khz);          // 夹进波段环；落在空档里就吸到最近的边界

// 转速 → 加速倍数：慢转 1（一格一个频道），正常 5，快转 10，甩起来 30。
int  radioGear(int msPerClick, int khz);

// 走 clicks 格（正=往上，负=往下），返回落点频率。
// 跨过波段边界时直接跳到下一个波段的第一格，空白频段不占格子。
int  radioWalk(int khz, int clicks, int gear);

// ============================================================
//  1.38.0：自动搜台（用户：「需要自动搜台把能听的列出来排序」）
//
//  为什么不再让旋钮直接转频率：
//    64~108MHz 有 440 个 0.1MHz 频道，中波还有 108 个 —— 老人转不到头，
//    而且「一会长波一会 FM」正是频率环跨越波段的副作用。
//    搜台之后旋钮只在**搜出来的台**之间走，一格一个，永远走不出可听范围。
//
//  台表按「波段（调频→中波→长波→短波）+ 信号强度降序」排列：
//    同一个波段里，信号强的排前面，也就是「先听到清楚的」。
// ============================================================
#define FM_SCAN_MAX 64
int  fmScanCount();                 // 搜到几个台
int  fmScanFreq(int i);             // 第 i 个台的频率（kHz）
int  fmScanRssi(int i);             // 第 i 个台的信号（0..100，仅用于排序展示）
int  fmScanStep(int fromKhz, int dir);// 在台表里走一格，返回落点频率（没台就返回 fromKhz）
bool fmScanBusy();                  // 正在搜台
int  fmScanProgress();               // 搜台进度 0..100
void fmScanStart();                 // 开始自动搜台（后台跑，不阻塞界面）
// 1.46.37：中断搜台。**退出收音模式时必须调**——
//   否则 s_scanBusy 会一直卡在 true，下次进收音再也搜不了台
//   （用户报的「收频段搜不到，没声音也没噪音」就是这个）。
void fmScanAbort();

// 1.46.42：延后自动搜台。openFm 时 WiFi 还在连，立刻搜会被「网络回来」打断，
//   白搜一次还把台表清空。改成进收音后 delayMs 才搜，fmLoop 里到点执行。
void fmScanWantSoon(uint32_t delayMs);
bool fmScanWanted();
void fmScanLoad();                  // 开机从 LittleFS 读上次的搜索结果
void fmScanSave();

