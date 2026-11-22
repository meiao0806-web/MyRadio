// ============================================================
//  state.h —— 跨模块共享的全局状态
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// 播放状态（决定屏上那行小字和颜色）
enum PlayState : uint8_t {
  PS_IDLE = 0,   // 未播放
  PS_LINK,       // 正在连接
  PS_PLAY,       // 播放中
  PS_BUFF,       // 缓冲中
  PS_ERR         // 出错 / 重连耗尽
};

struct Station {
  char name[40];
  char url[320];       // 200 装不下喜马拉雅 m4a 直链（实测 212~233，jt= 是签名不能删）
};

struct AppState {
  int      cur;          // 当前台在「本组内」的索引，-1 表示没有台
  int      preview;      // 旋钮正在选、还没落定的台（-1=无）
  int      count;        // 当前组的台数
  int      group;        // 当前分组号（0..storeGroupCount()-1）
  uint8_t  ps;           // PlayState
  bool     muted;
  int      vol;          // 0..100
  bool     wifiOk;
  int      rssi;
  // ---- 收音（Si4732）----
  bool     fmMode;       // true = 正在用芯片收音（WiFi 没连上时自动切）
  int      fmFreq;       // 当前收音频率，单位 kHz：
                         //   153~279 长波 / 531~1602 中波 / 64000~108000 调频 / 2300~26100 短波
                         //   （1.10.0 起不再是「100kHz 的调频值」，一个数就是全部波段）
  int      fmRssi;       // 0..100，给顶栏信号柱用
  // 1.40.1：上次关机/断电时是不是在收音模式（落盘，只在开机第一帧用一次）
  //
  //   为什么需要它：A.fmMode 开机时是 false，于是第一帧画的是「网络广播」，
  //   3 秒后 fmModeTick 发现没网才切成「广播」—— 用户看到的就是
  //   「没网时开机闪一下网络广播页面」。
  //   存下上次的模式，开机就能直接用对的模式画第一帧，中间不闪。
  //   连上 WiFi 之后 fmModeTick 照样会把模式切回网络，所以不怕存错。
  bool     fmModeBoot;

  // ---- 1.22.0：音量分两套（原厂是 volume / netvolume 分开）----
  //   为什么必须分：FM 走 SI4732 自己的音量寄存器，网络走 audioI2S 的软件音量。
  //   两条 0~100 的刻度对应的实际响度差很多 —— 共用一个值的结果是
  //   「网络听得好好的，一断网切到收音，声音突然大/小一截」，老人最容易被这一下吓到。
  int      fmVol;        // 收音那一路的音量（1.44.0：原厂口径 FVOL_MIN..FVOL_MAX = 6..63，落盘）
  // ---- 1.22.0：静噪（原厂 currentSquelch）----
  //   0 = 关（AM 空频道会一直沙沙响）；1..100 = 门限，信号低于它就压成静音。
  int      squelch;
  // ---- 1.22.0：半步进（原厂 halfstep）----
  bool     halfstep;

  // ---- 1.26.0：正在按住旋钮调音量 ----
  //   为真时台名区大字改成「音量 45」，松手立刻恢复台名。
  //   为什么不复用 A.preview 那一套：那个是「旋钮正在选的台」，语义完全不同，
  //   混在一起会让 updatePreview / commitPending 也跟着误动作。
  bool     volHold;
  uint32_t volHoldAt;    // 最后一次动音量的时刻（兜底超时用）

  bool     screenOn;
  int      bright;       // 0..100（自动省电时的实时值）
  int      brightMax;    // 亮度上限（App 设置里的「亮度」，掉电不丢）
  int      scrOffMin;    // 熄屏延时（分钟，0=从不熄屏；App 设置里可改）
  uint32_t lastAct;      // 最后一次操作 millis()，用于熄屏
  char     hint[40];     // 屏上第二行小字（"正在连接…"）
  volatile bool dirty;   // 需要整屏重绘
};

extern AppState A;

// 台表：静态分配，避免堆碎片（120 × 240B ≈ 29KB，S3 完全够）
// 1.45.1：改成指针 —— 380 台（136,800 B）从内部 DRAM 搬进 PSRAM。
//   内部堆被这块表吃掉大半，https 音频（TLS 握手要 30~40 KB）就必然失败。
extern Station* g_st;
extern int     g_stN;

// 一次「操作」：记录时间，唤醒屏幕
static inline void actTouch() {
  A.lastAct = millis();
  if (!A.screenOn) { A.screenOn = true; A.dirty = true; }
}

// 改状态。
//
// 1.34.2 修正 —— 这里原来会置 A.dirty，等于**整屏重绘**。
//   而换一次台会走 PS_IDLE → PS_LOAD → PS_PLAY 两三次状态变化，
//   于是「换台会闪」闪的就是这几次 fillScreen。
//   uiLoop() 的增量检测里 needBox 已经包含 (l_ps != A.ps)，状态框会自己刷新，
//   所以这里只改字段、不置 dirty。
//   （hint 字段在这个固件的界面里根本没有被绘制，纯属旧版残留，改了也不影响显示。）
static inline void setPlayState(PlayState s, const char* hint = nullptr) {
  if (A.ps != s) { A.ps = s; }
  if (hint && strncmp(A.hint, hint, sizeof(A.hint) - 1) != 0) {
    strncpy(A.hint, hint, sizeof(A.hint) - 1);
    A.hint[sizeof(A.hint) - 1] = 0;
  }
}
