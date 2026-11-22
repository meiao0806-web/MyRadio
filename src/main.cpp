// ============================================================
//  main.cpp —— 极简网络收音机
//
//  适老化原则：
//    1. 上电就出声，不需要任何操作
//    2. 老人只需要会一件事：转旋钮
//    3. 屏上不出现任何"设置""配对""配置"字样
//    4. 一切设置都在手机 App 里（App 打开即自动配对，无需配对码）
// ============================================================
#include "config.h"
#include "state.h"
#include "store.h"
#include "history.h"
#include "audio.h"
#include "input.h"
#include "evlog.h"      // 1.46.39：事件日志（断网那段时间也要留下证据）
#include "ui.h"
#include "net.h"
#include "fm.h"
#include "album.h"
#include "epis.h"
#include "resume.h"    // 1.46.2：有声书听到第几秒（断点续播）
#include "pairpin.h"   // 1.46.5：4 位手机连接码（App 输码即连）
#include "battery.h"   // 1.43.0：心跳日志要印电量
#include "cloud.h"
#include "health.h"
#include "offair.h"    // 1.46.6：停播台自动跳过（台账）
#include "battery.h"
#include "voice.h"
#include "power.h"          // 1.21.0：电源/电流保护
#include <esp_heap_caps.h>  // 1.45.1：台表放进 PSRAM

#if HWTEST
void selftestRun();            // 1.21.1：硬件自检（-DHWTEST=1 时才编进来）
#endif

// 全局状态定义（声明在 state.h）
AppState A;
// 1.45.1：台表指针，内存由 setup() 里的 g_st 分配块给出（优先 PSRAM）
Station* g_st = nullptr;
int      g_stN = 0;

static uint32_t s_bootAt = 0;
static bool     s_played = false;

// ---- 网络 / FM 自动切换 ----
static uint32_t s_netLostAt = 0;     // WiFi 开始断开的时刻，0 表示没断
static int      s_fmVol = -1;

// 进收音：停掉网络流，把芯片音量对齐，开声音
static void enterFm() {
  // 1.46.37：进收音前先确保「搜台状态」是干净的（见 exitFm 里的说明）。
  //   已经有台表的话，abort 不会清掉它（只清「正在搜」这个标志）。
  if (fmScanBusy()) fmScanAbort();
  A.fmMode = true;
  s_played = false;                  // 回到网络后重新起播
  audioStop();
  // 1.21.0：FM 也走电源上限（过热/满电时压下来的那个值）
  {
    // 1.22.0：进来用的是**收音自己**那一套音量，不是网络那个
    // 1.44.0：电源上限是百分比，按 FM 的量程（63）折算
    int v = A.fmVol;
    const int cap = FVOL_MAX * powerVolCapPct() / 100;
    if (v > cap) v = cap;
    if (v < FVOL_MIN) v = FVOL_MIN;
    fmSetVolume(v);
    s_fmVol = v;
  }
  // 1.43.0：静音已删 —— 进收音一律放声
  fmMute(false);
  A.fmFreq = fmFreq();               // 1.10.0 起是 kHz（一个数覆盖长波/中波/调频/短波）
  A.fmRssi = fmRssi();
  A.ps = PS_PLAY;
  A.dirty = true;

  // ---- 1.46.59：进收音**不再自动搜台**，也不再跳到"台表第一个" ----
  //
  //   用户原话：「去掉自动搜台改为可以手动搜台」。所以进收音就是：
  //     ① 芯片留在**上次听的那个频率**（fmBegin 已经按 A.fmFreq 调好了）
  //     ② 台表只读进来给 App 的"上一台/下一台"用，不参与开机定位
  //     ③ 想搜台就自己在 App 里点一次（`/api/fmscan`），搜完落到最强台
  //   这样既没有"进收音黑屏十几秒、屏上一直 0%"，也不会被残废的旧台表
  //   （历史上搜台失败留下的 1 个台）钉死在那一个台上。
  fmScanLoad();
  Serial.printf("[fm] 进收音 %.1fMHz（手动调频），台表 %d 个仅给 App 翻台用\n",
                A.fmFreq / 1000.0f, fmScanCount());
  evlog("fm", "进收音 %.1fMHz（手动），台表%d个", A.fmFreq / 1000.0f, fmScanCount());

  #if PIN_FM_AMP_MUTE >= 0
    pinMode(PIN_FM_AMP_MUTE, OUTPUT);
    digitalWrite(PIN_FM_AMP_MUTE, LOW);    // 功放静音脚：低=放声
  #endif
  // 1.40.1：记住"现在在收音"，下次开机第一帧就能画对
  if (!A.fmModeBoot) { A.fmModeBoot = true; storeSaveDebounced(); }
  Serial.printf("[main] 没网 → 切到芯片收音（%s %d kHz）\n", radioBandCode(A.fmFreq), A.fmFreq);
  evlog("fm", "进收音 %s %.1fMHz 台表%d个 音量%d",
        radioBandCode(A.fmFreq), A.fmFreq / 1000.0f, fmScanCount(), s_fmVol);
}

