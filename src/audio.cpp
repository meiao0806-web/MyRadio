#include "audio.h"
#include "store.h"
#include "album.h"
#include "epis.h"
#include "resume.h"
#include "evlog.h"      // 1.46.40：断点/续播留痕
#include "m4aseek.h"    // 1.46.52：m4a 续播（取文件头算字节位置）          // 1.46.2：有声书听到第几秒
#include "../lib/ESP32-audioI2S/src/Audio.h"
#include "health.h"
#include "offair.h"         // 1.46.6：停播台台账
#include "power.h"          // 1.21.0：音量上限（过热 / 满电时会被压下来）
#include "fm.h"             // 1.22.0：收音那一路有自己的音量寄存器
#include <LittleFS.h>

// ------------------------------------------------------------
//  Audio 对象只被 audioTask 访问；其它任务通过 s_cmd 发指令
// ------------------------------------------------------------
static Audio s_audio;

enum Cmd : int { CMD_NONE = 0, CMD_PLAY, CMD_VOL, CMD_PAUSE, CMD_STOP, CMD_MUTE, CMD_REPLAY,
                 CMD_VOICE,     // 1.19.0：念一段本地 mp3（电量播报）
                 CMD_REVOL,     // 1.21.0：上限变了，按新上限重设一次音量
                 CMD_PINS };    // 1.45.1：诊断 —— 在线换 I2S 三脚
// 1.45.1：诊断用的「换 I2S 三脚」请求（由 audioTask 执行，别在别的任务里碰 Audio 对象）
static volatile bool s_pinReq = false;
static volatile int  s_pBck = 11, s_pWs = 12, s_pDin = 13;

static volatile int s_cmd = CMD_NONE;
static volatile int s_arg = 0;

static bool     s_active   = false;   // 已下达播放指令
static bool     s_paused   = false;
static bool     s_gotAudio = false;   // 收到过流的元数据（说明真的通了）
static uint32_t s_connectAt = 0;
static uint32_t s_dropAt    = 0;
static uint32_t s_playAt    = 0;      // 这一台真正出声的时刻
static uint32_t s_pauseAt   = 0;      // 1.46.38：本次暂停的开始时刻（0 = 没暂停）
static int      s_retry     = 0;
static int      s_deadLinks = 0;      // 开机以来跳过的死链数
static int      s_bitrate   = 0;      // kbps
static char     s_url[336];           // 当前正在播的真实地址（蜻蜓台号已展开；喜马拉雅直链最长 233）
static bool     s_eof = false;        // 点播（有声书单集）播完了，等主循环接下一集
static uint32_t s_odAt = 0;            // 1.46.34：点播节目上次「播完重放」的时刻
static uint8_t  s_odLoop = 0;          //   60 秒内重放的次数（防短文件死循环）
// 1.9.0：一部书 = 一个台。s_epiB 是当前台对应的书号（-1 = 普通电台），
// s_epiE 是正在播的集号，s_epiFail 是「这本书连着几集连不上」，防止死链把整本书刷一遍。
static int      s_epiB = -1;
static int      s_epiE = -1;
static int      s_epiFail = 0;
static int      s_epiLast = -2;       // 上一次 startIndex 的台号（用来判断「换台了没」）

// 1.19.0：播报。s_voiceOn 期间整个看门狗都让路 —— 念一句「没电了，关机」
// 不该被判成「这个台连不上」然后跳台。
static bool     s_voiceOn   = false;
static bool     s_voiceBack = false;   // 念完要不要把节目接回来
static uint32_t s_voiceAt   = 0;
static int      s_voiceVol  = -1;      // 播报前用户设的音量（-1 = 没改过）
static char     s_voiceUrl[40] = {0};
static volatile bool s_voiceReq = false;   // 已排队、任务还没取走

// 直播流（蜻蜓 /live/）永远不该 EOF：它要是断了，属于网络问题，得走重连。
// 只有点播单文件（喜马拉雅 m4a）播到结尾才是"真的听完了"。
static inline bool isLiveUrl() { return strstr(s_url, "/live/") != nullptr; }
// 1.46.2：只有整段 mp3 能续播。m4a/aac 的 moov 在文件头，掐掉开头就解不出音；
//   直播流本来就没有「听到哪」这回事。
// 只有 mp3 能按字节续播。
//   m4a 不行 —— 实测（tools/_chk_mdat.py）：它的 mdat 是**裸 AAC 块**，
//   既没有同步字、也没有长度前缀（第一帧 371 字节，但负载前 4 字节读出来是 5.5 亿），
//   块边界只存在于文件头 moov 的 stsz 表里。从中间起播 = 解不出来。
//   要做 m4a 续播，得先单独取一次文件头、把 stsz 累加到目标帧（见 CHANGELOG 1.46.38）。
static inline bool isSeekUrl() { return strstr(s_url, ".mp3") != nullptr; }
// 1.46.52：m4a 现在也能续播了，但走**另一条路**（不是按码率估字节）：
//   取一次文件头 → 顺着 moov 找到 stsz → 累加到目标帧 → 得到精确字节位置。
//   详见 m4aseek.cpp 顶部。地址里可能是 .m4a 也可能带 query，用子串判断足够。
static inline bool isM4aUrl() {
  return strstr(s_url, ".m4a") != nullptr || strstr(s_url, ".mp4") != nullptr;
}

// ------------------------------------------------------------
//  1.46.0：没信号噪声 / 自动增益的状态（只有 audioTask 会碰）
// ------------------------------------------------------------
#define NOISE_PATH "/noise.wav"
static bool     s_noiseOn   = false;   // 正在放本地噪声（连不上时的兜底）
static bool     s_noiseLoop = false;   // 噪声文件放完了，等任务接着放
static uint32_t s_noiseTry  = 0;       // 上次重试真流的时刻
static uint32_t s_silBlocks = 0;       // 连续静音块数（PCM 层，进来就是 0 样本）
static int32_t  s_agcQ10    = 1024;    // 当前增益，Q10（1024 = 1.0 倍）
static uint32_t s_agcPk     = 0;       // 上一块的原始峰值（诊断）
// 1.46.30：有声书响度统一用的四个状态
static uint32_t s_pkEnv    = 0;       // 近期最响的峰值（每块 -0.6% 缓降）—— 上限拿它算，不拿单块峰值
static uint32_t s_agcRms   = 0;       // 上一块的 RMS（诊断 / 测试要看它有没有到目标）
static bool     s_agcFresh = true;    // 刚换台/换集：允许头一块一次性接到 3 倍以内
static bool     s_agcRst   = false;   // 噪声模式把增益归过 1.0，真声回来要按缓存接回
static int16_t  s_gainCache[AGC_GAIN_CACHE_N] = {0};  // 每台学到的增益（Q10，只放 RAM）
static uint32_t s_decBlocks = 0;       // 解出过多少块 PCM（诊断：0 = 根本没解码）
// 1.46.2：断点续播
static int      s_posBase   = 0;      // 这次是从第几秒接着放的（0 = 从头）
static int      s_posKbps   = 0;      // 上次听到时这一集的码率
static bool     s_posResumed = false; // 这次是续播出来的（诊断）
static uint32_t s_posAt     = 0;      // 上次记进度的时刻
static int      s_noiseTries = 0;     // 在沙沙里已经重试了几回
// 1.46.6：停播台账的两个标志（都只在 audioTask 里改）
static bool     s_probe  = false;     // 这台以前被判过停播，这次是到期回来复查
static bool     s_silHit = false;     // 静音流到点了，等任务循环去换台（回调里不能换）
// 1.46.22：自动跳台的连续计数（只有 audioTask 会碰）
static uint8_t  s_autoHop   = 0;      // 这一串自动跳了几台
static uint32_t s_autoHopAt = 0;      // 上一次自动跳的时刻
static uint32_t s_nzR       = 20260101u;
static int32_t  s_nzLp      = 0;

// 噪声发生器（定义在文件后面、PCM 回调旁边）。
//   这里先声明一次：生成 WAV 的 audioMakeNoise 在前面，它也要用同一个发生器，
//   否则两边"沙沙"的音色会不一样。
static void nzFill(int16_t* b, uint32_t n);

// 1.44.0：**不再换算**。网络这一路的音量值就是 audioI2S 的 setVolume(0..21)，
//   FM 那一路就是 SI4732 的 RX_VOLUME(0..63) —— 原厂两个库都是直接吃这个数。
//   1.38.5～1.43.0 那层「10 档 → 按比例映到 0..21 / 0..63」已删：
//   多一层换算就多一处能算错的地方（1.40.0 正是栽在这上面 ——
//   「音量 4」被当成寄存器值 4/63 写进芯片，几乎没声）。
static int netVolCap() {
  int cap = NVOL_MAX * powerVolCapPct() / 100;
  if (cap > NVOL_MAX) cap = NVOL_MAX;
  if (cap < NVOL_MIN) cap = NVOL_MIN;
  return cap;
}

