// ============================================================
//  audio.h —— 网络流播放（ESP32-audioI2S）
//  原则：Audio 对象只由 audioTask 一个任务碰，别的模块只发请求
// ============================================================
#pragma once
#include "state.h"

void audioBegin();                 // 建任务
void audioPlayIndex(int idx);
void heapAuditDiag(const char* where);   // 1.46.14-diag      // 切台（异步）
void audioNext(int dir);           // dir=+1/-1
void audioSetVol(int v);           // 0..100（异步，自动存盘）
void audioToggleMute();
void audioPauseToggle();
void audioStop();
bool audioIsPlaying();             // 是否真正在出声
// 1.46.29：此刻写 flash 会不会打断声音？
//   写 flash 期间 cache 是停的，而音频任务的代码也在 flash 上 ⇒ 解码停一下 ⇒ I2S 断流。
//   返回 true 表示「本来就没在出声」（没播 / 暂停 / 念播报 / 沙沙 / 连台中）或者 FM 模式
//   （FM 走芯片的模拟输出，跟 I2S 这条链无关），这时候落盘用户听不出来。
bool audioWriteSafe();

// ---- 1.4.0 手机 App「体检 / 修复」用 ----
void        audioReplay();         // 重开当前台（卡住时点一下就恢复）
int         audioRetry();          // 当前这一台已经重连几次
int         audioDeadLinks();      // 开机以来判死刑的地址数
int         audioBitrate();        // kbps，0=还不知道
const char* audioUrl();            // 正在播的真实地址
uint32_t    audioUptimeSec();      // 本台连续播放了多久（0=没在播）
// ---- 1.46.2 有声书断点续播（/api/diag 用）----
int         audioEpNo();           // 正在播第几集（1 基；0 = 不是书）
int         audioPosSec();         // 这一集播到第几秒（含续播起点）
int         audioResumed();        // 1 = 这次是从断点接着播的
// ---- 1.46.10 直播卡顿定位用 ----
int         audioBufBytes();       // 音频库实际输入缓冲字节数（0=还没起来）
int         audioBufPsram();       // 1 = 缓冲在 PSRAM（大），0 = 在内部 RAM（只有 6.4KB）

// ---- 1.19.0 语音播报 ----
// 把一段本地 mp3（LittleFS 里的路径）念出来：先停掉当前流，念完自动接回来。
// 异步，立刻返回；路径为空 / 正在念 都返回 false。
bool audioVoice(const char* path);
bool audioVoiceBusy();             // 正在念（用来判断"念完了没有"）

// ---- 1.21.0 电源保护 ----
// 音量上限被温度/满电状态压过之后，让音频任务立刻按新上限重设一次。
// 不碰用户设的 A.vol —— 温度回落或拔掉充电器，音量自己回到原来那个值。
void audioRefreshVol();

// ---- 1.45.1 声音链路诊断（只给 /api/pins、/api/tone 用）----
//   在线把 I2S 三脚换掉：11/12/13 一直是推断值，真机从没验证过，
//   靠它可以把 20 种组合挨个试一遍，不用每试一次烧一次固件。
void audioSetPins(int bck, int ws, int din);
// 生成一段单声道 16bit/44.1k 正弦 WAV 到 LittleFS（同一路径已存在就跳过）
bool audioMakeTone(const char* path, int hz, int ms);
// 在线扫 I2S 三脚：每 4 秒换一组候选，边放边换。屏上由 main.cpp 显示当前是第几组。
void        audioSweep(bool on);
bool        audioSweepActive();
int         audioSweepIndex();     // 从 0 开始，-1 = 还没开始
int         audioSweepCount();
const char* audioSweepPins();      // "11/12/13"
const char* audioSweepAmp();       // "使能=低" / "使能=高"

// ---- 1.46.0 没信号噪声 / 自动增益（诊断用）----
//   为什么暴露出来：有些台「没有声音」是源的问题（静音流 / 404），
//   光看 App 上的"播放中"分不出来。这三项一看就知道卡在哪一层：
//     noise=1      → 连不上，正在放本地噪声
//     dec 不涨     → 一个 PCM 块都没解出来（地址/编解码问题）
//     dec 在涨、sil 在涨 → 解出来了但样本全是 0（静音流，现在会被补噪声）
bool     audioNoiseActive();       // 正在放「没信号」噪声
uint32_t audioAgcMilli();          // 当前自动增益，1000 = 1.0 倍
uint32_t audioAgcRms();            // 1.46.30：上一块 PCM 的 RMS（增益之前）
uint32_t audioAgcPeak();           // 1.46.30：上一块 PCM 的峰值
int      audioSilentBlocks();      // 当前连续静音块数（0 = 有声音）
int      audioDecodeBlocks();      // 本次开机解出的 PCM 块数
// 生成一段「沙沙」噪声 WAV 到 LittleFS（同一路径已存在就跳过）
bool audioMakeNoise(const char* path, int ms);

