#include "input.h"
#include "fm.h"
#include "evlog.h"      // 1.46.40：按键/旋钮的可疑动作留痕
#include "audio.h"
#include "store.h"
#include "album.h"
#include "offair.h"   // 1.46.6：跳过现在不播的台
#include "net.h"      // 1.43.0：长按 3 秒切热点
#include <LittleFS.h>

// ------------------------------------------------------------
//  编码器（正交解码 + detent 锁存）
// ------------------------------------------------------------
// 1.46.20 —— 「不管转多快，一格就是一格」：不再数边沿，改成**认停位**。
//
//   老做法（1.45.5 ~ 1.46.19）是「累计原始边沿 ÷ ENC_DIV」。它能跑，但有结构性毛病：
//   这颗 EC11 每格的边沿数**不是常数**（真机 /api/enc 抓到 1 / 4 / 17 / 49 都出现过），
//   ÷4 只是平均值。于是：
//     · 快转时一格冒出十几个边沿 → 算成 3~4 格（用户："扭一格调了 3 格"）
//     · 慢转到一半松手 → 余数停 60ms 就兑现成一格，回弹再反向一格
//       （用户："轻轻动一点点台变了，松手又变回来"）
//
//   现在换成纯机械判据，一条时间常数都不靠：
//     EC11 每一格的停位（detent）落在**同一个 AB 电平**上（上拉时是 11），
//     走一格 = 走完一整个正交循环 11→01→00→10→11，中途**必然经过对侧电平** 00。
//     所以：
//       ① 看到对侧电平       → 这一格"成立"（arm）
//       ② 再回到 detent 电平 → 兑现一格，符号 = 这一趟的**净相位**
//       ③ 没经过对侧就回到 detent（手抖、半格退回）→ 净位移 0，不出格
//     一次停位最多出一格 ⇒ 快转、慢转、回弹、连甩都是「一格一台」。
//
//   detent 电平开机读一次，之后 AB 静止 ENC_DET_LEARN_MS 就跟着自愈
//   （防开机瞬间手正压在旋钮上、读到中间态）。
static const int8_t ENC_TBL[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

static volatile uint8_t  s_encAB    = 3;    // 当前 AB（bit1=A，bit0=B）
static volatile uint8_t  s_encDet   = 3;    // detent（静止）电平
static volatile uint8_t  s_encArm   = 0;    // 这一格已经走过对侧 ⇒ 回 detent 就兑现
static volatile uint8_t  s_encSkip  = 0;    // 1.46.29：这一趟出现过对角跳变（丢了一个中间态）
static volatile uint8_t  s_encOk    = 0;    // 已成功兑现过一格 ⇒ detent 可信，不再自动校正
static volatile int32_t  s_encPhase = 0;    // 原始相位累计（4 倍频，用来算净位移 / 诊断）
static volatile int32_t  s_encMark  = 0;    // 上一次停在 detent 时的相位
static volatile int16_t  s_encSteps = 0;    // 已兑现、还没被 inputLoop 取走的格数
static volatile uint32_t s_encEdgeAt = 0;
static volatile uint16_t s_encRun   = 0;    // 这一格到目前为止的边沿数（诊断）
static volatile uint16_t s_encPeak  = 0;    // 最近一格实际几个边沿（诊断：证明边沿数不固定）
// 1.46.48：最后一个**确定方向**（st != 0 时更新）。
//   用途：这一格的对侧样本被中断吞掉时，靠它判方向 —— 否则整格白转。
static volatile int8_t   s_encLastDir = 1;
// 1.46.49：诊断 —— 每次停位时的净相位（限幅 ±4），和丢格/救回计数
static volatile int8_t   s_encNet[8]  = { 0 };
static volatile uint8_t  s_encNetW    = 0;
static volatile uint16_t s_encDrop    = 0;  // 往返/抖动，没有兑现的次数
static volatile uint16_t s_encRescue  = 0;  // 靠 skip（丢过样本）兜住兑现的次数

// 最近 8 格的原始边沿数，给 /api/enc 看
static volatile uint16_t s_encHist[8]   = { 0 };
static volatile uint8_t  s_encHistW     = 0;
static volatile uint8_t  s_encHistTotal = 0;
static uint16_t s_encClampN = 0;          // 1.46.22：护栏夹住过几次（诊断）
static uint8_t  s_encTurnLog = 0;         // 1.46.50：转动日志已记几条（前 20 条）

static void IRAM_ATTR encIsr() {
  const uint32_t now = micros();
  if (now - s_encEdgeAt < 30) return;     // 只防同一个沿重复进中断，不滤真实脉冲
  s_encEdgeAt = now;

  const uint8_t ab   = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  const uint8_t prev = s_encAB;
  if (ab == prev) return;
  s_encAB = ab;

  const int8_t st = ENC_TBL[(prev << 2) | ab];
  if (st == 0) {
    // 1.46.29：对角跳变 = 两次中断之间 A 和 B 各变了一次 ⇒ 中间那个状态被吞掉了
    //   （WiFi / 刷屏把 ISR 拖了几十微秒）。记一笔，回停位时用它兜底。
    s_encSkip = 1;
    return;
  }
  s_encPhase += st;
  if (st > 0)      s_encLastDir = 1;      // 1.46.48：记最后一次确定方向
  else if (st < 0) s_encLastDir = -1;
  if (s_encRun < 999) s_encRun++;

  if (ab == (uint8_t)(s_encDet ^ 0x03)) {
    // ① 走到对侧电平：这一格成立
    s_encArm = 1;
  } else if (ab == s_encDet) {
    // ② 回到停位：按「这一趟的净相位」定符号，每次停位最多 ±1
    const int32_t net = s_encPhase - s_encMark;
    // 1.46.29：不再死要「必须看见对侧电平」。
    //   真机上这个 ISR 会被 WiFi / 刷屏打断，两次进中断之间 A、B 可能各变一次 ——
    //   对侧那个样本被整个吞掉（表现为一次对角跳变），s_encArm 一直是 0，
    //   这一格就白转了。用户原话：「扭一格没有换台，再扭一下才换台」。
    //   兜底只给「确实丢过中间态」的情况（s_encSkip），还要净位移 ≥2（半格以上）：
    //   不能写成裸的 net>=2 —— 原地抖动 11→01→11 的净位移也是 2，
    //   那样会把 1.46.20 之前「轻轻动一点点台就变了」的老毛病招回来。
    // 1.46.49：判据重写 —— 只走「方向明确」的路，绝不猜方向。
    //
    //   net = 这一趟的净相位。真机转一格通常是 ±2 或 ±4；
    //   中断丢一个中间态（对角跳变）会掉到 ±1；原地往返是 0。
    //
    //   · net == 0        → 往返，什么都没转，不兑现
    //                       （以前只要 arm 置过位就兑现 dir=-1 ——
    //                        「轻轻动一点点台就变了」的残余就是这个）
    //   · |net| >= 1 且 (arm 或 skip) → 兑现，方向取 net 符号（真实相位，不是猜）
    //   · 其余            → 不兑现
    bool fired = false;
    int  dir   = 0;
    if (net == 0) {
      if (s_encArm || s_encSkip) s_encDrop++;   // 往返：不计格，但记一笔
    } else if (s_encArm || s_encSkip) {
      dir   = (net > 0) ? 1 : -1;
      fired = true;
      if (s_encSkip) {
        // 靠 skip 兜住的一般就是「被吞掉的那格」。记下来，事后能数清有多少。
        s_encRescue++;
        if (s_encRescue <= 20)
          evlog("enc", "skip 兑现 dir=%d net=%d run=%d", dir, (int)net, (int)s_encRun);
      }
    } else {
      // 既没采到对侧、也没有对角跳变 —— 半格抖动，不能兑现
      s_encDrop++;
    }
    // 留最近 8 次的净相位，供 /api/enc 看真实分布
    s_encNet[s_encNetW & 7] = (int8_t)((net > 4) ? 4 : (net < -4 ? -4 : net));
    s_encNetW++;

    if (fired) {
      if (dir > 0)      s_encSteps++;
      else if (dir < 0) s_encSteps--;
      s_encOk = 1;
      s_encPeak = s_encRun;
      s_encHist[s_encHistW & 7] = s_encRun;
      s_encHistW++;
      if (s_encHistTotal < 255) s_encHistTotal++;
    }
    s_encArm  = 0;
    s_encSkip = 0;
    s_encMark = s_encPhase;
    s_encRun  = 0;
  }
}

// 旋钮停转 250ms 才切节目 —— 中间不逐个连接，老人不会听到一串杂音
// 1.7.0：单位从「台」改成「节目」（一部书算一个），见 album.h
static int32_t  s_pending  = 0;
static uint32_t s_lastTurn = 0;

// ------------------------------------------------------------
//  按键（旋钮按下）
// ------------------------------------------------------------
static bool     s_stable   = false;   // 消抖后的状态（true=按下）
static bool     s_lastRaw  = false;
static uint32_t s_rawAt    = 0;
static uint32_t s_downAt   = 0;
static uint32_t s_upAt     = 0;
static bool     s_longSent = false;
// 1.43.0：这一次按住是不是已经触发过「切热点」（满 3 秒）。松手时用它决定要不要再重连。
static bool     s_apLong   = false;
static bool     s_waitDbl  = false;

// ------------------------------------------------------------
//  红外（NEC，只测下降沿间隔，够用且省中断）
// ------------------------------------------------------------
#define IR_N 7
enum { IR_NEXT = 0, IR_PREV, IR_VOLUP, IR_VOLDN, IR_PLAY, IR_GPREV, IR_GNEXT };
static const char* kIrName[IR_N] = {
  "下一台", "上一台", "音量+", "音量-", "播放暂停", "左键", "右键"
};
// 常见 NEC 遥控的默认码；每项给几个候选（不同遥控器码值不同），0xFF = 空位。
// 都不匹配也没关系 —— 上电按住旋钮进学习模式，按一遍就记住。
// 注意：新增的两项必须排在最后，否则老的 /ir.txt 会串键。
static const uint8_t kIrAlt[IR_N][3] = {
  { 0x52, 0x5A, 0xFF },   // 下一台   CH+
  { 0x18, 0x10, 0xFF },   // 上一台   CH-
  { 0x0A, 0x02, 0xFF },   // 音量+
  { 0x42, 0x4A, 0xFF },   // 音量-
  { 0x08, 0x00, 0xFF },   // 播放暂停
  { 0x45, 0x44, 0x07 },   // 左键     ◀（1.15.0 起无功能，键位留着）
  { 0x46, 0x40, 0x09 },   // 右键     ▶（1.15.0 起无功能，键位留着）
};
static uint8_t s_irLearn[IR_N] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static int     s_learnStep = -1;      // -1=不在学习；否则 0..IR_N-1
// 1.46.3：学习模式原来**只有「学完 7 个键」这一条出路**。上电只要误判一次按住
//   （见 inputBegin），台名就被「按 下一台 (1/7)」永久顶掉 —— 用户报的
//   「黑屏后转一下亮屏，台名没了，只剩按下一台(1/7)」就是它。现在加三个逃生口。
#define IR_LEARN_IDLE_MS 30000       // 30 秒没按遥控器 → 自动退出
static uint32_t s_learnAt = 0;       // 最近一次学习动作的时刻
static uint8_t s_lastCmd = 0;

#if IR_HAS_RECEIVER
static volatile uint16_t s_irBuf[40];
static volatile uint8_t  s_irN = 0;
static volatile uint32_t s_irLast = 0;

static void IRAM_ATTR irIsr() {
  uint32_t t = micros();
  uint16_t d = (uint16_t)(t - s_irLast);
  s_irLast = t;
  if (d > 20000) s_irN = 0;           // 帧间间隔，重新开始
  if (s_irN < 40) s_irBuf[s_irN++] = d;
}
#endif

static void irLoad() {
  if (!LittleFS.exists("/ir.txt")) return;
  File f = LittleFS.open("/ir.txt", "r");
  if (!f) return;
  for (int i = 0; i < IR_N && f.available(); i++) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (ln.startsWith("0x") || ln.startsWith("0X")) s_irLearn[i] = (uint8_t)strtol(ln.c_str(), nullptr, 16);
  }
  f.close();
}