// 1.21.0：真正送进解码器的音量 = min(用户设的, 电源保护给的上限)。
// !! 只夹「送出去的」，不改 A.vol —— 过热降载是临时的，温度回落要能自动回到
//    用户原来那个音量，不能把用户的设置改掉（还会被 storeSave 写进 flash）。
static void applyVol() {
  int v = A.vol;
  const int cap = netVolCap();
  if (v > cap) v = cap;
  if (v < NVOL_MIN) v = NVOL_MIN;      // 1.44.0：下限固定 6
  s_audio.setVolume((uint8_t)v);
}

static void savePosNow();      // 1.46.38：定义在后面，startIndex 要用

static void startIndex(int idx) {
  if (idx < 0 || idx >= g_stN) { s_active = false; return; }
  // 1.46.38：换到**别的台**时，先把这一台听到哪儿记下来（同一本书换集不记，
  //   那种情况由 nextEpOrStation 里的 resumeClear 负责）。
  if (idx != A.cur && s_epiB >= 0 && s_epiE >= 0 && s_gotAudio) savePosNow();
  A.cur = idx;
  s_paused = false;
  s_gotAudio = false;
  s_retry = 0;
  s_bitrate = 0;
  s_connectAt = millis();
  s_playAt = 0;
  s_dropAt = 0;
  s_eof = false;
  s_active = true;
  // 1.46.0：换台要把「没信号噪声」收掉；静音计数也归零，
  //   否则上一台是静音流时，新台一上来就先沙沙一秒。
  s_noiseOn = false;
  s_noiseLoop = false;
  s_silBlocks = 0;
  s_agcPk = 0;
  // 1.46.30：换台/换集先把"这台上次学到的增益"接上，再清峰值包络。
  //   s_agcFresh 只放一块：头一块量到真电平后一次性把增益接到目标附近，
  //   省掉"换过去先小声 5 秒"。
  s_pkEnv = 0;
  s_agcFresh = true;
  s_agcRst = false;
  if (idx < AGC_GAIN_CACHE_N && s_gainCache[idx] > 0) s_agcQ10 = s_gainCache[idx];
  // 正在念播报时被要求换台：直接收掉播报，别接回来（用户意图优先）
  if (s_voiceOn) {
    s_voiceOn = false;
    s_voiceBack = false;
    if (s_voiceVol >= 0) { applyVol(); s_voiceVol = -1; }
  }

  setPlayState(PS_LINK, "正在连接…");

  // 换到别的台才把「这本书连不上」的计数清零；同一本书里换集要接着数
  if (idx != s_epiLast) { s_epiFail = 0; s_epiLast = idx; }

  const int bk = episBookOfStation(idx);
  s_epiB = bk;
  if (bk >= 0) {
    // 有声书：从上次听的那一集接着放（没听过就是第 1 集）
    s_epiE = episCur(bk);
    // 1.46.33：App 里设过「下次调到这个台，从第 X 集起」→ 这次就用它，用一次作废。
    //   设的时候只写了标记，机器既没换台也没起播，所以走到这里才第一次生效。
    {
      const int nx = episTakeNext(bk);
      if (nx >= 0) {
        s_epiE = nx;
        Serial.printf("[epis] 按 App 的设置，从第 %d 集起播\n", s_epiE + 1);
      }
    }
    if (!episUrl(bk, s_epiE, s_url, sizeof(s_url)))
      storeUrl(idx, s_url, sizeof(s_url));   // 集表没编进去时退回台表里的地址
    episSetCur(bk, s_epiE);                  // 记进度（2 秒合并写盘）
  } else {
    s_epiE = -1;
    storeUrl(idx, s_url, sizeof(s_url));     // 蜻蜓台号在这里展开成真实地址
  }
  albumRemember(idx);                       // 1.7.0：记住这部书听到了第几集
  healthStart(idx);                         // 1.17.0：这次播完要给这一台打分

  // 1.46.6：以前判过「现在不播」、现在倒计时到期回来的台，用短超时复查。
  //   正常连接超时是 20 秒（点播 35 秒）—— 对着一个整夜没开播的台干等 20 秒、
  //   再补 30 秒沙沙，用户看到的就是「转过去一片死寂」。9 秒足够判断"连得上连不上"。
  s_probe  = (bk < 0) && offairProbe(idx);
  s_silHit = false;

  // ---- 1.46.2：有声书断点续播 ----
  //   换台回来 / 关机再开，都从上次那一集的那一分钟接着放。
  //   做法是给 HTTP 请求加 Range，从头之外的第 N 个字节起播（按秒 × 码率 ÷ 8 换算）。
  s_posBase    = 0;
  s_posKbps    = 0;
  s_posResumed = false;
  s_posAt      = millis();
  s_noiseTries = 0;
  uint32_t rangeFrom = 0;
  // 1.46.52：m4a 的「尾巴模式」参数
  bool     m4aTail   = false;
  uint32_t m4aTailSz = 0;
  int      m4aSec    = 0;
  // ⚠️ ASC 必须留到最后一次 stopSong() **之后**才交给库 ——
  //   stopSong() 开头无条件清 m_aacAot/m_aacBaseRate/m_aacChans（1.46.21 的修复，
  //   防「换一本书就崩」）。在这儿设等于白设，尾巴模式下会拿默认 2ch/44100
  //   去解 22050Hz 的蜻蜓音频，每帧都错。
  int      m4aAot = 0, m4aCh = 0, m4aRate = 0;

  if (bk >= 0 && (isSeekUrl() || isM4aUrl())) {
    int rep = -1, rsec = 0, rkbps = 0, aot = 0, ach = 0, arate = 0;
    if (resumeGet(bk, &rep, &rsec, &rkbps, &aot, &ach, &arate) &&
        rep == s_epiE && rsec >= RESUME_MIN_SEC) {
      if (isM4aUrl()) {
        // ---- m4a：先取文件头，把「第 rsec 秒」算成精确字节 ----
        //   为什么不能像 mp3 那样按码率估：m4a 的 bitrate 在库里是
        //   bps×声道×采样率（未压缩 PCM 口径，1411kbps），跟真实码率差 20 倍；
        //   而且 VBR 每帧大小都不同，只能靠 stsz 累加。
        //   代价：多一次 HTTP 请求（实测 0.3~1.5 秒）。失败就退回从头播，不变差。
        //   ⚠️ 取头前必须先把解码器停了（下一行的 stopSong）——取头要 malloc 160KB
        //      PSRAM + 做一次 TLS 握手，解码器缓冲还占着就容易失败。
        s_audio.stopSong();
        M4aSeek ms;
        if (m4aSeekOffset(s_url, rsec, &ms) && ms.total > ms.absPos) {
          rangeFrom = ms.absPos;
          m4aTail   = true;
          m4aTailSz = ms.total - ms.absPos;
          m4aSec    = rsec;
          s_posBase    = rsec;
          s_posKbps    = rkbps;
          s_posResumed = true;
          // ASC 记下来，等最后一次 stopSong() 之后再交给库（见下面的说明）
          if (aot > 0 || arate > 0) { m4aAot = aot; m4aCh = ach; m4aRate = arate; }
          evlog("audio", "续播(m4a) 第%d集 从%d秒 字节%u",
                s_epiE + 1, rsec, (unsigned)ms.absPos);
          Serial.printf("[resume] m4a 第 %d 集从 %d 秒（字节 %lu / 尾巴 %lu，ASC %d/%d/%d）\n",
                        s_epiE + 1, rsec, (unsigned long)ms.absPos,
                        (unsigned long)m4aTailSz, aot, ach, arate);
        } else {
          evlog("audio", "m4a 定位失败，第%d集从头播", s_epiE + 1);
        }
      } else if (rkbps > 0) {
        // ---- mp3：按码率估字节（CBR，够准） ----
        evlog("audio", "续播 第%d集 从%d秒", s_epiE + 1, rsec);
        s_posBase    = rsec;
        s_posKbps    = rkbps;
        s_posResumed = true;
        rangeFrom = (uint32_t)((uint64_t)rsec * (uint64_t)rkbps * 1000ull / 8ull);
        Serial.printf("[resume] 第 %d 集从 %d 秒（%lu 字节）接着放\n",
                      s_epiE + 1, rsec, (unsigned long)rangeFrom);
      }
    }
  }
  Serial.printf("[audio] play #%d %s\n", idx, s_url);
  s_audio.stopSong();
  s_audio.setRangeFrom(rangeFrom);
  if (m4aTail) {
    s_audio.setM4aTailMode(m4aTailSz, (uint32_t)m4aSec);
    // 尾巴模式下流里没有 moov，解码参数只能从断点记录里来。
    // 必须放在上面那行 stopSong() 之后 —— 它会把 ASC 清空。
    s_audio.setM4aRawParams(m4aAot, m4aCh, m4aRate);
  }
  s_audio.connecttohost(s_url);
  storeSaveDebounced();
}

static void enterNoise();     // 定义在下面；刹车要用

