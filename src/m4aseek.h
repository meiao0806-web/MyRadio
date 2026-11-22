// m4aseek.h —— 1.46.52：有声书 m4a 的「按秒找字节」
//
// 为什么需要它：m4a 的音频数据（mdat）是**裸 AAC 块**，没有同步字、也没有长度前缀，
// 块边界只存在于文件头 moov 的 stsz 表里。所以「听到第 3 分 20 秒」要接着放，
// 必须先把文件头取一次、把 stsz 累加到目标帧，才能知道该从第几个字节起播。
//
// mp3 不需要这套（有同步字，按码率估位置就够了），见 audio.cpp 里的两条路径。
#pragma once
#include <stdint.h>

struct M4aSeek {
  uint32_t absPos;      // 目标帧在文件里的绝对字节位置（已对齐到帧边界）
  uint32_t total;       // 文件总长
  uint32_t dataStart;   // mdat 负载起点（= absPos 的基准）
  uint32_t moovBytes;   // 这次实际读了多少字节（诊断用）
  uint32_t frame;       // 目标帧号（诊断用）
  uint32_t timescale;   // mdhd 时间基准（诊断用）
  uint32_t frames;      // stsz 里的总帧数（诊断用）
};

// 取 url 的文件头，算出「第 sec 秒」该从哪个字节起播。
// 返回 true 才算拿到；false 时调用方应当退回从 0 秒播（不能更差）。
bool m4aSeekOffset(const char* url, int sec, M4aSeek* out);