static void irSave() {
  File f = LittleFS.open("/ir.txt", "w");
  if (!f) return;
  for (int i = 0; i < IR_N; i++) f.printf("0x%02X\n", s_irLearn[i]);
  f.close();
}

// ------------------------------------------------------------
//  动作
// ------------------------------------------------------------
// 收音模式下的「上一台/下一台」（遥控器 CH±）：一次走 5 个频道，跨波段自动接上
static void fmStep(int dir) {
  fmTune(radioWalk(A.fmFreq, dir >= 0 ? 1 : -1, 5));
  A.fmFreq = fmFreq();
  // 1.34.0：不再置 A.dirty。A.fmFreq 变了，uiLoop 的 needFreq 会自己发现，
  //   只重画频率/台名区；置 A.dirty 会整屏 fillScreen，调台时一片闪。
}

// 旋钮转收音频率：一格走多远看她转得多快 —— 慢转 1 个频道（精调），
// 正常 5 个，快转 10 个，甩起来 30 个。所以「换一个台」不用再一格一格挪。
// 只改目标值（fmTune 内部不动 I2C），芯片会等手停下来再跟过去，连转不会卡。
static uint32_t s_prevTurnAt = 0;      // 上一批脉冲的时刻，用来算转速

// 1.37.0 —— 加速档改判「这一批脉冲有几格」，不再按毫秒间隔判。
//
//   旧的 radioGear(per) 里 `per <= 130ms` 就算「快转」→ gear = 10，
//   而**老人正常慢转正好是 100~300ms 一格**，于是每一格都跳 10 个频道 = 1MHz。
//   用户说的「频道乱窜」有一大半是这个：他以为在精调，实际每格都在跨越。
//
//   新规则：只有「同一批里连着来了好几格」才加速。
//   一批只有一格时，默认就是**精调 0.1MHz**（除非两批之间不到 80ms，那是真在甩）。
static int turnGear(int clicks, uint32_t dtMs) {
  const int n = (clicks < 0) ? -clicks : clicks;
  if (n >= 8) return 30;        // 一口气甩 8 格以上：整段扫
  if (n >= 4) return 10;        // 连转 4~7 格：快
  if (n >= 2) return 5;         // 连转 2~3 格：正常加速
  return (dtMs <= 80) ? 5 : 1;  // 只有一格：80ms 内又动了才算快，否则精调
}