// 1.46.38：把「这一集听到第几秒」立刻落一条。
//   除了 30 秒定时，**离开这一台时也要立刻记一次** —— 否则用户「第 5 集 3 分 20 秒
//   换走」，回来只能回到上次定时存的位置（最多差半分钟），跟他要的「还是 3 分 20 秒」不符。
//   ASC 一起带上：m4a 从中间起播要靠它解（mp3 用不到，自然是 0）。
// 1.46.40：这一集的码率，三个来源依次退。
//   为什么必须兜底：真机 /api/diag 的 audio.kbps 一直是 0（蜻蜓 64k.mp3 播放中也是 0），
//   说明库的 bitrate 回调没喂到 s_bitrate。而 resume 的续播条件是 `rkbps > 0`，
//   拿不到码率 = 断点记录里 kbps 永远 0 = 每集都从 0 秒开始（用户说的「从头开播」）。
static int curKbps() {
  if (s_bitrate > 0) return s_bitrate;
  const uint32_t b = s_audio.getBitRate(true);      // 库里的平均码率
  if (b > 0 && b < 1000000u) return (int)(b / 1000u);
  if (s_posKbps > 0) return s_posKbps;
  return 0;
}

static void savePosNow() {
  if (s_epiB < 0 || s_epiE < 0) return;
  int aot = 0, ach = 0, arate = 0;
  s_audio.getM4aAsc(&aot, &ach, &arate);
  // 1.46.54：**向上取整**。用户要求「播过的那一段不要再播」：
  //   听了 5.9 秒，向下取整记成 5 秒 → 回来先把这 0.9 秒重播一遍。
  //   向上取整记 6 秒 → 从第 7 秒起播，跳过的最多 1 秒（听不出来），
  //   但绝不会重播。这也正好对应他说的「播了 5 秒要从第 6 秒开始」。
  const uint32_t el = s_playAt ? (uint32_t)(millis() - s_playAt) : 0;
  const int pos = s_posBase + (int)((el + 999u) / 1000u);
  // 1.46.52：m4a 续播不靠码率（靠 stsz 累加），所以码率拿不到也要记 ——
  //   否则 m4a 的断点会因为 kb=0 被当成「无效记录」，永远续不上。
  int kb = curKbps();
  if (kb <= 0 && isM4aUrl()) kb = 1;      // 占位：表示「这条记录是有效的」
  resumeRemember(s_epiB, s_epiE, pos, kb, aot, ach, arate);
  Serial.printf("[resume] 记：第 %d 集 %d 秒（%dkbps asc=%d/%d/%d）\n",
                s_epiE + 1, pos, kb, aot, ach, arate);
  evlog("audio", "记断点 第%d集 %d秒 %dkbps", s_epiE + 1, pos, kb);
}

// 1.46.22：自动跳台的刹车。
//   用户报「140 多号突然跳到 280 多」—— 这不是转一格能出来的量级，是自动换台
//   连着跳：连不上 → 判停播 → 跳下一个 → 也连不上 → 再跳……网络一抖几十秒能跳几十台。
//   现在同一个原因连跳 AUTO_HOP_MAX 台就停住，放沙沙 + 提示，等用户自己动手。
//   用户手动切台（CMD_PLAY）会把 s_autoHop 清零，所以不挡正常换台。
static void advance(int dir) {
  if (g_stN == 0) return;
  const uint32_t now = millis();
  if (s_autoHopAt == 0 || (uint32_t)(now - s_autoHopAt) > AUTO_HOP_WINDOW_MS) s_autoHop = 0;
  if (s_autoHop >= AUTO_HOP_MAX) {
    Serial.printf("[auto] 连跳 %d 台都连不上 -> 停在这一台（%s）\n",
                  (int)s_autoHop, (A.cur >= 0 && A.cur < g_stN) ? g_st[A.cur].name : "?");
    // 注意：**不清零**。清零的话下一次噪声重试又会放行 6 台，
    //   就变成「跳一格、停一下、再跳一格」，等于没刹。
    //   保持计数不变，靠上面那句「距上次超过 AUTO_HOP_WINDOW_MS 才归零」
    //   在 25 秒后自动恢复 —— 既不锁死，也不会在窗口内接着飞。
    s_autoHopAt = now;
    enterNoise();
    setPlayState(PS_ERR, "信号不好，停在这一台");
    return;
  }
  s_autoHop++;
  s_autoHopAt = now;
  int i = A.cur;
  if (i < 0) i = 0;
  // 1.46.6：自动换台（死链 / 一集播完 / 一本书全连不上）也要跳过停播台
  else i = offairStep(i, dir >= 0 ? 1 : -1);
  startIndex(i);
}

// ------------------------------------------------------------
//  1.46.0：「没信号」两种兜底
// ------------------------------------------------------------
// 连不上：不再跳台，改成放本地噪声循环 + 定时重试真流。
//   为什么用本地 WAV 而不是现造噪声：连不上时根本没有 PCM 回调，
//   只能在 Audio 对象上"放个文件"；文件短（6 秒）所以循环时几乎无缝。
static void enterNoise() {
  if (s_noiseOn) return;
  if (!LittleFS.exists(NOISE_PATH)) audioMakeNoise(NOISE_PATH, NOISE_FILE_MS);
  s_audio.stopSong();
  s_noiseOn   = true;
  s_noiseLoop = false;
  s_gotAudio  = false;
  s_paused    = false;
  s_eof       = false;
  s_silBlocks = 0;
  s_active    = true;
  s_noiseTry  = millis();
  applyVol();
  if (!s_audio.connecttoFS(LittleFS, NOISE_PATH)) {
    Serial.println("[noise] 噪声文件打不开，只能静音");
    s_active = false;
  }
  setPlayState(PS_ERR, "这个台没有信号");
  Serial.printf("[noise] #%d %s 连不上 -> 无信号噪声\n", A.cur,
                (A.cur >= 0 && A.cur < g_stN) ? g_st[A.cur].name : "?");
}

// 噪声放到一半，再试一次真流（成功的话音频回调会自己把 s_noiseOn 关掉前的状态接上）
static void retryStation() {
  s_noiseOn   = false;
  s_noiseLoop = false;
  s_gotAudio  = false;
  s_retry     = 0;
  s_connectAt = millis();
  s_silBlocks = 0;
  s_audio.stopSong();
  setPlayState(PS_LINK, "正在连接…");
  Serial.printf("[noise] 再试一次 %s\n", s_url);
  s_audio.connecttohost(s_url);
}

// 播报念完（或超时）：还原音量，需要的话把节目接回来
static void finishVoice() {
  if (!s_voiceOn) return;
  s_voiceOn = false;
  s_voiceAt = 0;
  if (s_voiceVol >= 0) { applyVol(); s_voiceVol = -1; }
  const bool back = s_voiceBack;
  s_voiceBack = false;
  if (back && A.cur >= 0 && !A.fmMode) {
    Serial.println("[audio] 播报结束，接回节目");
    startIndex(A.cur);
  }
}

// 一集结束（播完 or 连不上）之后往哪走：
//   有声书 → 同一台换下一集（播完循环回第一集；连不上就试下一集，试完一圈才换台）
//   普通台 → 换下一台
static void nextEpOrStation(bool eof) {
  // 1.46.37：网络整体不通时，**绝不能**推进集表。
  //   原来断网 → 每一集都连不上 → 换下一集 → 再连不上 → 再换……
  //   沙沙里每两轮重试又推一集，几十秒就能把「听到第 12 集」推到绕回第 1 集。
  //   用户看到的就是「调走再回来又从第一集开头播」。
  //   现在：网络断了就原地不动 —— 不换集、不动进度、连断点都不清。
  if (!eof && !A.wifiOk) {
    Serial.println("[audio] 断网中，不动集表（保持在当前集）");
    s_epiFail = 0;
    s_retry = 0;
    setPlayState(PS_BUFF, "网络断了，等它回来");
    enterNoise();          // 有沙沙声，用户知道机器还活着
    return;
  }
  if (s_epiB >= 0) resumeClear(s_epiB);   // 1.46.2：这一集结束，断点作废
  s_posBase = 0;                           // 换集后从 0 起播
  s_posResumed = false;
  if (s_epiB >= 0 && episCount(s_epiB) > 1) {
    if (eof || s_epiFail + 1 < episCount(s_epiB)) {
      if (!eof) s_epiFail++;
      const int e2 = episStep(s_epiB, +1);     // 最后一集之后回第一集
      episSetCur(s_epiB, e2);
      Serial.printf("[audio] %s，接第 %d 集\n", eof ? "一集播完" : "这一集连不上", e2 + 1);
      startIndex(A.cur);                       // 还是同一个台，只换集
      return;
    }
    Serial.println("[audio] 整本书都连不上，换下一个台");
  }
  // 1.46.34：不是书、也不是直播 —— 这是「点播节目」（蜻蜓节目页 / 单集 mp3 / m4a）。
  //   以前走下面那句 advance(+1)，用户听到的就是「播完机器自己跳到别的台」。
  //   现在：播完**回到第 1 集（头）重放本台**，绝不换台 —— 用户明确要求
  //   「点播节目播完需要从第一集播，不要调台」。
  if (eof && !isLiveUrl()) {
    const uint32_t now = millis();
    // 短节目保护：一个 3 秒的坏文件会「播完→重来」无限打转，把网络和 CPU 全占住。
    //   60 秒内重放超过 6 次就停在本台不放了（屏上显示「播完了」），等人来处理。
    if (s_odAt == 0 || (uint32_t)(now - s_odAt) > 60000u) s_odLoop = 0;
    s_odAt = now;
    if (++s_odLoop > 6) {
      Serial.println("[audio] 点播节目太短/反复结束，停止重放");
      s_odLoop = 0;
      s_epiFail = 0;
      s_active = false;
      setPlayState(PS_IDLE, "播完了");
      return;
    }
    Serial.printf("[audio] 点播节目播完，从第 1 集重放（第 %d 次）\n", s_odLoop);
    s_epiFail = 0;
    // 走到这里的有两种：① 普通点播（没有集的概念）；② 只有 1 集的书。
    // 后者要明确把进度按回第 1 集，否则 startIndex 会拿「刚才那集」再放一遍。
    if (s_epiB >= 0) episSetCur(s_epiB, 0);
    startIndex(A.cur);          // 同一台，从头开始；断点/进度已在函数开头清掉
    return;
  }
  s_epiFail = 0;
  if (g_stN > 1) {
    advance(+1);
  } else {
    s_active = false;
    if (eof) setPlayState(PS_IDLE, "播完了");
  }
}

