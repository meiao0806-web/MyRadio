// ============================================================
//  power.h —— 电源保护 / 电流保护（1.21.0）
//
//  一段话说完这个文件：
//    读芯片温度，分三档往下压「音量 / 亮度」上限，最热就暂停播放；
//    再配合 battery 那边的「已充满」判定，把亮度压一档；
//    如果板子上充电 IC 的使能脚接到了 GPIO，满电后真的停充、掉到 4.05V 再开。
//
//  为什么要它：
//    这台机器是「一直插着 USB 当桌面收音机」的用法。大音量 + 充电 + WiFi
//    同时跑的时候，功放和充电 IC 都在发热，电池跟着烤；长期停在 4.2V 满电
//    又会让锂电加速老化。充电电流固件改不了（外围电阻定死），但能改「自己吃
//    多少电」—— 这就是本模块的全部内容。
//
//  各模块怎么用：
//    · main.cpp   setup 里 powerBegin()，loop 里 powerTick()
//    · audio.cpp  applyVol() 里用 powerVolCapPct() 夹一下音量
//    · ui.cpp     setBright() 前用 powerBrightCap() 夹一下亮度
//    · api.cpp    powerJson() 拼诊断字段（温度 / 档位 / 是否停充）
// ============================================================
#pragma once
#include <Arduino.h>

void powerBegin();        // 有电池功能时在 batteryBegin() 之后调
void powerTick();         // 主循环里调：采样温度、判档、执行停充

// ---- 上限（100 = 不限制）----
// 1.44.0：音量分两套量程（FM 0..63 / 网络 0..21），所以上限只能给**百分比**，
//   由各自那一侧乘自己的 MAX。返回值 100 = 不限制。
int  powerVolCapPct();    // 音量上限百分比：温度档 + 满电档取更狠的那个
int  powerBrightCap();    // 亮度上限

// ---- 状态查询 ----
int  powerTempC();        // 平滑后的芯片温度（℃，-999 = 读不到）
int  powerLevel();        // 降载档位 0=正常 1=轻降 2=重降 3=暂停播放
bool powerHeatPaused();   // 是否因为过热暂停了播放
bool powerChgCut();       // 是否正在「真停充」（只在 PIN_CHG_CTRL >= 0 时可能为真）
bool powerOnExt();        // 是否判定为插着外接电源（battery 那边算出来的）

// ---- 1.21.0 快充检测 ----
int  powerVbusMv();       // USB 输入电压（mV，-1 = 没接分压脚读不到）
bool powerFastChg();      // 是否判为快充档（≥7V，需要接了 PIN_VBUS_ADC 才可能为真）
int  powerChgRateMvpm();  // 充电速率（mV/分钟，5 分钟平均）
bool powerFastRate();     // 充得太猛（速率超过 BATT_FAST_MVPM）

// ---- 给 api.cpp 拼 JSON ----
void powerJson(String& o);