static void radioTurn(int clicks) {
  const uint32_t now = millis();
  const uint32_t dt = s_prevTurnAt ? (now - s_prevTurnAt) : 9999u;

  // ---- 1.46.59：收音改成**手动调频**，旋钮不再"在搜到的台里跳" ----
  //
  //   1.38.0 起旋钮一格 = 搜到的下一个台。好处是不用一格一格挪，坏处有三个：
  //     · 转不到两个台之间的频率 ⇒ 听不到底噪，也没法自己判断"这里有没有台"
  //     · 台表一旦是残废的（历史遗留只有 1 个台），旋钮就永远只能在这 1 个台上转
  //     · 台表漏掉的弱台，用户凭手感永远找不回来（别的收音机能收到，这台收不到）
  //   用户的原话：「去掉自动搜台改为可以手动搜台，现在底噪也听不到，只有一个台」。
  //   现在旋钮就是频率本体：慢转一格 0.1MHz（FM），快转按 turnGear 加速跨段。
  //   想要"跳到下一个台"用 App 的上一台/下一台（那边仍走台表，见 apiStep）。
  const int gear = turnGear(clicks, dt);
  // 1.37.0：起点直接用 fmFreq()，不再用 A.fmFreq。
  //   A.fmFreq 是「上一帧拿去画屏」的副本，可能落后一帧；拿它算下一格会累积偏差。
  //   fmFreq() 返回的就是芯片目标值本体，唯一真相。
  fmTune(radioWalk(fmFreq(), clicks, gear));
  A.fmFreq = fmFreq();
  s_prevTurnAt = now;
}