// ------------------------------------------------------------
//  任务
// ------------------------------------------------------------
// ------------------------------------------------------------
//  1.45.2：I2S 三脚扫描（诊断）
//
//    1.45.1 那版有两个硬伤，真机上都踩到了：
//      ① 候选表里有 5~9 和 39~42/47/48 —— 那是屏幕的并口控制线（RST/CS/DC/WR/RD）
//         和数据线 D0~D7！拿它们当 I2S，等于往屏幕总线上灌方波，
//         屏幕立刻花屏，而且一直花到重启（用户实测：「第 9 组之后一直花屏」）。
//      ② 表里还有 10（功放使能）、15（外设电源自锁）、16（SI4732 复位）、
//         17/18（I2C）—— 全是别人的线，碰一下功放就哑、收音就失联。
//
//    这块板子真正空的只有 11/12/13/14 四个脚（见 config.h 的排除法注释），
//    所以候选只在它们里面取有序三元组：4×3×2 = 24 种。
//    再把「功放使能极性」当第二个变量一起试（低/高各一遍），共 48 组。
//    前两组固定是「当前引脚 11/12/13 + 功放低 / 功放高」——
//    因为最早怀疑的就是极性，这样一开跑前 6 秒就能出结论。
//
//    每一组都放一段本地提示音（LittleFS 里的 WAV），不走网络：
//    这样听到的「嘀——」只可能来自 I2S → DAC → 功放这一段，排除流地址的干扰。
#define SWEEP_STEP_MS 2600        // 每组多少毫秒（提示音 2 秒 + 间隔）

static const uint8_t kFreePins[] = { 11, 12, 13, 14 };
static const int kFreeN  = (int)(sizeof(kFreePins) / sizeof(kFreePins[0]));
static const int kSweepN = 48;    // 24 种引脚组合 × 2 档功放极性

// 功放使能 + 静音脚：扫描期间这两个也要跟着摆对，否则 A 组有声音、B 组没有
static void setAmpLevel(int high) {
#if PIN_AMP_EN >= 0 && AMP_EN_ENABLE
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, high ? HIGH : LOW);
#endif
#if PIN_FM_AMP_MUTE >= 0
  pinMode(PIN_FM_AMP_MUTE, OUTPUT);
  digitalWrite(PIN_FM_AMP_MUTE, LOW);      // 静音脚始终「不静音」，只让使能脚变
#endif
}

// idx -> (BCK, LCK, DIN, 功放电平)。lvl: 0=低 1=高
static void sweepCombo(int idx, int& b, int& w, int& d, int& lvl) {
  lvl = idx & 1;
  int k = idx >> 1;                        // 0..23
  const int i0 = k / 6; k %= 6;
  const int i1 = k / 2; k %= 2;
  const int i2 = k;
  int rem1[4], n1 = 0;
  for (int i = 0; i < kFreeN; i++) if (i != i0) rem1[n1++] = i;
  const int c1 = rem1[i1];
  int rem2[4], n2 = 0;
  for (int i = 0; i < n1; i++) if (rem1[i] != c1) rem2[n2++] = rem1[i];
  b = kFreePins[i0];
  w = kFreePins[c1];
  d = kFreePins[rem2[i2]];
}

static volatile bool s_swOn  = false;
static volatile int  s_swIdx = -1;
static volatile int  s_swLvl = 0;
static uint32_t      s_swAt  = 0;
static bool          s_swNeedRestore = false;

void audioSweep(bool on) {
  if (on) {
    s_swIdx = -1;
    s_swAt  = 0;
    s_swNeedRestore = false;
  } else {
    s_swNeedRestore = true;                // 复原动作交给 audioTask（只有它能碰 s_audio）
  }
  s_swOn = on;
  Serial.printf("[i2s] 扫描%s，共 %d 组\n", on ? "开始" : "停止", kSweepN);
}
bool audioSweepActive() { return s_swOn; }
int  audioSweepIndex()  { return s_swIdx; }
int  audioSweepCount()  { return kSweepN; }
const char* audioSweepPins() {
  static char b[40] = "---";
  if (s_swIdx < 0 || s_swIdx >= kSweepN) return b;
  int x, y, z, lv;
  sweepCombo(s_swIdx, x, y, z, lv);
  snprintf(b, sizeof(b), "%d/%d/%d", x, y, z);
  return b;
}
const char* audioSweepAmp() {
  static char b[12] = "---";
  if (s_swIdx < 0 || s_swIdx >= kSweepN) return b;
  snprintf(b, sizeof(b), s_swLvl ? "使能=高" : "使能=低");
  return b;
}

// 1.46.14-diag：heap corruption 巡检。
//  背景：调到喜马拉雅书台（redirect m4a）后 WiFi ppTask 在内部堆分配时崩
//  （StoreProhibited），真凶是别的代码把堆的空闲链表写坏了。
//  巡检发现坏块就当场 abort —— backtrace 离真凶最近。
#include <esp_heap_caps.h>
#include <esp_system.h>
void heapAuditDiag(const char* where) {
    if (!heap_caps_check_integrity_all(true)) {
        Serial.printf("[heapchk] CORRUPT at %s\n", where);
        Serial.flush();
        esp_system_abort("HEAP_CORRUPT");
    }
}