// 回网络：先把 FM 静音，免得两个声音叠在一起
static void exitFm() {
  A.fmMode = false;
  // 1.46.37（P0）：退出收音时必须中断正在跑的搜台。
  //   s_scanBusy 只有 scanTick() 能清，而 scanTick() 只在 fmLoop() 里调、
  //   fmLoop() 只在 A.fmMode 时调 —— 所以「搜到一半网络回来了」会导致
  //   s_scanBusy 永远卡在 true，下次再进收音 fmScanStart() 一句就早退：
  //   一个台也不搜、也不 tune，芯片停在原地 → 用户报的
  //   「收频段搜不到，没有声音噪音也没有」。
  fmScanAbort();
  // 这里是**芯片级**静音（不是用户那个开关）：回网络时要让 SI4732 闭嘴，
  // 否则 FM 的声音会和网络流一起从功放出来。1.43.0 删的是用户静音，这条保留。
  fmMute(true);
  // ---- 1.40.2 修 P0：这里**不能**再拉高 IO3 ----
  //
  //  原来写的是 digitalWrite(HIGH)，注释说「FM 先闭嘴，交给网络流」。
  //  问题：这个静音电路在**放大器输入端**（原厂 README 原话 "mute the audio on
  //  amplifier input"），拉高会连网络那一路一起压掉 —— 从收音切回网络就永久静音。
  //
  //  而且完全没必要：FM 的静音上一句 fmMute(true) 已经用芯片寄存器
  //  PROP_RX_HARD_MUTE 做掉了，芯片自己就把音频掐了，不需要外部电路再补一刀。
  //
  //  所以这里明确保持「放声」，让网络能出声。
  #if PIN_FM_AMP_MUTE >= 0
    digitalWrite(PIN_FM_AMP_MUTE, LOW);    // 保持放声（FM 已由芯片寄存器静音）
  #endif
  A.ps = PS_IDLE;
  A.dirty = true;
  // 1.40.1：记住"现在在网络"，下次开机不抢先画收音界面
  if (A.fmModeBoot) { A.fmModeBoot = false; storeSaveDebounced(); }
  Serial.println("[main] 网络恢复 → 切回网络电台");
  evlog("fm", "退回收音 → 回网络台 #%d", A.cur);
  if (A.cur >= 0) audioPlayIndex(A.cur);
}

// ---- 1.46.43：诊断用的「临时进 FM」窗口 ----
//   /api/fmtest 会打开它：进收音、跑 N 秒、到点无条件退回网络。
//   这样「搜台慢/搜不到/没声音」这类问题在有网的时候也能复现并远程观察，
//   不用让用户拔网线（拔了就没 HTTP，什么都看不到）。
static uint32_t s_fmTestUntil = 0;
static int      s_fmTestBackTo = -1;

void fmTestBegin(uint32_t ms) {
  if (ms == 0) ms = 10000;
  if (ms > 30000) ms = 30000;          // 最多占 30 秒，绝不长锁
  if (A.fmMode) {                      // 已经在收音里：顺延窗口就好
    s_fmTestUntil = millis() + ms;
    return;
  }
  s_fmTestBackTo = A.cur;
  s_fmTestUntil  = millis() + ms;
  evlog("fm", "诊断：临时进 FM，%lu ms 后自动回网络", (unsigned long)ms);
  enterFm();
}