// 1.7.0 起「换台」的粒度是「节目」（一部书 / 一个普通台），见 album.h
static int stepUnit(int base, int steps);     // 前置声明，定义在下面

static void doNext(int dir) {
  actTouch();
  if (A.fmMode) { fmStep(dir); return; }
  // 1.7.0：旋钮/遥控的「上一台/下一台」按节目走，一格一部书，不再一集一格
  if (g_stN <= 0) return;
  int base = (A.cur < 0) ? 0 : A.cur;
  int tgt  = stepUnit(base, dir);
  if (tgt >= 0) audioPlayIndex(albumEntry(tgt));
}

static void doVol(int d) {
  actTouch();
  // 1.22.0：以「当前这一路」的音量为基准加减，不是永远拿网络那个值。
  //   原来 A.vol 恒为基准 ⇒ 在收音里拧旋钮，先把网络的音量改了，声音没变，很怪。
  const int base = A.fmMode ? A.fmVol : A.vol;
  // 1.44.0：每格 ±1（原厂 volumeUp/volumeDown 就是一格一档），
  //   上下限在 audioSetVol 里按当前那一路夹
  audioSetVol(base + d);

  // 1.26.0：只有「按住旋钮转」这条路径才把台名换成音量。
  //   红外遥控的音量键（IR_VOLUP/DN）也走 doVol，但用户手里没按旋钮，
  //   屏上突然变成「音量」会很怪 —— 所以由调用点显式置位，不在 doVol 里做。
}

// 1.26.0：进入/退出「按住调音量」的显示状态
// 1.46.40：这次「按住」里有没有调过音量。
//   调过就说明用户是在调音量，不是长按 —— 松手时不能执行「长按重连」。
static bool s_volTurned = false;
// 1.46.54：最近一次「按住调音量」的时刻。松手那一瞬间编码器常还多出一格，
//   那时 s_stable 已经是 false，原来就掉进「换台」分支 —— 用户听到的正是
//   「调完音量松手，台被换掉了」。有这个时间戳就能给一段宽限窗。
static uint32_t s_volAt = 0;
#define VOL_TURN_GRACE_MS  400   // 松手后这段时间内的转动仍按「调音量」处理

static void volHoldBegin() {
  if (!A.volHold) { A.volHold = true; }   // 1.34.0：volHold/音量变化由 needFreq 的专用分支处理
  A.volHoldAt = millis();
}

static void volHoldEnd() {
  if (A.volHold) {
    A.volHold = false;     // 1.34.0：退出时也只让 needFreq 重画台名区
  }
}

// 从 base 走 steps 个「节目单元」（一部书算一个，普通台自己算一个），返回落地的台索引
static int stepUnit(int base, int steps) {
  const int n = g_stN;
  if (n <= 0) return -1;
  if (base < 0 || base >= n) base = 0;
  if (steps >  n) steps =  n;          // 转过的圈数没有意义，夹到一圈
  if (steps < -n) steps = -n;
  int idx = base;
  // 1.46.6：格子按「能听的台」走 —— 凌晨停播的台直接跳过，转不到它们。
  //   offairStep 内部就是 albumStep + 跳过台账，全表都被跳过时它自己会退回普通换台。
  while (steps > 0) { idx = offairStep(idx, +1); steps--; }
  while (steps < 0) { idx = offairStep(idx, -1); steps++; }
  return idx;
}

static void commitPending() {
  if (s_pending == 0) return;
  if (A.fmMode) { s_pending = 0; A.preview = -1; return; }   // 收音模式不碰网络台
  if (g_stN > 0) {
    // 1.46.22：待判格数再夹一道（旋钮 + 遥控/App 都汇到这儿）
    if (s_pending > ENC_STEP_CLAMP)  s_pending = ENC_STEP_CLAMP;
    if (s_pending < -ENC_STEP_CLAMP) s_pending = -ENC_STEP_CLAMP;
    int base = (A.cur < 0) ? 0 : A.cur;
    int tgt  = stepUnit(base, (int)s_pending);
    if (tgt >= 0) {
      // 1.46.2：先把 A.cur 挪到落点，再发播放命令。
      //   原来只发命令、不改 A.cur，屏上的台名要等 audioTask 处理完才换；
      //   而 audioTask 可能正卡在上一个台的 connecttohost（最长 30 秒），
      //   这段时间里"声音已经是新台、台名还是老台" —— 正是用户报的
      //   "换一个台台名还是一样，内容不一样了"。startIndex 里会再赋同样的值，幂等。
      const int land = albumEntry(tgt);
      // 1.46.54：记一条。用户报「屏上台序有时候不变」时，看这里就能分清
      //   是「这一格压根没提交」还是「提交了但屏没刷新」。
      if (land != A.cur) evlog("key", "换台 #%d → #%d（待判 %d 格）", A.cur, land, (int)s_pending);
      A.cur = land;
      audioPlayIndex(land);   // 一部书 → 从上次停的那集接着放
    }
  }
  s_pending = 0;
  A.preview = -1;
  // 1.34.0：A.cur / A.preview 变了 → needFreq 会重画；不再整屏刷
}

