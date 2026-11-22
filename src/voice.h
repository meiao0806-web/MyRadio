// ============================================================
//  voice.h —— 语音播报（1.19.0）
//
//  这台机器只说三句话，而且都是"要紧的时候"：
//    电量低于 15%  →「电量低，请充电」
//    电量耗尽     →「没电了，关机」
//    开始自动搜台 →「正在搜台，请稍等」   （1.40.1 加）
//
//  为什么搜台也要说话：搜台要十几秒，屏上虽然写着「正在搜台 45%」，
//  但老人转旋钮时眼睛是看着旋钮的。给一声提示，他就知道"机器在干活、不是坏了"，
//  不会中途去拧旋钮或者直接关机。
//
//  为什么把 mp3 编进固件：
//    这两句必须**离线**能说（没电的时候很可能正好没网）。所以句子是提前用
//    edge-tts 合成好的（tools/gen_voice.py），字节数组在 src/voice_data.h 里，
//    首次开机 voiceBegin() 把它们写进 LittleFS，播放走 Audio 库的本地文件通路：
//
//        battery.cpp → voicePlay(VO_LOW) → audio.cpp 的任务 → connecttoFS(LittleFS)
//
//  和节目的关系：
//    播报会打断当前节目，念完自动接回来（有声书还是同一集，位置从头开始 ——
//    这点只能这样，音频库不做断点续传）。
//    收音（FM）模式下也照样播：FM 的声音是从芯片走 3.5mm 进功放，I2S 是另一路，
//    在功放输入端自然叠在一起，听着就是"广播里有人在提醒你"，不冲突。
// ============================================================
#pragma once
#include <Arduino.h>

// !! 顺序必须和 tools/gen_voice.py 的 CLIPS 一致（0=low 1=off 2=scan）
//    加一段就两边一起加：CLIPS 里加一条 → 跑 --tts → 这里加一个枚举值。
//    只改一边的话，voice.cpp 的 kPath/kLen/kSrc 三个数组长度会对不上，
//    编译期 char[2] 初始化成 3 个值就报错了（算是好事）。
enum VoiceId : uint8_t {
  VO_LOW  = 0,    // 电量低，请充电
  VO_OFF  = 1,    // 没电了，关机
  VO_SCAN = 2,    // 正在搜台，请稍等
  VOICE_N = 3
};

void voiceBegin();             // setup 里，storeBegin() 之后（LittleFS 已挂上）
bool voicePlay(VoiceId id);    // 请求播报：异步，立刻返回；发不出去返回 false
bool voiceBusy();              // 正在念
bool voiceReady(VoiceId id);   // 这一段可用（文件就位）
const char* voicePath(VoiceId id);
void voiceJson(String& o);     // {"ready":2,"busy":0}  给 App 的诊断用