bool fmTesting() { return s_fmTestUntil != 0; }

// 没网自动收台；网一回来就交还给网络电台。没接 SI4732 时整段不生效。
static void fmModeTick() {
  if (!fmReady()) return;

  const uint32_t now = millis();
  // 1.46.43：诊断窗口到点 → 无条件退回网络（不管 WiFi 现在什么状态）
  if (s_fmTestUntil && (int32_t)(now - s_fmTestUntil) >= 0) {
    s_fmTestUntil = 0;
    evlog("fm", "诊断窗口结束 → 回网络");
    if (A.fmMode) exitFm();
    if (s_fmTestBackTo >= 0 && s_fmTestBackTo != A.cur) {
      audioPlayIndex(s_fmTestBackTo);
      s_fmTestBackTo = -1;
    }
    return;
  }

  // 1.46.39：WiFi 状态一变就记一条 —— 「没回归网络广播」这种问题全靠它定位
  static int8_t s_lastOk = -1;
  if ((int8_t)(A.wifiOk ? 1 : 0) != s_lastOk) {
    s_lastOk = A.wifiOk ? 1 : 0;
    evlog("net", "WiFi %s", A.wifiOk ? "连上了" : "断了");
  }

  if (!A.wifiOk) {
    if (!s_netLostAt) s_netLostAt = now;
    // 1.34.3：分两档。开机就没网 → 3 秒直接进收音；
    //   1.46.37：连上过又掉线 → 6 秒（原来 20 秒，用户说「没立刻切」）。
    const uint32_t wait = netEverUp() ? FM_NET_LOST_MS_RE : FM_NET_LOST_MS;
    if (!A.fmMode && now - s_netLostAt > wait) {
      evlog("fm", "断网 %lu ms，超过 %lu ms → 进收音", (unsigned long)(now - s_netLostAt),
            (unsigned long)wait);
      enterFm();
    }
  } else {
    s_netLostAt = 0;
    if (A.fmMode && !s_fmTestUntil) {          // 1.46.43：诊断窗口内别抢
      
      // 1.46.39：这里以前只有一行 exitFm()。现在补一道「退不出去」的自愈：
      //   连着两次 tick 都还在收音模式，说明 exitFm 没起作用（芯片静音没解开 /
      //   音频任务卡在旧连接），再退一次并记日志。
      static uint8_t s_stuck = 0;
      exitFm();
      if (A.fmMode) {                     // exitFm 应当把它置 false
        if (++s_stuck >= 2) { s_stuck = 0; A.fmMode = false; evlog("fm", "强制退收音（自愈）"); }
      } else s_stuck = 0;
    }
  }

  if (!A.fmMode) return;

  // 音量/静音跟着 App 和旋钮走；1.21.0 起还要跟着电源上限走
  {
    int v = A.fmVol;
    const int cap = FVOL_MAX * powerVolCapPct() / 100;
    if (v > cap) v = cap;
    if (v < FVOL_MIN) v = FVOL_MIN;
    if (v != s_fmVol) { fmSetVolume(v); s_fmVol = v; }
  }
  fmLoop();
  A.fmFreq = fmFreq();
  A.fmRssi = fmRssi();
}