// 1.46.55：双击 → 回到第一个台。
//   网络模式：台表第 0 项所在的"单元入口"（第 0 项若属于某部书，就从那部书
//   上次听到的那一集开始；没记录就是第 1 集）。
//   收音模式：搜到的第一个台（台表按信号排序，第一个是最强的）；一个台都没搜到
//   就落到 FM 98.0，至少不是停在短波末端那种死寂。
static void goFirstStation() {
  if (A.fmMode) {
    const int khz = (fmScanCount() > 0) ? fmScanFreq(0) : 98000;
    fmTune(khz);
    evlog("key", "双击 → 回到第一个电台 %d kHz", khz);
    return;
  }
  if (g_stN <= 0) return;
  s_pending = 0;
  A.preview = -1;
  const int land = albumEntry(0);
  A.cur = land;
  audioPlayIndex(land);
  evlog("key", "双击 → 回到第一个台 #%d %s", land, g_st[land].name);
}

static void updatePreview() {
  if (g_stN <= 0 || s_pending == 0) { A.preview = -1; return; }
  int base = (A.cur < 0) ? 0 : A.cur;
  int tgt  = stepUnit(base, (int)s_pending);
  A.preview = (tgt >= 0) ? albumEntry(tgt) : -1;     // 预览就显示「接下来要播的那一集」
  // 1.34.0：转旋钮时这一行原本每格整屏重绘一次 → 就是「调台闪」的元凶，去掉
}

// 1.15.0：**不再有分组**，所以「点一下旋钮 / 遥控器左右 = 换组」这一整条链路都删了。
// 现在只剩：旋转=换台、按住旋转=音量、双击=静音、长按=重连当前台。
// 遥控器的「上一组 / 下一组」两个键位保留在学习表里（老遥控器的键码映射不用重学），
// 但按下去什么都不做 —— 换台请用 CH+ / CH-。

static void doAction(int i) {
  switch (i) {
    case IR_NEXT:  doNext(+1); break;
    case IR_PREV:  doNext(-1); break;
    case IR_VOLUP: doVol(+5);  break;
    case IR_VOLDN: doVol(-5);  break;
    case IR_PLAY:  audioPauseToggle(); break;   // ps 变了 needBox 会重画
    // case IR_GPREV / IR_GNEXT：1.15.0 起没有分组可切，按了不响应
  }
}

// 1.46.3：退出遥控器学习模式。三个入口：
//   ① 学完 7 个键（onIrKey 里）② 30 秒没按遥控器 ③ 有人转旋钮（那是在听广播，不是配遥控器）。
//   退出时不置 A.dirty：uiLoop 的 needFreq 比较了 l_learn，只重画台名区，不整屏闪。
static void learnExit(const char* why) {
  if (s_learnStep < 0) return;
  const int got = (s_learnStep > IR_N) ? IR_N : s_learnStep;
  s_learnStep = -1;
  if (got > 0) irSave();          // 学到几个先存几个（没轮到的槽位仍是旧键码）
  Serial.printf("[ir] 退出学习模式（%s，已学 %d/%d）\n", why, got, IR_N);
}

static void onIrKey(uint8_t cmd) {
  actTouch();
  // 学习模式：按顺序记下 IR_N 个键
  if (s_learnStep >= 0 && s_learnStep < IR_N) {
    s_learnAt = millis();         // 有动作就重置超时
    s_irLearn[s_learnStep] = cmd;
    Serial.printf("[ir] learn %s = 0x%02X\n", kIrName[s_learnStep], cmd);
    s_learnStep++;
    if (s_learnStep >= IR_N) { irSave(); s_learnStep = -1; A.dirty = true; }
    return;
  }
  // 1) 学过的码优先
  for (int i = 0; i < IR_N; i++)
    if (s_irLearn[i] != 0xFF && s_irLearn[i] == cmd) { doAction(i); return; }
  // 2) 没学过就用常见默认码兜底
  for (int i = 0; i < IR_N; i++)
    for (int k = 0; k < 3; k++)
      if (kIrAlt[i][k] != 0xFF && kIrAlt[i][k] == cmd) { doAction(i); return; }
  Serial.printf("[ir] 未映射 0x%02X（想用它请进学习模式）\n", cmd);
}

// ------------------------------------------------------------
void inputBegin() {
  // 编码器
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  // 1.37.1：开机必须把「上一次的 AB」填成当前值，否则第一帧查表会拿到一格偏移，
  //   表现为**开机后第一下转动白跳一格**。这里顺便把 AB 当作 detent 电平的初始值。
  {
    const uint8_t ab = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
    s_encAB    = ab;
    // 1.46.50：只接受 00 / 11 当停位初值。读到 01 / 10 说明旋钮正停在两格之间
    //   （上次关机前半格、或手搭在旋钮上）—— 那不是停位，硬拿来当 detent
    //   会让后面每半格出一格。这种情况先按 11 起步，靠「学错自愈」纠正。
    s_encDet   = (ab == 0x00 || ab == 0x03) ? ab : 0x03;
    s_encArm   = 0;
    s_encPhase = 0;
    s_encMark  = 0;
    s_encRun   = 0;
    s_encEdgeAt = micros();
    Serial.printf("[enc] 停位电平 AB=%d%d\n", (ab >> 1) & 1, ab & 1);
  }
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);

  // 1.46.3：这里必须「确认」一次再判按住。
  //   原来 pinMode(INPUT_PULLUP) 之后**立刻**读一次就定论 —— 而旋钮开关上并的
  //   消抖电容此刻还是 0V，读到的必然是 LOW ⇒ **每次开机都误进遥控器学习模式**，
  //   屏台上第一行就被「按 下一台 (1/7)」顶掉，台名看起来「没了」。
  //   等 600ms 让上拉把电容充起来、也让「顺手按一下」的手松开 ——
  //   两次都是 LOW 才算真的「按住上电」（学遥控器时本来就要一直按着，不受影响）。
  s_lastRaw = (digitalRead(PIN_ENC_SW) == LOW);
  if (s_lastRaw) { delay(600); s_lastRaw = (digitalRead(PIN_ENC_SW) == LOW); }
  s_stable  = s_lastRaw;

  // 上电时按住旋钮 → 遥控器学习模式
  if (s_stable) {
    s_learnStep = 0;
    s_learnAt   = millis();
    Serial.println("[ir] 学习模式：请依次按 下一台 / 上一台 / 音量+ / 音量- / 播放暂停 / 左键 / 右键");
  }

  irLoad();

