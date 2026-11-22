// ============================================================
//  net.h —— WiFi / 配网 / REST API（给手机 App 用）
// ============================================================
#pragma once
#include "state.h"

void netBegin();
void netLoop();

bool netWifiOk();
int  netRssi();
void netClockStr(char* buf, int len);
const char* netIP();
const char* netToken();          // 已配对返回令牌，未配对返回 nullptr
bool netNeedSetup();             // 还没配 WiFi
bool netEverUp();                // 本次上电有没有成功连上过 WiFi（决定断网多久切收音）

// 1.46.53：一直连不上 WiFi 时的兜底 —— 自己开热点等人来配网。
//   主循环里每帧调一次。返回 true = 现在该在屏上显示配网提示。
bool     netAutoSetupTick();
// 那条提示还要显示多久（0 = 不显示）
uint32_t netAutoSetupLeftMs();

// ---- 1.43.0：热点手动开关（旋钮长按 3 秒触发）----
bool netApActive();              // 热点现在是不是开着
void netApOn();
void netApOff();
void netApToggle();              // 开↔关，长按用这个

// ---- 1.46.35：延迟断链重连 ----
//   断链会把当前这条 TCP 掐掉，回包就发不出去，App 只能弹「没成功」。
//   所以「重连」这个动作一律先回包，再由 netLoop 在 700ms 后真正断开。
void netReconnectSoon();         // HTTP / MQTT 两条通道都用这个