void setup() {
  // ---- 1.43.0：背光先锁成「灭」，再给屏那一路供电 ----
  //
  //  顺序不能反：
  //    ① digitalWrite 只写输出**锁存器**（此时引脚还是输入，不驱动电平）
  //    ② pinMode(OUTPUT) 立刻按锁存器输出 —— 一步到位就是灭的
  //    ③ 最后才拉高 IO15 给屏幕/收音机供电
  //  这样屏一上电，背光脚已经是确定电平，中间不存在「浮空被当成高电平」的窗口
  //  （1.38.1 是供电之后才按灭，仍有几微秒的窗口）。
  digitalWrite(PIN_TFT_BL, BL_ON_LEVEL == HIGH ? LOW : HIGH);
  pinMode(PIN_TFT_BL, OUTPUT);

  // !! ATS MINI：IO15 是整板外设电源的自锁脚，必须最先拉高，
  //    否则屏幕/收音机那一路根本没电（原厂固件也是第一句就干这个）。
  #ifdef BOARD_ATSMINI
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
  #endif


  Serial.begin(115200);
  delay(150);
  Serial.printf("\n=== %s %s ===\n", FW_NAME, FW_VER);

  // ---- 1.45.1：台表搬进 PSRAM ----
  //   380 台 x 360 B = 136,800 B，原来整块躺在内部 DRAM 里，
  //   内部堆只剩 ~30 KB；而 https 音频（CCTV 伴音、全部有声书）做 TLS 握手
  //   要 30~40 KB 连续内存 ==> 握手失败，socket 根本连不上。
  //   现象就是「http 台能放、https 台一个都不响」，日志里 `Request ... failed!`。
  //   PSRAM 有 8 MB 闲着：board json 声明的是无 PSRAM 的开发板，所以
  //   psramFound() 一直是 0，但 IDF 开机已经把 OPI PSRAM 初始化好了，
  //   heap_caps_* 能直接拿到那 8 MB。
  {
    if (!psramFound()) psramInit();
    const size_t need = sizeof(Station) * MAX_STATIONS;
    g_st = (Station*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    const char* where = "PSRAM";
    if (!g_st) { g_st = (Station*)malloc(need); where = "内部RAM"; }
    Serial.printf("[mem] 台表 %u B -> %s（%s），内部空闲 %u KB / PSRAM 空闲 %u KB\n",
                  (unsigned)need, where, g_st ? "成功" : "失败",
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }

#if PIN_AMP_EN >= 0 && AMP_EN_ENABLE
  // 功放使能。原厂源码 #define AMP_EN 10，但那台是纯 FM 版；
  // 这块 v2.2 改过板（加了 PCM5102），IO10 到底是不是功放使能**未经确认**。
  // 上一次在 Serial 之前拉高它，结果屏幕黑掉 —— 所以先关掉（AMP_EN_ENABLE=0），
  // 等屏幕确认稳定后再单独试。
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, AMP_EN_LEVEL);
  Serial.printf("[amp] 功放使能 IO%d = %s\n", PIN_AMP_EN, AMP_EN_LEVEL ? "HIGH" : "LOW");
#endif

  // ---- 1.40.2：外部静音电路兜底放开 ----
  //
  //  功放有两条「开关」串在一起，两条都得打开才有声音：
  //    ① IO10 AMP_EN      —— 功放使能（上面那个块）
  //    ② IO3  AUDIO_MUTE  —— 放大器输入端的静音电路，**低电平才是放声**
  //
  //  为什么在这里再做一次（fmBegin 里已经设过）：fmBegin() 有几个提前 return 的
  //  失败分支（I2C 没应答、POWER_UP 没 CTS）。那些分支一旦走到，后面那句放开
  //  IO3 就永远不执行 —— 于是「收音芯片坏了」会连带「网络广播也没声音」，
  //  两个完全无关的功能被一个浮空脚拖下水，排查时能把人逼疯。
  //
  //  IO3 在 ESP32-S3 上是 JTAG 的 strap 脚，上电有内部弱上拉 ⇒ 默认读出来是 HIGH
  //  ⇒ 静音电路压着。所以这一句不是可有可无的保险，是**开机必做**。
  //
  //  放在 Serial 之前也好：万一以后又要查出声问题，这一行日志在整段启动信息最前面。
  #if PIN_FM_AMP_MUTE >= 0
    pinMode(PIN_FM_AMP_MUTE, OUTPUT);
    digitalWrite(PIN_FM_AMP_MUTE, LOW);     // 低 = 放声
  #endif

#if HWTEST
  // 自检在其它初始化之前跑：它只依赖 I2S 和屏幕，不碰网络/存储
  selftestRun();
#endif

  memset(&A, 0, sizeof(A));
  A.cur = -1;
  A.preview = -1;
  A.vol = NVOL_DEFAULT;            // 1.44.0：原厂 netvolume 默认 18
  A.fmVol = FVOL_DEFAULT;            // 1.22.0：收音那一路是独立的一套
  A.squelch = SQUELCH_DEFAULT;       // 1.22.0：静噪门限
  A.halfstep = HALFSTEP_DEFAULT;     // 1.22.0：半步进
  A.bright = 100;
  A.brightMax = 100;
  A.scrOffMin = SCR_OFF_MS / 60000;    // 出厂默认跟 config.h 保持一致
  A.screenOn = true;
  A.ps = PS_IDLE;
  A.rssi = -100;
  A.lastAct = millis();

  uiBegin();
  uiSplash("收音机", "正在启动…");
  // 1.38.1：首帧已经推到屏上了，现在才真正点亮（开显示 + 背光渐亮）。
  //   这一步之后用户看到的第一眼就是完整画面，中间没有白屏。
  uiShow();
  // 1.34.1 修正 —— 这一句是必须的，别删：
  //   A 被 memset 清过，A.dirty 初始为 false，而 uiBegin() 只画了一层底色 + 欢迎字。
  //   1.34.0 我把各处「多余的 A.dirty」去掉后，首帧就没人触发整屏绘制了 ——
  //   屏幕停在纯底色上，看着就是「黑掉/没内容」。
  //   增量检测（needTop/needFreq）只负责局部重画，管不了「从无到有」的第一帧。
  A.dirty = true;

  storeBegin();
  storeSeedIfEmpty();        // 首次开机：把固件里的 1038 台灌进 LittleFS
  storeLoad();               // 整表读进内存（1.15.0 起没有分组）
  storeLoadCfg();            // 覆盖音量/亮度/当前台
  storeLoadVolNvs();         // 1.46.29：音量以 NVS 那份为准（每次调音量都立刻进 NVS）
  albumBegin();              // 1.7.0：每部有声书上次听到第几集
  episBegin();               // 1.9.0：「一部书 = 一个台」的集表与进度
  resumeBegin();             // 1.46.2：每部书听到第几秒（断点续播）
  historyBegin();            // 收听记录（要在 cur/group 定下来之后）
  healthBegin();             // 1.17.0：每台的体检档案（/health.bin）
  offairBegin();             // 1.46.6：谁现在不播（换台时跳过它们）
  batteryBegin();            // 1.19.0：电量采样（要在 LittleFS 挂上之后）
  powerBegin();              // 1.21.0：电源保护（温度基线 / 充电使能脚接管）
  // 1.43.0：配置和电源上限都就绪了 —— 把开机亮度一次锁到最终值。
  //   否则开机 1.5 秒后第一帧 uiLoop 才把背光从 100 砸到用户设的值（像"闪一下"）。
  uiBrightSettle();
  voiceBegin();              // 1.19.0：把两句播报写进 LittleFS（首次开机才写）
  A.count = g_stN;
  if (A.cur < 0 || A.cur >= g_stN) A.cur = g_stN ? 0 : -1;
  // 1.46.6：上次听的那个台现在是「停播」状态 → 开机直接落到最近一个能听的台。
  //   否则老人一开机就是一片沙沙，还得自己转走。
  if (A.cur >= 0 && offairSkip(A.cur)) {
    const int alt = offairFirstAudible(A.cur);
    if (alt >= 0 && alt != A.cur) {
      Serial.printf("[offair] 上次的 #%d 停播 → 开机改播 #%d\n", A.cur, alt);
      A.cur = alt;
    }
  }

  inputBegin();
  netBegin();
  pinBegin();                  // 1.46.5：读 /pin.txt（出厂 1234）
  cloudBegin();              // 1.8.0：跨网远程控制（WiFi 连上后自己往外连）
  audioBegin();

  // 收音芯片（ATS MINI 的 SI4732）。没接/接错就返回 false，FM 功能整块关闭，
  // 网络收音机照旧，不影响任何现有行为。
  if (!fmBegin()) Serial.println("[main] FM 不可用，只走网络电台");
  else {
    fmMute(true);                     // 先静音，等真正没网了再放开
    // 1.38.0：开机就把上次搜的台表读进来 —— 这样没网时 3 秒一切过去就有台，
    //   而不是「切进去是一片空白，还得等搜台」。
    fmScanLoad();
    Serial.printf("[main] 收音台表 %d 个\n", fmScanCount());
  }

  // 1.46.39：日志缓冲就位（后面所有插桩都往这里写）。
  //   必须放在 fmBegin/fmScanLoad 之后 —— 那之后 fmReady()/fmScanCount() 才是真值。
  evlogBoot(FW_VER);
  evlog("boot", "FM=%s 台表%d个 网络音量%d 收音音量%d 静噪%d",
        fmReady() ? "就绪" : "不可用", fmScanCount(), A.vol, A.fmVol, A.squelch);

  s_bootAt = millis();
  Serial.printf("[main] 单列表 %d 台 / %d 个标签, cur=%d\n",
                g_stN, storeTagCount(), A.cur);

  // ---- 1.40.1：开机第一帧就用对的模式，别再「先画网络广播再闪一下」 ----
  //
  //  原来：setup 结束时 A.fmMode 还是 false → 开机画面之后第一帧画「网络广播」；
  //  loop 里要等 BOOT_AUTOPLAY_MS(3s) + FM_NET_LOST_MS(3s) 共 6 秒才切收音。
  //  用户看到的就是闪一下。
  //
  //  现在：上次是收音、这次开机也还没连上网 → 立刻按收音模式起步。
  //  （wifiOk 在第一帧时一定还是 false —— WiFi 还在连；真连上之后
  //    fmModeTick 会 exitFm 切回网络，和以前一模一样。）
  if (fmReady() && A.fmModeBoot && !A.wifiOk) {
    enterFm();
    Serial.println("[main] 上次是收音 → 开机直接进收音界面（不闪网络广播）");
  }
}

void loop() {
  inputLoop();
  netLoop();
  pinLoop();                   // 1.46.5：有人广播问「谁是 1234」就答一句
  batteryTick();                 // 1.19.0：采样 + 低电/没电判定（含播报与断电）
  powerTick();                   // 1.21.0：温度降载 / 满电降载 / 真停充

  // ---------- 熄屏 / 变暗（1.4.4 起：亮度上限与熄屏延时可在 App 里设置） ----------
  const uint32_t idle = millis() - A.lastAct;
  if (idle < 3000) A.bright = A.brightMax;
  else if (SCR_SAVER_MS && idle > SCR_SAVER_MS) {
    const int saver = A.brightMax * 45 / 100;      // 省电时压到上限的 45%
    if (A.bright > saver) A.bright = saver;
  }
  const uint32_t offMs = (uint32_t)A.scrOffMin * 60000UL;
  if (offMs && idle > offMs && A.screenOn) A.screenOn = false;

  // ---------- 开机画面停留一会儿，然后直接进收音界面 ----------
  if (millis() - s_bootAt < BOOT_AUTOPLAY_MS) {
    // 1.40.1：开机这 3 秒里也要跑模式判定。
    //
    //   原来这里只 delay，模式判定要等 3 秒后才第一次跑，于是"没网 → 进收音"
    //   被推到了开机 6 秒后，中间那几秒屏上一直写着「网络广播」，最后再跳一下。
    //   现在这 3 秒里就开始计时，正好在 BOOT 结束、第一帧上屏之前完成切换。
    //   （enterFm 里会 audioStop/播报/调芯片，audioBegin 和 fmBegin 都已经跑过，安全。）
    fmModeTick();
    delay(1);
    return;
  }

  // ---------- 1.45.2：扫描期间，屏上只画这一页 ----------
  //   1.45.1 是「先 uiLoop() 画主界面，再把扫描页盖上去」，两套绘制轮流上屏，
  //   看上去就是主界面和扫描页来回闪 —— 用户反馈的「来回闪动」正是这个。
  //   现在扫描时干脆不跑 uiLoop，等退出扫描再整屏重画一次。
  static bool s_swPrev = false;
  const bool sweeping = audioSweepActive();

  // 1.46.53：连不上 WiFi 的提示页。
  //   ⚠️ **每帧都要调** —— 它自己推进「60 秒才开热点」和「提示 15 秒后收起」的计时。
  //   而且不能在这里 return：后面还有 cloudLoop / storeTick / albumTick ……
  //   一 return 那些全停（数据不落盘、跨网命令不执行）。
  const bool netHelp = netAutoSetupTick();

  if (sweeping) {
    static int s_swDrawn = -2;
    const int idx = audioSweepIndex();
    if (idx != s_swDrawn) {        // 只在「组号变了」时画，别每帧都刷
      s_swDrawn = idx;
      if (idx >= 0) {
        char l2[40], l3[40], l4[40];
        snprintf(l2, sizeof(l2), "第 %d 组 / 共 %d 组", idx + 1, audioSweepCount());
        snprintf(l3, sizeof(l3), "引脚 %s", audioSweepPins());
        snprintf(l4, sizeof(l4), "功放 %s", audioSweepAmp());
        uiInfoPage("正在试音频引脚", "听到「嘀——」就记下这个号", l2, l3, l4);
      }
    }
    A.lastAct = millis();          // 别让省电逻辑在这时候熄屏
  } else if (netHelp) {
    // 提示页：告诉用户「连哪个热点、连上会怎样」。15 秒后自动收起，
    // 免得一直占着屏看不出机器还在不在工作（广播照常在响）。
    char l2[40], l3[40], l4[40];
    snprintf(l2, sizeof(l2), "手机连「我的收音机」（无密码）");
    snprintf(l3, sizeof(l3), "连上会自动弹出配网页");
    snprintf(l4, sizeof(l4), "填家里 WiFi 名字和密码 · %lu 秒后收起",
             (unsigned long)(netAutoSetupLeftMs() / 1000));
    uiInfoPage("WiFi 连不上，要重新配网", "机器记的那个网络连不上", l2, l3, l4);
    A.lastAct = millis();               // 提示期间别熄屏
    if (s_swPrev) uiForceRedraw();
  } else {
    uiLoop();
    if (s_swPrev) uiForceRedraw(); // 退出扫描：整屏重画，把可能残留的花屏一次性抹掉
  }
  s_swPrev = sweeping;

  // ---------- 一有网就自动开播，不等任何人操作 ----------

  if (A.wifiOk && !s_played && !sweeping) {
    s_played = true;
    if (A.cur >= 0) audioPlayIndex(A.cur);
  }
  if (s_played && !A.wifiOk) s_played = false;   // 掉线重连后重新起播

  if (!sweeping) fmModeTick();   // 没网自动切 FM，有网自动切回来（扫描时不切，免得抢音频）
  cloudLoop();                   // 跨网命令（外地手机发来的）在主循环里执行
  storeTick();
  albumTick();                   // 有声书进度合并写盘（2 秒）
  episTick();                    // 集进度的合并写盘（同一个节奏）
  resumeTick();                  // 1.46.2：断点进度的合并写盘
  historyTick();                 // 收听时长按秒累计，60 秒才写一次 flash
  healthTick();                  // 1.17.0：体检记录合并写盘（8 秒）
  offairTick();                  // 1.46.6：停播台账倒计时 / 到点自动放回

  // ---------- 1.43.0：每 5 秒一行状态心跳 ----------
  //
  //  为什么加：这台机器的调试口是 USB-CDC（ARDUINO_USB_CDC_ON_BOOT=1），
  //  插着电脑就能看。用户报「没声音 / 只能搜到一个台」时，没有这行就只能猜 ——
  //  有了它一眼能分清是「固件没跑」「WiFi 没连」「FM 芯片没起来」还是
  //  「芯片起来了但 RSSI 就是低（天线/前端的事）」，这几种的修法完全不同。
  {
    static uint32_t hbAt = 0;
    if (millis() - hbAt > 5000) {
      hbAt = millis();
      Serial.printf("[hb] wifi=%d ap=%d ip=%s | fm=%d ready=%d %s %d rssi=%d n=%d | "
                    "vol=%d/%d | batt=%dmV %d%% ok=%d | sta=%d\n",
                    A.wifiOk ? 1 : 0, netApActive() ? 1 : 0, netIP(),
                    A.fmMode ? 1 : 0, fmReady() ? 1 : 0,
                    fmReady() ? radioBandCode(fmFreq()) : "-", fmFreq(),
                    fmRssi(), fmScanCount(),
                    A.vol, A.fmVol,
                    batteryMv(), batteryPct(), batteryOk() ? 1 : 0,
                    g_stN);
    }
  }
  delay(1);
}