#if IR_HAS_RECEIVER
  pinMode(PIN_IR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_IR), irIsr, FALLING);
#endif
}

bool inputLearnActive() { return s_learnStep >= 0; }

const char* inputLearnHint() {
  static char b[48];
  if (s_learnStep < 0) return "";
  if (s_learnStep >= IR_N) return "完成";
  snprintf(b, sizeof(b), "按 %s (%d/%d)", kIrName[s_learnStep], s_learnStep + 1, IR_N);
  return b;
}

// 编码器诊断。GET /api/enc（1.46.20 改口径）：
//   mode  = "detent" —— 不再按边沿数换算，所以 div/gap 这两个字段没有了；
//   det   = 学到的停位电平（0..3），detHex 是两位 AB；
//   arm   = 当前这一格是否已经走过对侧（1 = 手正停在格中间，不会出格）；
//   phase = 原始相位累计（4 倍频）；steps = 已兑现、还没被取走的格数；
//   peak  = 最近一格实际几个边沿，raw = 最近 8 格。
// 判读：raw 忽大忽小（1 / 4 / 17 / 49）正是这颗编码器边沿数不固定的证据，
//   也正是它不能按固定除数换算的原因 —— 现在只认停位，raw 只作参考。
const char* inputEncDiag() {
  static char b[240];
  uint16_t h[8];
  uint8_t  w = 0, tot = 0, det = 0, arm = 0, ok = 0;
  int32_t  ph = 0;
  int16_t  stp = 0;
  uint16_t peak = 0;
  int8_t   ldir = 1;
  uint16_t drop = 0, resq = 0;
  int8_t   nt[8] = { 0 };
  uint8_t  netW = 0;
  noInterrupts();
  for (int i = 0; i < 8; i++) { h[i] = s_encHist[i]; nt[i] = s_encNet[i]; }
  netW = s_encNetW;
  w = s_encHistW; tot = s_encHistTotal;
  ph = s_encPhase; stp = s_encSteps;
  det = s_encDet; arm = s_encArm; peak = s_encPeak; ok = s_encOk;
  ldir = s_encLastDir; drop = s_encDrop; resq = s_encRescue;
  interrupts();

  const int cnt = (tot < 8) ? tot : 8;
  int n = snprintf(b, sizeof(b),
                   "{\"mode\":\"detent\",\"det\":%d,\"detHex\":\"%d%d\",\"arm\":%d,\"ok\":%d,"
                   "\"peak\":%d,\"clamp\":%d,\"phase\":%ld,\"steps\":%d,\"batches\":%d,"
                   "\"dir\":%d,\"drop\":%d,\"rescue\":%d,\"net\":[",
                   (int)det, (det >> 1) & 1, det & 1, (int)arm, (int)ok,
                   (int)peak, (int)s_encClampN, (long)ph, (int)stp, (int)tot,
                   (int)ldir, (int)drop, (int)resq);
  // 1.46.49：最近 8 次停位的净相位（0=往返，±1=丢过样本，±2/±4=正常）
  for (int i = 0; i < 8 && n > 0 && n < (int)sizeof(b); i++) {
    const int idx = ((int)netW - 8 + i + 16) & 7;
    n += snprintf(b + n, sizeof(b) - n, "%s%d", i ? "," : "", (int)nt[idx]);
  }
  for (int i = 0; i < cnt && n > 0 && n < (int)sizeof(b); i++) {
    const int idx = ((int)w - cnt + i + 16) & 7;
    n += snprintf(b + n, sizeof(b) - n, "%s%d", i ? "," : "", (int)h[idx]);
  }
  if (n > 0 && n < (int)sizeof(b))
    snprintf(b + n, sizeof(b) - n, "]}");
  return b;
}