void audioTask(void*) {
  A.vol = constrain(A.vol, NVOL_MIN, NVOL_MAX);
  applyVol();                       // 1.21.0：统一走 applyVol，别绕过电源上限

  for (;;) {
    // 0) 1.45.2：扫描结束 —— 引脚、功放、播放状态全部复原
    if (!s_swOn && s_swNeedRestore) {
      s_swNeedRestore = false;
      s_swIdx = -1;
      setAmpLevel(AMP_EN_LEVEL == HIGH ? 1 : 0);
      s_audio.stopSong();
      s_audio.setPinout(PIN_I2S_BCK, PIN_I2S_LCK, PIN_I2S_DIN);
      s_active = false; s_paused = false; s_voiceOn = false;
      s_gotAudio = false; s_eof = false;
      A.dirty = true;
      Serial.println("[i2s] 扫描结束，引脚/功放已复原");
    }

    // 0) 1.45.2：I2S 三脚 + 功放极性扫描 —— 每 SWEEP_STEP_MS 换一组
    //    换脚、放音都必须在 audioTask 里做（setPinout 会重装 I2S 驱动）
    if (s_swOn && (s_swIdx < 0 || millis() - s_swAt >= SWEEP_STEP_MS)) {
      s_swIdx = (s_swIdx + 1) % kSweepN;
      s_swAt  = millis();
      int b, w, d, lvl;
      sweepCombo(s_swIdx, b, w, d, lvl);
      s_swLvl = lvl;
      setAmpLevel(lvl);
      s_audio.setPinout((uint8_t)b, (uint8_t)w, (uint8_t)d);

      if (!LittleFS.exists("/tone.wav")) audioMakeTone("/tone.wav", 1000, 2000);

      // 先停掉网络流，再放本地提示音：这一步只考察「I2S → DAC → 功放」
      s_audio.stopSong();
      s_paused = false; s_gotAudio = false; s_eof = false; s_voiceOn = false;
      s_active = true;
      s_audio.setVolume((uint8_t)NVOL_MAX);
      if (!s_audio.connecttoFS(LittleFS, "/tone.wav")) Serial.println("[i2s] 提示音打不开");
      Serial.printf("[i2s] 试音 %d/%d  BCK=%d LCK=%d DIN=%d 功放使能=%d\n",
                    s_swIdx + 1, kSweepN, b, w, d, lvl);
    }

    // 0) 1.45.1：诊断用的换脚请求 —— 必须在 audioTask 里做，
    //    因为 setPinout 会动 I2S 驱动，和 loop() 里的解码抢同一个外设。
    if (s_pinReq) {
      s_pinReq = false;
      const int bck = s_pBck, ws = s_pWs, din = s_pDin;
      s_audio.stopSong();
      s_active = false;
      const bool ok = s_audio.setPinout((uint8_t)bck, (uint8_t)ws, (uint8_t)din);
      Serial.printf("[i2s] 换脚 BCK=%d LCK=%d DIN=%d -> %s\n", bck, ws, din, ok ? "ok" : "fail");
      setPlayState(PS_IDLE, "");
    }

    // 1) 处理指令
    int c = s_cmd;
    if (c != CMD_NONE) {
      s_cmd = CMD_NONE;
      switch (c) {
        case CMD_PLAY:  s_autoHop = 0; s_autoHopAt = 0;   // 用户/App 主动切台：刹车归零
                        startIndex(s_arg); break;
        case CMD_REPLAY: startIndex(A.cur); break;
        case CMD_VOL:   A.vol = constrain(s_arg, NVOL_MIN, NVOL_MAX);
                        // 1.46.29：音量写 NVS（1~2ms），不再写 LittleFS ——
                        //   那条路要擦 4KB 块，几十毫秒里音频任务跑不动，
                        //   用户听到的就是「调完音量空一下才继续播」。
                        applyVol(); A.dirty = true; storeSaveVolFast(); break;
        // 1.43.0：CMD_MUTE 不再有动作（静音功能整个删了）。
        //   枚举值保留，免得历史数据/别的模块引用时编号错位。
        case CMD_MUTE:  break;
        case CMD_REVOL: applyVol(); A.dirty = true; break;   // 1.21.0：上限变了，重设音量
        case CMD_PINS:  break;                              // 1.45.1：真正的工作在循环顶部
        case CMD_PAUSE: if (s_active && !s_paused) { s_audio.pauseResume(); s_paused = true;
                            s_pauseAt = millis();      // 1.46.38：暂停这几分钟不算「听到了」
                            setPlayState(PS_IDLE, "已暂停");
                            healthHold(true);          // 1.17.0：暂停不算「这台没声音」
                        }
                        else if (s_active && s_paused) { s_audio.pauseResume(); s_paused = false;
                            // 把「出声起点」往后推掉暂停的时长，位置显示和断点都才对
                            if (s_pauseAt && s_playAt) {
                              const uint32_t d = millis() - s_pauseAt;
                              s_playAt += d;
                              s_posAt  += d;
                            }
                            s_pauseAt = 0;
                            setPlayState(PS_PLAY, "");
                            healthHold(false);
                        }
                        break;
        case CMD_STOP:  s_audio.stopSong(); s_active = false; s_playAt = 0;
                        healthStop();
                        setPlayState(PS_IDLE, "已停止"); break;
        case CMD_VOICE: {
          s_voiceReq = false;
          if (s_voiceOn || !s_voiceUrl[0]) break;
          if (!LittleFS.exists(s_voiceUrl)) { Serial.printf("[audio] 播报文件不在 %s\n", s_voiceUrl); break; }
          s_voiceBack = s_active && !A.fmMode;      // FM 模式下没有"节目"要接回来
          s_voiceVol  = A.vol;
          const int v0 = A.vol;
          // 1.44.0：播报直接给网络那一路的满档（21），念完 applyVol() 还原
          s_audio.setVolume((uint8_t)(v0 < VOICE_VOL_NET ? VOICE_VOL_NET : v0));
          s_audio.stopSong();
          s_paused    = false;
          s_gotAudio  = false;
          s_voiceOn   = true;
          s_voiceAt   = millis();
          if (!s_audio.connecttoFS(LittleFS, s_voiceUrl)) {
            Serial.printf("[audio] 播报打不开 %s\n", s_voiceUrl);
            finishVoice();
          } else {
            Serial.printf("[audio] 播报 %s\n", s_voiceUrl);
          }
          break;
        }
        default: break;
      }
    }

    // 2) 驱动解码
    if (s_active) s_audio.loop();


    // 2.1) 1.46.2：有声书「听到第几秒」每 30 秒记一次（断电最多丢半分钟）
    //      1.46.38：顺手把 ASC 一起记下来 —— m4a 续播全靠它
    if (s_epiB >= 0 && s_gotAudio && !s_paused && !s_noiseOn && !s_voiceOn) {
      const uint32_t pnow = millis();
      if (pnow - s_posAt > RESUME_SAVE_MS) {
        s_posAt = pnow;
        savePosNow();          // 1.46.40：内部已带码率兜底
      }
    }

    // 2.2) 1.17.0：把这一毫秒的 PCM 峰值抹成电平，顺便做「有没有声音」的判定。
    //      不在这儿判，App 那头就只能看到「播放中」，分不清「在放」和「放得出声」。
    // 播报期间的电平不算这个台的（否则「这台忽然没声音」会误判）
    // 1.46.0：噪声期间同样不算 —— 那沙沙是**我们**造的，不是这个台的声音；
    //   放进去会让健康档案把这台记成"能出声"。
    if (!s_voiceOn) {
      const uint8_t pk = healthTakePeak();
      healthLevel(s_noiseOn ? 0 : pk);
    }
    else            (void)healthTakePeak();

    // 2.5) 点播播完 → 立刻接下一集（有声书）或下一台。
    //      不能交给下面的断流重连：那要 2+4+8+16+32 秒才放弃，听书会以为机器坏了。
    if (s_active && s_eof && !s_swOn) {
      s_eof = false;
      if (s_voiceOn) finishVoice();     // 1.19.0：这一声念完了
      else           nextEpOrStation(true);
    }

    // 2.6) 播报的超时保护：6 秒还没出声 / 20 秒还没念完，都放弃
    if (s_voiceOn) {
      const uint32_t vnow = millis();
      const bool running = s_audio.isRunning();
      if ((!running && vnow - s_voiceAt > VOICE_START_MS) ||
          vnow - s_voiceAt > VOICE_MAX_MS) {
        Serial.println("[audio] 播报超时，放弃");
        finishVoice();
      }
    }

    // 2.7) 1.46.0：噪声模式 —— 文件放完接着放，每 NOISE_RETRY_MS 试一次真流
    if (s_noiseOn) {
      const uint32_t nnow = millis();
      if (s_noiseLoop) {
        s_noiseLoop = false;
        s_audio.stopSong();
        if (!s_audio.connecttoFS(LittleFS, NOISE_PATH)) s_active = false;
      }
      if (nnow - s_noiseTry > NOISE_RETRY_MS) {
        s_noiseTries++;
        // 1.46.2：有声书在沙沙里躺着没有任何意义 —— 第二轮回就去试下一集
        if (s_epiB >= 0 && s_noiseTries >= 2 && s_epiFail < 6 && A.wifiOk) {
          s_noiseTries = 0;
          Serial.println("[noise] 这一集没指望了 -> 试下一集");
          nextEpOrStation(false);
        } else {
          retryStation();
        }
      }
    }

    // 2.8) 1.46.6：静音流 —— 解出来了、但样本一直全是 0。
    //   广播台正常播出时不会有 8 秒的**数字静音**（连广告都有底噪）；
    //   连续这么长只可能是这个台此刻没在播（凌晨停播 / 检修）。
    //   判据用的是 PCM 回调置的 s_silHit，因为那儿不能直接换台。
    if (s_silHit) {
      s_silHit = false;
      if (s_active && !s_noiseOn && !s_voiceOn && !s_swOn && s_epiB < 0) {
        healthSilent();
        if (offairMark(A.cur)) {
          Serial.println("[offair] 静音流 8 秒 -> 判停播，跳下一个能听的台");
          advance(+1);
          vTaskDelay(1);
          continue;
        }
      }
    }

    // 3) 看门狗：死活判断 / 自动重连 / 跳过死链
    //    !! 播报期间整段跳过：念「没电了，关机」的那几秒不该被判成连不上
    if (s_active && !s_voiceOn && !s_swOn) {
      const uint32_t now = millis();

      // 1.46.0：点播（有声书）源常常要下几十 KB 的 moov 才出声，
      //   20 秒对它们太苛刻 —— 给 35 秒，别把"慢"当成"死"。
      // 1.46.6：到期回来的台只给 9 秒（复查用），别让用户对着死台干等 20 秒
      const uint32_t connLimit = s_probe ? OFFAIR_PROBE_MS
                               : (isLiveUrl() ? 20000u : 35000u);
      // 1.46.2：续播点了 Range 却 12 秒还没出声（服务器没给 206 / 解码不认这个起点）
      //   → 退回从头放。宁可从头，也不能卡成"正在连接…"。
      if (!s_gotAudio && !s_noiseOn && s_posBase > 0 && now - s_connectAt > RESUME_FALLBACK_MS) {
        Serial.println("[resume] 续播没出声 -> 从头放");
        s_posBase = 0;
        s_posResumed = false;
        s_audio.setRangeFrom(0);
        s_audio.stopSong();
        s_connectAt = millis();
        s_audio.connecttohost(s_url);
      } else if (!s_gotAudio && !s_noiseOn && now - s_connectAt > connLimit) {
        // 连不上 —— 不再"静悄悄跳到下一台"（用户看到的是台自己跑了）。
        // 改成留在这一台，放没信号的沙沙声，30 秒后再试一次。
        s_deadLinks++;
        healthFail("连不上");               // 1.17.0：这台记成「坏」
        // ---- 1.46.6：确认停播就不再让用户听电流声 ----
        //   第一次失败仍然按 1.46.0 的规矩补沙沙 + 30 秒后重试（那是网络抖，
        //   不该把台判死）；但**连第二回都连不上**、或者这本就是到期回来复查的那次
        //   ⇒ 这台此刻确实没在播。记进停播台账（旋钮以后不再走到它），直接换下一台。
        if (s_epiB < 0 && (s_probe || s_noiseTries > 0) && offairMark(A.cur)) {
          Serial.println("[offair] 两回都连不上 -> 判停播，跳下一个能听的台");
          advance(+1);
          vTaskDelay(1);
          continue;
        }
        // 1.46.2：有声书不走沙沙 —— 用户要的是「一直有书听」，不是听电流声。
        //   连着几集都不行（s_epiFail）才退回沙沙，免得整本书把台刷一遍。
        if (s_epiB >= 0 && s_epiFail < 3 && A.wifiOk) {
          Serial.println("[audio] 这一集连不上 -> 下一集");
          nextEpOrStation(false);
        } else {
          Serial.println("[audio] dead link -> noise");
          enterNoise();
        }
      } else if (s_gotAudio && !s_audio.isRunning() && !s_paused) {
        if (s_dropAt == 0) s_dropAt = now;
        const uint32_t wait = 2000u << (s_retry > 3 ? 3 : s_retry);
        if (now - s_dropAt > wait) {
          s_dropAt = 0;
          if (s_retry < NET_RETRY_MAX) {
            s_retry++;
            setPlayState(PS_BUFF, "信号不好，重连中…");
            s_audio.stopSong();
            s_audio.connecttohost(s_url);
          } else {
            s_deadLinks++;
            healthFail("老掉线");               // 1.17.0：能连上但撑不住，也算坏
            if (s_epiB >= 0 && s_epiFail < 3 && A.wifiOk) {   // 1.46.2：书先换下一集再谈沙沙
              Serial.println("[audio] 这一集老掉线 -> 下一集");
              s_retry = 0;
              nextEpOrStation(false);
              vTaskDelay(1);
              continue;
            }
            Serial.println("[audio] retry exhausted -> noise");
            // 1.46.0：和"连不上"一致 —— 留在这一台出声（沙沙），不自己跳走。
            //   下面这个定时重试会让它隔 30 秒再连一次，恢复了就自动接回节目。
            enterNoise();
          }
        }
      }
    }

    vTaskDelay(1);
  }
}

