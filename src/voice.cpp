// ============================================================
//  voice.cpp —— 把两句播报落到 LittleFS，并按 id 请求播放（1.19.0）
//
//  首次开机做一次"落盘"：
//    voice_data.h 里的字节数组 → /v/low.mp3、/v/off.mp3、/v/scan.mp3
//    之后每次开机只核对大小，对得上就不再写（省 flash 擦写寿命）。
//
//  为什么不每次开机都重写：LittleFS 写 28KB 约 100ms，擦写次数有限；
//  而且用户可能自己换掉文件（比如想换成方言版）—— 大小不一样就重写，
//  这个策略既自愈又不折腾。
// ============================================================
#include "voice.h"
#include "config.h"
#include "audio.h"
#include "voice_data.h"

#include <LittleFS.h>

static const char* kPath[VOICE_N] = { "/v/low.mp3", "/v/off.mp3", "/v/scan.mp3" };
static const size_t kLen [VOICE_N] = { sizeof(kVoLow), sizeof(kVoOff), sizeof(kVoScan) };
static const uint8_t* kSrc[VOICE_N] = { kVoLow, kVoOff, kVoScan };
static bool s_ready[VOICE_N] = { false, false, false };

// 文件名 → 这一段到底能不能播
static bool fileOk(int i) {
  if (!LittleFS.exists(kPath[i])) return false;
  File f = LittleFS.open(kPath[i], "r");
  if (!f) return false;
  const size_t n = f.size();
  f.close();
  return n == kLen[i];
}

static void drop(int i) {
  File f = LittleFS.open(kPath[i], "w");
  if (!f) {
    Serial.printf("[voice] 写不进 %s\n", kPath[i]);
    return;
  }
  const size_t n = f.write(kSrc[i], kLen[i]);
  f.close();
  Serial.printf("[voice] 落盘 %s  %u/%u 字节\n", kPath[i], (unsigned)n, (unsigned)kLen[i]);
}

void voiceBegin() {
#if VOICE_ENABLE
  // 1.27.0 修正：这里原来自己又调了一次 LittleFS.begin(true)。
  //   两个问题：
  //     ① 用的是默认参数 → 去找标签叫 "spiffs" 的分区，而我们的分区标签是 "littlefs"
  //        ⇒ 必然打印 'partition "spiffs" could not be found'；
  //     ② 就算标签对了，store.cpp 已经挂载过，第二次 begin() 也会返回 false
  //        ⇒ 被误判成"挂不上"，播报白白关掉。
  //   正确做法：文件系统由 storeBegin() 统一挂载，这里只检查挂没挂上，不再重复 begin。
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[voice] LittleFS 挂不上，播报功能关闭");
    return;
  }
  LittleFS.mkdir("/v");
  for (int i = 0; i < VOICE_N; i++) {
    if (!fileOk(i)) drop(i);
    s_ready[i] = fileOk(i);
    if (!s_ready[i]) Serial.printf("[voice] %s 不可用\n", kPath[i]);
  }
  int okN = 0;
  for (int i = 0; i < VOICE_N; i++) if (s_ready[i]) okN++;
  Serial.printf("[voice] 播报就绪 %d/%d，共 %u 字节\n",
                okN, (int)VOICE_N, (unsigned)VOICE_BYTES_TOTAL);
#else
  Serial.println("[voice] VOICE_ENABLE = 0，播报功能关闭");
#endif
}

bool voiceReady(VoiceId id) {
  const int i = (int)id;
  return (i >= 0 && i < VOICE_N) ? s_ready[i] : false;
}

const char* voicePath(VoiceId id) {
  const int i = (int)id;
  return (i >= 0 && i < VOICE_N) ? kPath[i] : "";
}

bool voicePlay(VoiceId id) {
#if VOICE_ENABLE
  if (!voiceReady(id)) return false;
  return audioVoice(voicePath(id));
#else
  (void)id;
  return false;
#endif
}

bool voiceBusy() { return audioVoiceBusy(); }

void voiceJson(String& o) {
  int okN = 0;
  for (int i = 0; i < VOICE_N; i++) if (s_ready[i]) okN++;
  o += "\"vready\":";
  o += okN;
  o += ",\"vbusy\":";
  o += audioVoiceBusy() ? 1 : 0;
}