void inputLoop() {
  const uint32_t now = millis();

  // 1.46.3：学习模式超时自动退出 —— 否则台名会被学习提示一直顶着
  if (s_learnStep >= 0 && (uint32_t)(now - s_learnAt) > IR_LEARN_IDLE_MS)
    learnExit("30 秒没按遥控器");

  // ---------- 1) 编码器 ----------
  // 1.46.20：ISR 里已经算好「几格」了（一次停位最多 ±1），这里只取走 + 维护 detent 电平。
  int d = 0;
  noInterrupts();
  d = (int)s_encSteps;
  s_encSteps = 0;
  interrupts();

  // 1.46.22：硬护栏。detent 判据理论上一次停位只 ±1，但编码器坏、线松、电容漏电
  //   都可能让中断里冒出一长串假沿。一次取到 ±20 以上只可能是这种情况，
  //   夹住并记数 —— 宁可少走几台，也不能让编号从 140 一下飞到 280。
  if (d > ENC_STEP_CLAMP || d < -ENC_STEP_CLAMP) {
    Serial.printf("[enc] 一次取到 %d 格，超出护栏 ±%d，夹住\n", d, ENC_STEP_CLAMP);
    if (s_encClampN < 65535) s_encClampN++;
    d = (d > 0) ? ENC_STEP_CLAMP : -ENC_STEP_CLAMP;
  }

  // detent 电平自愈：AB 连续 ENC_DET_LEARN_MS 没动 ⇒ 旋钮就停在这一格上
  {
    static uint8_t  s_abSeen = 0xFF;
    static uint32_t s_abAt   = 0;
    const uint8_t abNow = s_encAB;
    if (abNow != s_abSeen) { s_abSeen = abNow; s_abAt = now; }
    else if ((uint32_t)(now - s_abAt) > ENC_DET_LEARN_MS &&
             s_encDet != abNow && !s_encOk) {
      // 只在「还没兑现过任何一格」时才校正 —— 一旦出过格，说明当前 detent 是对的，
      // 此后永不再改。这一条防的是：老人拧到两格中间停下犹豫，静止超过阈值就被
      // 当成新的停位，之后每格都判成半格。
      // 另外只接受 00 / 11：正交编码器的机械停位是「两相同断」或「两相同通」，
      // 01 / 10 是两格之间的中间态，不可能停在那儿。
      if (abNow == 0x00 || abNow == 0x03) {
        noInterrupts();
        const uint8_t old = s_encDet;
        s_encDet  = abNow;
        s_encArm  = 0;
        s_encSkip = 0;
        s_encMark = s_encPhase;
        s_encRun  = 0;
        interrupts();
        Serial.printf("[enc] detent 校正为 AB=%d%d\n", (abNow >> 1) & 1, abNow & 1);
        evlog("enc", "detent 校正 %d%d → %d%d", (old >> 1) & 1, old & 1,
              (abNow >> 1) & 1, abNow & 1);
      }
    }
  }

  // 1.46.50：detent 学错的自愈。
  //   硬判据：一次都没兑现过（!s_encOk），但相位已经累计超过一格（|phase-mark| >= 4）
  //   ⇒ 人确实在转，判据却一次都不成立 ⇒ detent 选错了电平。
  //   正常转动绝不会走到这一步（转一格立刻兑现，mark 会被重置）。
  //   翻转成对侧电平，并把基准拉回当前相位 —— 立刻恢复正常。
  if (!s_encOk) {
    int32_t drift = 0;
    uint8_t det = 0;
    noInterrupts();
    drift = s_encPhase - s_encMark;
    det   = s_encDet;
    if (drift >= 4 || drift <= -4) {
      s_encDet  = (uint8_t)(det ^ 0x03);
      s_encArm  = 0;
      s_encSkip = 0;
      s_encMark = s_encPhase;
      s_encRun  = 0;
    }
    interrupts();
    if (drift >= 4 || drift <= -4) {
      Serial.printf("[enc] detent 学错（相位已累计 %ld 却没出格）→ 翻转成 AB=%d%d\n",
                    (long)drift, (s_encDet >> 1) & 1, s_encDet & 1);
      evlog("enc", "detent 学错，翻转 %d%d → %d%d", (det >> 1) & 1, det & 1,
            (s_encDet >> 1) & 1, s_encDet & 1);
    }
  }

  if (d != 0) {
    if (ENC_REVERSE) d = -d;
    actTouch();
    // 1.46.3：会转旋钮 = 人在听广播，不是在配遥控器 → 学习模式让路，台名立刻回来
    if (s_learnStep >= 0) learnExit("旋钮被转动");
    // 1.37.0：光看 s_stable 不够 —— 12ms 消抖挡不住旋钮按钮的接触抖动，
    //   一次误触发的 LOW 脉冲就会把「转频率」变成「调音量」。
    //   所以要求「按下状态已经持续」才算真的按住。
    //
    // 1.46.54：40ms -> 20ms，并加一条「宽限窗」。两个漏点都在用户那一手之内：
    //   ① 按下后 40ms 内转的那一格 —— 他想调音量，却被当成换台；
    //   ② 松手那一瞬间多出来的一格 —— s_stable 已是 false，同样掉进换台分支。
    //      用户报的「调音量松手时换台了」就是这一格。
    //   只要这一手调过音量（s_volTurned），松手后 400ms 内的转动仍按音量算；
    //   过了 400ms 照旧换台，不影响正常转台。
    const bool volNow = (s_stable && (now - s_downAt) >= 20) ||
                        (s_volTurned && s_volAt && (now - s_volAt) < VOL_TURN_GRACE_MS);
    if (volNow) {
      // 1.26.0：按住旋转 = 音量，同时把屏上大字换成「音量 X」
      // 1.46.20：一格一档，连甩几格就调几档（夹到 ±5，免得一下拧到底）
      int dv = d;
      if (dv >  5) dv =  5;
      if (dv < -5) dv = -5;
      volHoldBegin();
      s_volTurned = true;          // 1.46.40：这次按住是调音量，别当长按
      s_volAt     = now;           // 1.46.54：宽限窗的时间基准
      doVol(dv * VOL_HOLD_STEP);
    } else if (A.fmMode) {
      radioTurn(d);                                    // 收音模式：转的就是频率/台
      s_waitDbl = false;
    } else {
      // 1.46.50：把「转了几格 → 落到第几台」记下来（只记前 20 次）。
      //   用户说"转两次才走一台"时，看这个就知道是格数不对还是落点不对。
      if (s_encTurnLog < 20) {
        s_encTurnLog++;
        evlog("enc", "转 %d 格，从 #%d 起算（累计 %ld）", d, A.cur, (long)s_pending);
      }
      s_pending += d;
      updatePreview();
      // 转了就说明用户要换节目，把「刚才那一下是不是单击」的待判作废
      s_waitDbl = false;
    }
    s_lastTurn = now;
  }
  if (s_pending != 0 && now - s_lastTurn > 250) commitPending();

  // ---------- 2) 按键消抖 ----------
  bool raw = (digitalRead(PIN_ENC_SW) == LOW);
  if (raw != s_lastRaw) { s_lastRaw = raw; s_rawAt = now; }
  if (now - s_rawAt > 12 && raw != s_stable) {
    s_stable = raw;
    if (s_stable) {                       // 按下
      s_downAt = now;
      s_longSent = false;
      s_apLong   = false;
      s_volTurned = false;                // 1.46.40：新的一次按住，重新开始算
    } else {                              // 松开
      s_upAt = now;
      if (s_longSent) {
        // 1.43.0：0.7 秒的老长按（重连当前台）改到**松手**才执行。
        //   按下瞬间执行的话，继续按到 3 秒就会「先重连一次、再切热点」，
        //   两件事叠加，用户听到的是断一下再响，还以为是切热点弄坏了声音。
        //   按满 3 秒那次（s_apLong）只切热点，不重连。
        // 1.46.40：**调过音量就不重连**。
        //   原来只要按住超过 0.7 秒就算「长按」，而调音量必须按住转，
        //   于是每次调完音量松手都会重连一次 —— 用户听到的是「断一下」，
        //   有声书还会因为 resume 失效从头播。
        if (!s_apLong && !s_volTurned && !A.fmMode && A.cur >= 0) {
          audioPlayIndex(A.cur);
          evlog("key", "长按重连当前台 #%d", A.cur);
        }
        s_volTurned = false;
      }
      else if (s_waitDbl && now - s_upAt < BTN_DBL_MS) {
        // 1.38.0：**双击不再静音**。
        //   用户反馈：「双击不扭动时不要音量归零」—— 老人手上没准，连点两下
        //   就把音量显示成 0、还得再点两下才回来，非常吓人。
        //   双击现在只做一件事：亮屏（actTouch 本来就会做）。
        //   要静音请用 App，或者按住旋钮转音量把数字调到最小。
        s_waitDbl = false;
        actTouch();
        goFirstStation();                 // 1.46.55：双击 → 回到第一个台
      } else {
        s_waitDbl = true;                 // 等 300ms 看有没有第二下
      }
    }
  }

  // 长按（按下未松）—— 1.43.0 分两档
  if (s_stable && !s_longSent && now - s_downAt > BTN_LONG_MS) {
    s_longSent = true;            // 只认可"这是一次长按"，动作等松手再做
    s_waitDbl  = false;
    actTouch();
  }

  // 满 3 秒：切热点（开 ↔ 关）。用户原话：「旋钮长按 3 秒打开热点，
  //   再长按 3 秒关闭热点」。放在按下期间判定，不等松手 —— 松手才动作的话，
  //   老人按满 3 秒看不到反馈，会以为没生效又接着按。
  if (s_stable && !s_apLong && now - s_downAt >= AP_HOLD_MS) {
    s_apLong = true;
    actTouch();
    netApToggle();
    Serial.printf("[input] 旋钮长按 3 秒 → 热点%s\n", netApActive() ? "开" : "关");
  }

  // 1.26.0：松开旋钮 -> 台名恢复。
  //   兜底：2.5 秒没再转动也退（万一松开检测漏一次，屏幕不会永久卡在「音量」上）。
  if (A.volHold && (!s_stable || now - A.volHoldAt > 2500)) volHoldEnd();

  // 单击（超时没有第二下）
  // 1.15.0：原来这里是「换下一个分组」，分组没了 —— 单击现在**什么都不做**，
  // 只把屏幕唤醒（actTouch）。不改成「换台」是因为旋钮转动已经在换台了，
  // 单击再换一次会让老人误碰就跳台。
  // 双击静音、长按重连、按住旋转音量，三个手势都不变。
  if (s_waitDbl && !s_stable && now - s_upAt > BTN_DBL_MS) {
    s_waitDbl = false;
    actTouch();
  }

  // ---------- 3) 红外解码 ----------
#if IR_HAS_RECEIVER
  if (s_irN > 0 && (micros() - s_irLast) > 15000) {
    uint8_t n = s_irN;
    uint16_t hdr = s_irBuf[0];
    s_irN = 0;
    if (n >= 32 && hdr >= 11000 && hdr <= 15000) {
      uint32_t raw = 0;
      bool ok = true;
      for (int i = 0; i < 31; i++) {
        uint16_t di = s_irBuf[1 + i];
        if (di > 1900) raw |= (1UL << i);
        else if (di > 800) ok = false;
      }
      uint8_t b0 = raw & 0xFF, b1 = (raw >> 8) & 0xFF, b2 = (raw >> 16) & 0xFF;
      if (ok && (uint8_t)(~b0) == b1) {          // NEC 地址校验
        if (b2 != s_lastCmd) { s_lastCmd = b2; onIrKey(b2); }
      }
    }
  }
#endif
}