// ------------------------------------------------------------
//  对外接口（都是发指令，立刻返回）
// ------------------------------------------------------------
void audioBegin() {
  s_audio.setPinout(PIN_I2S_BCK, PIN_I2S_LCK, PIN_I2S_DIN);
  s_audio.setConnectionTimeout(8000, 8000);   // 8s 连接超时，别让老人干等
  // 1.21.0：开机这一句也要过电源上限（此刻 powerBegin 可能已经跑过，
  // 满电状态下上限是 90 —— 直接送 A.vol 就绕过去了）
  {
    int v = A.vol;
    const int cap = netVolCap();
    if (v > cap) v = cap;
    if (v < NVOL_MIN) v = NVOL_MIN;
    s_audio.setVolume((uint8_t)v);
  }
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);
}

void audioPlayIndex(int idx) { s_arg = idx; s_cmd = CMD_PLAY; }
void audioNext(int dir) {
  int n = g_stN;
  if (n == 0) return;
  // 这里直接算好目标索引，任务里不用再读 A.cur（避免竞态）
  // 1.46.6：跳过「现在不播」的台（App 的上一台/下一台也走这条）
  int i = (A.cur < 0) ? 0 : offairStep(A.cur, (dir >= 0) ? 1 : -1);
  s_arg = i;
  s_cmd = CMD_PLAY;
}
// 1.22.0：所有音量入口（旋钮 / App / 遥控 / /api/vol）都走这里。
// 按当前模式分流，两条路各存各的值 —— 这就是原厂 volume / netvolume 的语义。
//   为什么不改各调用点：调用点有 4 处以上，漏一个就会出现
//   「App 能调、旋钮调不动」这种查半天的问题；收口在一处最不容易漏。
// 1.45.1：诊断接口 —— 换 I2S 三脚（异步，由 audioTask 执行）
void audioSetPins(int bck, int ws, int din) {
  s_pBck = bck; s_pWs = ws; s_pDin = din; s_pinReq = true;
}

// 1.45.1：生成一段正弦 WAV 到 LittleFS（诊断用：不放网络也能验证 I2S + 功放）
//   44.1kHz / 16bit / 单声道，够长就行。文件已存在就不重写。
bool audioMakeTone(const char* path, int hz, int ms) {
  if (hz < 100) hz = 100;
  if (hz > 8000) hz = 8000;
  if (ms < 300) ms = 300;
  if (ms > 8000) ms = 8000;
  if (LittleFS.exists(path)) return true;

  const uint32_t rate = 44100;
  const uint32_t frames = (uint32_t)rate * (uint32_t)ms / 1000;
  const uint32_t dataBytes = frames * 2;

  File f = LittleFS.open(path, "w");
  if (!f) { Serial.printf("[tone] 建不了 %s\n", path); return false; }

  uint8_t h[44] = {0};
  const uint32_t chunk = 36 + dataBytes, sub1 = 16, sr = rate;
  const uint16_t fmt = 1, ch = 1, bits = 16, align = 2;
  const uint32_t brate = rate * 2;
  memcpy(h + 0,  "RIFF", 4);
  memcpy(h + 4,  &chunk, 4);
  memcpy(h + 8,  "WAVEfmt ", 8);
  memcpy(h + 16, &sub1, 4);
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &brate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
  f.write(h, 44);

  int16_t buf[512];
  uint32_t done = 0;
  double ph = 0.0;
  const double step = 2.0 * 3.14159265358979323846 * (double)hz / (double)rate;
  while (done < frames) {
    uint32_t n = frames - done; if (n > 512) n = 512;
    for (uint32_t i = 0; i < n; i++) {
      buf[i] = (int16_t)(9000.0 * sin(ph));
      ph += step;
      if (ph > 6.283185307179586) ph -= 6.283185307179586;
    }
    f.write((const uint8_t*)buf, n * 2);
    done += n;
  }
  f.close();
  Serial.printf("[tone] 生成 %s：%d Hz / %d ms / %u B\n", path, hz, ms, (unsigned)dataBytes);
  return true;
}

// 1.46.0：生成一段「沙沙」噪声 WAV（单声道 / NOISE_RATE / 16bit）。
//   "连不上"的台靠它出声：没有网络、没有解码，只是把文件循环放。
//   文件存在就跳过 —— 别每次开机都磨一遍 flash。
bool audioMakeNoise(const char* path, int ms) {
  if (ms < 1000)  ms = 1000;
  if (ms > 20000) ms = 20000;
  if (LittleFS.exists(path)) return true;

  const uint32_t rate  = NOISE_RATE;
  const uint32_t frames = (uint32_t)rate * (uint32_t)ms / 1000;
  const uint32_t dataBytes = frames * 2;

  File f = LittleFS.open(path, "w");
  if (!f) { Serial.printf("[noise] 建不了 %s\n", path); return false; }

  uint8_t h[44] = {0};
  const uint32_t chunk = 36 + dataBytes, sub1 = 16, sr = rate;
  const uint16_t fmt = 1, ch = 1, bits = 16, align = 2;
  const uint32_t brate = rate * 2;
  memcpy(h + 0,  "RIFF", 4);
  memcpy(h + 4,  &chunk, 4);
  memcpy(h + 8,  "WAVEfmt ", 8);
  memcpy(h + 16, &sub1, 4);
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &brate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
  f.write(h, 44);

  s_nzR = 20260101u;                        // 每次生成用同一条序列：可复现
  s_nzLp = 0;
  int16_t buf[512];
  uint32_t done = 0;
  while (done < frames) {
    uint32_t n = frames - done;
    if (n > 512) n = 512;
    nzFill(buf, n);
    f.write((const uint8_t*)buf, n * 2);
    done += n;
  }
  f.close();
  Serial.printf("[noise] 生成 %s：%u B（%d ms @ %u Hz）\n", path,
                (unsigned)dataBytes, ms, (unsigned)rate);
  return true;
}

void audioSetVol(int v) {
  // 1.44.0：两路量程不同（FM 6..63 / 网络 6..21），必须**分别**夹。
  //   外面不要再先夹一次共用区间 —— 那会把 FM 的 40 夹成 21。
  if (A.fmMode) {
    v = constrain(v, FVOL_MIN, FVOL_MAX);
    // 收音模式：只动芯片音量，不碰 audioI2S（那边可能正在播报/缓冲）
    A.fmVol = v;
    A.muted = false;
    fmSetVolume(v);
    // 1.34.0：A.fmVol 变了 → shownVol() 变 → needTop 重画顶栏，不必整屏刷
    storeSaveVolFast();          // 1.46.29：同上，音量走 NVS
    return;
  }
  // 1.26.0：这里必须**同步**写 A.vol。
  //   原来只发 s_cmd，A.vol 要等音频任务取到 CMD_VOL 才更新 —— 屏上「音量 X」
  //   会滞后好几帧，转旋钮时数字一顿一顿的，跟手感全没了。
  //   任务里那条 CMD_VOL 仍会写一次同样的值，重复写没有副作用（都是同一个 v）。
  v = constrain(v, NVOL_MIN, NVOL_MAX);
  A.vol = v;
  A.muted = false;
  // 1.34.0：顶栏音量胶囊由 needTop 的 (l_vol != shownVol()) 自动跟上，不必整屏刷
  s_arg = v;
  s_cmd = CMD_VOL;
}
// 1.43.0：静音功能已删。留空函数是为了不改 audio.h 和所有调用点
//   （api / net / cloud 三处），改漏一处就编不过。
void audioToggleMute()       { }
void audioPauseToggle()      { s_cmd = CMD_PAUSE; }
void audioRefreshVol()       { s_cmd = CMD_REVOL; }
void audioStop()             { s_cmd = CMD_STOP; }
void audioReplay()           { s_cmd = CMD_REPLAY; }

bool audioIsPlaying() { return s_active && s_gotAudio && s_audio.isRunning(); }

// 1.46.29：给 storeTick() 用的「现在能不能安全写 flash」。
//   判断依据全是**已经在出声**的几个条件，多余的状态不猜：
//     · 正在念播报  → 歌早就停了，写盘无所谓
//     · 暂停 / 停止  → 没声
//     · 沙沙（连不上）→ 本来就是噪声，断一下没人听得出
//     · 还没等到第一个 PCM（连接中）→ 无声
//   剩下的情况（真的在放节目）一律 false：宁可不存，也不能让用户听见断音。
bool audioWriteSafe() {
  if (A.fmMode) return true;          // FM：芯片模拟输出，I2S 那条链空着
  if (s_paused || s_voiceOn || s_noiseOn) return true;
  return !audioIsPlaying();
}

// 1.19.0：念一段本地 mp3。异步：只把路径塞给任务就返回。
// !! 已经在念 / 手上还有别的指令时返回 false（调用方自己决定要不要重试）
bool audioVoice(const char* path) {
  if (!path || !*path) return false;
  if (s_voiceOn || s_voiceReq || s_cmd != CMD_NONE) return false;
  strncpy(s_voiceUrl, path, sizeof(s_voiceUrl) - 1);
  s_voiceUrl[sizeof(s_voiceUrl) - 1] = 0;
  s_voiceReq = true;          // 先举手，再发指令：中间那一瞬也算「忙」
  s_cmd = CMD_VOICE;
  return true;
}

// 已经排进队列但还没开始念，也算「忙」——否则电量那侧会在
// 命令发出和任务取走之间的那 1ms 里以为"念完了"，直接把电断了。
bool audioVoiceBusy() { return s_voiceOn || s_voiceReq; }

// ---- 1.46.0：给 /api/diag 的读数 ----
bool     audioNoiseActive()  { return s_noiseOn; }
uint32_t audioAgcMilli()     { return (uint32_t)((int64_t)s_agcQ10 * 1000 / 1024); }
// 1.46.30：有声书"到底抬上去了没有"就看这两个数（RMS 应该落在目标附近）
uint32_t audioAgcRms()       { return s_agcRms; }
uint32_t audioAgcPeak()      { return s_agcPk; }
int      audioSilentBlocks() { return (int)(s_silBlocks > 100000u ? 100000u : s_silBlocks); }
int      audioDecodeBlocks() { return (int)(s_decBlocks > 0x7FFFFFFF ? 0x7FFFFFFF : s_decBlocks); }

int  audioRetry()     { return s_retry; }

// 1.46.10：直播流"一顿一顿"要看的就是缓冲有多大。
//   库里有公开的 inBufferFilled()/inBufferFree()，两个一加就是容量，
//   不用去改库头文件（那两处一改就踩「私有基类 + 同名成员」的解析坑）。
//   PSRAM 可用 → 约 283 KB ⇒ 64kbps 有 35 秒余量，怎么卡都够；
//   PSRAM 没拿到 → 只有内部 RAM 那一档 ⇒ 余量不到 1 秒，主循环一忙就断。
int  audioBufBytes()  { return (int)(s_audio.inBufferFilled() + s_audio.inBufferFree()); }
// ⚠️ 别用 havePSRAM()：Audio 是**私有继承** AudioBuffer，库里那套
//   「转成 AudioBuffer 再调」的写法在外部用不了，报 "is not an accessible base"。
//   直接按容量反推就够了 —— 内部 RAM 那档 48 KB，PSRAM 那档 283 KB，差一个量级。
int  audioBufPsram()  { return (audioBufBytes() > 100000) ? 1 : 0; }
int  audioEpNo()      { return (s_epiB >= 0) ? s_epiE + 1 : 0; }
int  audioPosSec()    {
  if (!s_playAt) return s_posBase > 0 ? s_posBase : 0;
  return s_posBase + (int)((millis() - s_playAt) / 1000);
}
int  audioResumed()   { return s_posResumed ? 1 : 0; }
int  audioDeadLinks() { return s_deadLinks; }
int  audioBitrate()   { return s_bitrate; }
const char* audioUrl() { return s_url; }

uint32_t audioUptimeSec() {
  if (!s_playAt || !s_gotAudio) return 0;
  return (millis() - s_playAt) / 1000;
}

// ------------------------------------------------------------
//  audioI2S 回调
// ------------------------------------------------------------
void audio_info(const char* info) {
  Serial.printf("[i2s] %s\n", info);
  if (strstr(info, "Connect") || strstr(info, "connect")) setPlayState(PS_LINK, "正在连接…");
}

void audio_showstation(const char* info) {
  if (info && *info && A.cur >= 0 && A.cur < g_stN) {
    // 不覆盖用户起的名字，只在没名字时补一个
    if (strncmp(g_st[A.cur].name, "未命名", 9) == 0 || g_st[A.cur].name[0] == 0) {
      strncpy(g_st[A.cur].name, info, sizeof(g_st[0].name) - 1);
      A.dirty = true;
    }
  }
  s_gotAudio = true;
  s_retry = 0;
  healthMeta();                        // 1.17.0：流是活的（元数据下来了）
  s_epiFail = 0;                       // 这一集通了，别再拿旧失败次数去跳集
  if (!s_playAt) s_playAt = millis();
  setPlayState(PS_PLAY, "");
}

void audio_showstreamtitle(const char* info) {
  s_gotAudio = true;
  s_retry = 0;
  healthMeta();
  if (!s_playAt) s_playAt = millis();
  if (A.ps != PS_PLAY) setPlayState(PS_PLAY, "");
}

void audio_eof_stream(const char* info) {
  Serial.println("[audio] eof stream");
  if (s_voiceOn) { s_eof = true; return; }          // 1.19.0：播报念完了
  if (s_noiseOn) { s_noiseLoop = true; return; }    // 1.46.0：噪声文件放完 → 接着放
  if (!isLiveUrl()) s_eof = true;      // 直播流 EOF 当断流处理，别跳台
}

void audio_eof_mp3(const char* info) {
  Serial.println("[audio] eof mp3");
  // 播报是本地短文件，结尾一定走这里；s_url 还留着上一个台的地址
  // （可能是 /live/），所以不能靠 isLiveUrl() 判断，得先看 s_voiceOn
  if (s_voiceOn) { s_eof = true; return; }
  if (s_noiseOn) { s_noiseLoop = true; return; }   // 1.46.0：WAV 也是走这条路
  if (!isLiveUrl()) s_eof = true;
}

// 库回调给的是 "Bitrate: 128 kbps" 这种文本，抠数字出来
void audio_bitrate(const char* info) {
  Serial.printf("[audio] bitrate %s\n", info);
  if (!info) return;
  int v = 0;
  for (const char* p = info; *p; p++) {
    if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0');
    else if (v > 0) break;
  }
  if (v > 0) s_bitrate = v;
}

// 小工具：软限幅。硬切会在波形上留下"啪"的边缘，斜率压一点听不出来。
// 1.46.18：平滑软限幅。
//   老版本是 30000 之后 1/6 斜率、32700 硬截 —— 两个拐点都会在波形上留下刷角，
//   听感就是人声发毛、音乐发糊（"不清"）。这里改成一条连续可导的曲线：
//     |v| <= 26000           → 直通
//     |v| >  26000           → 26000 + R·x/(x+R)，x = |v|-26000，渐近 34000
//   再兜一层 32600 防止越界。没有硬拐点，削波只表现为轻微压缩。
#define LIM_K 26000
#define LIM_R 8000
static inline int16_t nzLim(int32_t v) {
  int32_t a = (v < 0) ? -v : v;
  if (a <= LIM_K) return (int16_t)v;
  int32_t x = a - LIM_K;
  int32_t y = LIM_K + (LIM_R * x) / (x + LIM_R);
  if (y > 32600) y = 32600;
  return (int16_t)((v < 0) ? -y : y);
}

// 往一块 PCM 里填"没信号"的沙沙声。
//   高频为主（低通取慢包络再减掉 = 高通），听起来是调频空频道的嘶声；
//   再以 1/2048 的概率撒一个爆点 —— 那就是"滋啦"。
static void nzFill(int16_t* b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    s_nzR = s_nzR * 1664525u + 1013904223u;
    int32_t w = (int32_t)(s_nzR >> 17) - 16384;      // ±16384 白噪
    s_nzLp += (w - s_nzLp) >> 3;                     // 慢包络
    int32_t y = (w - s_nzLp) * NOISE_LEVEL / 12000;  // 高通后按 NOISE_LEVEL 定标
    if ((s_nzR & 0x7FFu) == 0x123u) y += (int32_t)(s_nzR & 0x1FFFu) - 4096;
    b[i] = nzLim(y);
  }
}

// 1.17.0：库在「解码完、写 I2S 之前」回调这里（Audio.h 里的 weak 钩子）。
// 1.46.0 起这里还干两件事：① 静态增益比（AGC）② 静音流补噪声。
//   ⚠️ 顺序很关键：**先量原始电平、再改样本**。
//     反过来 health 会把我们自己造的噪声当成"这个台有声音"，
//     于是"没声音的台"永远判不出来。
void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
  if (continueI2S) *continueI2S = true;
  if (!buff || !len) return;

  // 1) 先交给健康档案：它取的是**改之前**的 PCM
  //    1.46.0：放本地噪声文件时不要喂给它 —— 那是我们自己造的声，
  //    会让"这个台没声音"永远判不出来。
  if (!s_noiseOn) healthSamples(buff, len);

  const uint8_t ch = (s_audio.getChannels() == 1) ? 1 : 2;
  uint32_t n = (uint32_t)len * ch;
  if (n > 4096) n = 4096;                 // 每块最多 4096 个 int16，够判断也够快
  if (n == 0) return;

  uint32_t pk = 0;
  for (uint32_t i = 0; i < n; i++) {
    int32_t v = buff[i] < 0 ? -buff[i] : buff[i];
    if ((uint32_t)v > pk) pk = (uint32_t)v;
  }
  s_agcPk = pk;
  if (s_decBlocks < 0x7FFFFFFFuL) s_decBlocks++;

  // 1.46.2：**「收到音频」的真判据是解出了非零 PCM**。
  //   1.46.0 只在 ICY 元数据回调（audio_showstation / showstreamtitle）里置
  //   s_gotAudio，而喜马/蜻蜓的**直链 mp3 / m4a 没有 ICY 头** ⇒ s_gotAudio 永远是
  //   false ⇒ 看门狗 35 秒后判「死链」→ 停流、改放本地沙沙。
  //   用户听到的正是「刚听一会儿就变沙沙」，而且怎么换台回来还是沙沙。
  if (pk && !s_noiseOn && !s_voiceOn && !s_gotAudio) {
    s_gotAudio = true;
    s_retry    = 0;
    s_epiFail  = 0;
    if (!s_playAt) s_playAt = millis();
    healthMeta();
    setPlayState(PS_PLAY, "");
    offairOk(A.cur);            // 1.46.6：出声了 → 从停播台账里放回（有记录才动）
  }

  // 2) 静音计数：连着 NOISE_SIL_BLOCKS 块都是 0 样本 = 这个流"活着但不出声"
  if (pk < AGC_SILENT_PK) { if (s_silBlocks < 100000u) s_silBlocks++; }
  else                    { s_silBlocks = 0; }

  // 1.46.6：静音到「停播判据」只置标志，不在这儿换台 ——
  //   这一段是 s_audio.loop() 内部的解码回调，直接 stopSong/connecttohost
  //   会把库的状态机打断（1.16.0 踩过同款）。换台交给任务循环的 s_silHit。
  //   用 >= 而不是 ==：万一越过阈值那一块恰好赶上别的分支（播报/扫描）没置位，
  //   用 == 就永久错过了（s_silBlocks 只增不减），只能等下一次换台。
  if (s_silBlocks >= OFFAIR_SIL_BLOCKS && !s_silHit &&
      !s_noiseOn && !s_voiceOn && !s_swOn)
    s_silHit = true;

  // 3) 自动增益：只在有声音的块上收敛
  //    静音块不动增益 —— 否则空频道的底噪会把增益推到上限，
  //    真声音一回来就是"砰"的一声。
#if AGC_ON
  // 1.46.30：峰值包络。原来拿"这一块的峰值"算上限，人声一句里轻重差得远：
  //   轻的那块把增益放上去，重的那块立刻撞顶 —— 于是既不敢抬、又忽大忽小。
  //   改成"最近约 4 秒里最响的那一下"，增益才稳得住。
  {
    const uint32_t env = (uint32_t)((float)s_pkEnv * AGC_PK_ENV_DECAY);
    s_pkEnv = (pk > env) ? pk : env;
  }
  if (pk >= AGC_SILENT_PK && !s_noiseOn && !s_voiceOn && !s_swOn) {
    // 1.46.30：从噪声里回来（增益被归过 1.0）→ 按缓存接上，不要从 1.0 重爬
    if (s_agcRst) {
      s_agcRst = false;
      if (A.cur >= 0 && A.cur < AGC_GAIN_CACHE_N && s_gainCache[A.cur] > 0)
        s_agcQ10 = s_gainCache[A.cur];
    }
    uint64_t sq = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i += 4) {
      int32_t v = buff[i];
      sq += (uint64_t)((int64_t)v * v);
      cnt++;
    }
    if (cnt) {
      const float rms = sqrtf((float)((double)sq / (double)cnt));
      s_agcRms = (uint32_t)rms;
      if (rms > 1.0f) {
        // 1.46.17：有声书/点播（带分集）与人声源，目标更高、上限更大；
        //   直播电台保持 1.46.0 的调校，免得把电台底噪一起抬起来。
        const bool  isBook = (s_epiB >= 0);
        const float tgt    = isBook ? (float)AGC_TARGET_RMS_BOOK : (float)AGC_TARGET_RMS;
        const float gmax   = isBook ? AGC_MAX_GAIN_BOOK : AGC_MAX_GAIN;
        const float ceil   = isBook ? AGC_PEAK_CEIL_BOOK : AGC_PEAK_CEIL;
        float want = tgt / rms;
        if (want > gmax) want = gmax;
        if (want < AGC_MIN_GAIN) want = AGC_MIN_GAIN;
        // 1.46.18 的峰值保护，1.46.30 改口径：拿"近期最响"算，不拿单块峰值。
        //   单块峰值一抖，增益跟着抖，听感就是"一会大一会小"。
        const uint32_t pe = s_pkEnv ? s_pkEnv : pk;
        if (pe > 0) {
          const float pkCap = ceil / (float)pe;
          if (want > pkCap) want = pkCap;
          if (want < 0.05f) want = 0.05f;
        }
        float cur = (float)s_agcQ10 / 1024.0f;
        // 1.46.30：轻的块不许加增益。换气、留白、底噪都会让 RMS 变小，
        //   照 RMS 加就会把增益推得老高，下一句人声直接撞限幅（"抽气"）。
        if (want > cur && pk < AGC_UP_GATE_PK) want = cur;
        // 乘性步进：恒定 dB/秒（加法步长时从 1.0 到 9.5 倍要 20 多秒）
        if (want > cur) {
          const float nxt = cur * (1.0f + AGC_STEP_UP);
          cur = (nxt > want) ? want : nxt;
        } else {
          const float nxt = cur * (1.0f - AGC_STEP_DN);
          cur = (nxt < want) ? want : nxt;
        }
        // 1.46.30：换台后的头一块，量到真电平就直接接到目标（3 倍以内）。
        //   放在步进之后 —— 否则刚接上 3 倍又被加了一步，越界一点点。
        //   再高不敢直跳：万一这一块恰好是轻的留白，下一句会炸。
        if (s_agcFresh && pk >= AGC_UP_GATE_PK) {
          s_agcFresh = false;
          const float f = (want > AGC_FAST_GAIN) ? AGC_FAST_GAIN : want;
          if (f > cur) cur = f;
        }
        s_agcQ10 = (int32_t)(cur * 1024.0f + 0.5f);
        // 学到的增益记到这台名下（只放 RAM）：下次换回来直接接上
        if (A.cur >= 0 && A.cur < AGC_GAIN_CACHE_N) s_gainCache[A.cur] = (int16_t)s_agcQ10;
      }
    }
  }
#endif

  // 4) 出声：静音流 → 填噪声；否则按增益缩放
  if (s_silBlocks >= NOISE_SIL_BLOCKS) {
    s_agcQ10 = 1024;                      // 噪声自带电平，别再乘一次增益
    s_agcRst = true;                      // 1.46.30：真声回来时按缓存接上
    nzFill(buff, n);
  } else if (s_agcQ10 != 1024) {
    for (uint32_t i = 0; i < n; i++) {
      buff[i] = nzLim(((int32_t)buff[i] * s_agcQ10) >> 10);
    }
  }
}

void audio_id3data(const char* info) { (void)info; }
void audio_icyurl(const char* info)  { (void)info; }
