#include "ui.h"
#include "config.h"
#include "net.h"
#include "input.h"
#include "store.h"
#include "offair.h"      // 1.46.6：右下角「跳过 N」
#include "epis.h"
#include "fm.h"
#include "battery.h"
#include "power.h"          // 1.21.0：亮度上限（过热 / 满电时会被压下来）
#include <time.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <TFT_eSPI.h>   // 1.38.0：font 7（48px 七段）   // 1.36.0：分辨帧缓存到底吃的是 PSRAM 还是内部堆

#include "font_cn.h"     // 由 tools/gen_font.py 生成

static TFT_eSPI tft;

// ============================================================
//  布局（1.4.4 起）
//   y   0..22   顶栏   时钟 + 星期 | 标签 | WiFi + 信号 + 音量胶囊
//   y  22..102  台名区 32px 大字（加大到 80 高，坐远了也看得清）
//   y 106..130  三个小框 状态 / 信号 / 音量
//  y 134..170  调谐盘 36px：指针钉在屏幕正中，刻度条随台滚动
//
//  1.4.4 的调整（真机调谐样式）：
//   · 指针固定屏幕中央不动（红竖线 + 顶部小倒三角，与蓝条同高）
//   · 刻度以当前台频率为中心滚动，数字是真实 FM 频率、对准长刻度、跟着一起滚
//   · 调谐盘 52px → 36px，省下的高度给台名区（64 → 80）
//   · 顶栏音量胶囊加高 1px，文字不再超出色块
// ============================================================
// ============================================================
//  布局（1.38.3 —— 用户逐条回调后的最终版）
//
//    y   0.. 21   顶栏   时钟 周日 [网络广播] …… WiFi 信号柱 电量图标 82%
//    y  22..103   台名/频率区（82px）
//    y 106..129   三个框  状态+序号 | 信号柱+dB | 音量+进度条
//    y 132..169   刻度条（38px）：指针居中、刻度随频率滚动
//
//  和 1.38.0 的差别（用户要的）：
//    · 刻度改回「滚动式」（不是原厂那种固定 40 格），但稀疏/加粗/加短线/灰色数字
//    · 三个方框回来了（1.38.0 删掉是错的：图2 就是原设计）
//    · 右侧音量竖条删掉 —— 框3 已经显示音量，再放一条竖向的会重复
//    · 电量图标改回 20×10 那个，并带百分比
//    · 顶栏中间那块改成「广播 / 网络广播」
// ============================================================
// 1.38.5：顶栏 22 → 24px。
//   原来 20px 的字塞在 22px 高里，上下各只留 1px，肉眼就是「贴到屏幕最上边」。
//   24px 里字占 20px，上下各 2px，看着才不憋。
#define TOP_H     24
#define MAIN_Y    24
#define MAIN_H    80               // 24..103
#define BOX_Y    106
#define BOX_H     24               // 106..129
#define BOX1_X     5
#define BOX1_W   118               // 状态圆点 + 状态词 + 台序
#define BOX2_X   128
#define BOX2_W    86               // 信号柱 + dB
#define BOX3_X   219
#define BOX3_W    96               // 音量 + 进度条
// 刻度区 40px 的四段分配（1.38.4 重算，1.38.3 把数字和刻度画重叠了）
//     130..137  倒三角（8px，贴在三个框下面）
//     138..153  频率数字（16px）
//     154..169  刻度线（最长 16px）
#define TUN_Y    130
#define TUN_H    (SCR_H - TUN_Y)   // 40
// 1.38.5：去掉倒三角（用户要求）。原来 130..137 给三角，现在这 8px 还给数字区。
//   指针只留一根竖线，从数字行下面一直扎到屏底。
#define TUN_NUM_Y 130              // 数字行（16px）
#define TUN_PTR_Y 146              // 指针竖线起点（数字下面）
#define DIAL_CX  (SCR_W / 2)       // 指针钉在正中
#define DIAL_PX    8               // 每 0.1MHz 占 8px（1MHz=80px，稀疏好认）

#define MARGIN     5
#define BIG_PX    36               // 台名字高（font36）
#define MID_PX    20               // 顶栏/三个框字高（font20）
#define SML_PX    16               // 刻度数字等（font16）
#define F7_H      48               // TFT_eSPI font 7 的字高

// ============================================================
//  1.36.0 —— 离屏帧缓存（双缓冲），这才是「闪烁」的正解
//
//  1.35.0 只做了「限速」，用户反馈「频率低了但还闪」—— 说明限速只是把
//  闪的节奏从 25Hz 压到 7Hz/25Hz 两档，闪这个动作本身没被消除。
//
//  为什么必然闪：这块屏是 **8 位并口、没有 DMA**，TFT_eSPI 的每个
//  fillRect / drawBitmap 都是**立刻写进玻璃**的。所以
//      fillRect(整块抹成本色)  →  （十几毫秒的空白期）  →  画新字
//  这两步在屏上是分开的、肉眼看得见的。调台时 needFreq 每 40ms 触发一次，
//  drawFreq(320×76) + drawBoxes + drawTuner(320×44) 一起走一遍，
//  等于大半个屏每秒被抹黑二十几次 —— 就是「整个屏幕在闪」。
//
//  解法：所有绘制先画进 PSRAM 里的离屏缓存，画完**一次性**把那几块矩形
//  推到屏上。屏上只会看到「上一步的完整画面 → 这一步的完整画面」，
//  中间那一下空白只存在于内存里，永远不出现在玻璃上。
//
//  内存：320×170×2 = 108,800 字节（N16R8 有 8MB PSRAM，随便放）。
//  PSRAM 没起来时 createSprite 返回空 → 自动退回直绘（老行为），
//  功能一切照旧，只是还会闪。
// ============================================================
static TFT_eSprite spr(&tft);
static TFT_eSPI*  P    = &tft;      // 绘制目标：有帧缓存就画帧缓存，没有就直画
static bool       s_fb = false;     // 帧缓存是否可用

// 屏被分成四块互不重叠的区域，各自可以单独推屏 —— 只改顶栏就只推 22 行，
// 不比整屏 170 行省一点半点。
#define RGN_TOP   0x01              // y   0.. 21
#define RGN_MAIN  0x02              // y  22..103
#define RGN_BOX   0x04              // y 106..129（只有中间三段）
#define RGN_TUN   0x08              // y 130..169
static uint8_t s_dirtyRgn = 0;
static inline void mark(uint8_t r) { s_dirtyRgn |= r; }

// 诊断计数：串口里能直接看出「每秒推几次」，闪不闪不用猜
static uint32_t s_nFull = 0, s_nPush = 0, s_diagAt = 0, s_nFullSeen = 0, s_nPushSeen = 0;

// 把这轮画过的区域推上去
static void flushRegions() {
  const uint8_t d = s_dirtyRgn;
  s_dirtyRgn = 0;
  if (!s_fb || !d) return;
  s_nPush++;
  if (d & RGN_TOP)  spr.pushSprite(0,   0, 0,   0, SCR_W, TOP_H);
  if (d & RGN_MAIN) spr.pushSprite(0,  24, 0,  24, SCR_W, MAIN_H);
  if (d & RGN_BOX)  spr.pushSprite(0, 106, 0, 106, SCR_W, BOX_H);
  if (d & RGN_TUN)  spr.pushSprite(0, 130, 0, 130, SCR_W, SCR_H - 130);
}

// 整屏一次推完（开机 / 换页用；一次写完，绝不会看到中间态）
static void pushAll() {
  s_dirtyRgn = 0;
  if (!s_fb) return;
  s_nPush++; s_nFull++;
  spr.pushSprite(0, 0);
}

// ---------------- 亮度 ----------------
#define BL_CH 7
static int s_brightApplied = -1;

// 1.38.1：屏「点亮」总闸。为假时背光恒为 0、且屏处于 DISPLAY OFF。
//   之所以要这个闸：开屏那一下必须等**首帧画完**再亮，
//   否则背光先亮、显存还是随机的 ⇒ 用户看到白闪（见文件底部 uiShow 的注释）。
static bool s_show = false;

static void setBright(int pct) {
  pct = constrain(pct, 0, 100);
  if (pct == s_brightApplied) return;
  s_brightApplied = pct;
  int duty = (pct == 0) ? 0 : map(pct, 1, 100, 24, 255);
  // 1.43.0：极性改看自家的 BL_ON_LEVEL。
  //   原来这里写的是 TFT_eSPI 的 TFT_BACKLIGHT_ON —— 那个宏已经从
  //   platformio.ini 删掉了（留着它，库的 init() 会自己点背光 ⇒ 开屏白闪）。
  //   这块板实机是 **高电平点亮**（IO38）。哪天真换板成低电平点亮，
  //   只改 BL_ON_LEVEL 就行，这里会自动反相。
#if BL_ON_LEVEL == LOW
  duty = 255 - duty;
#endif
  ledcWrite(BL_CH, duty);
}

// ---------------- 点阵中文 ----------------
static const CnGlyph* findG(const CnGlyph* tab, int n, uint16_t cp) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int m = (lo + hi) >> 1;
    if (tab[m].cp == cp) return &tab[m];
    if (tab[m].cp < cp) lo = m + 1; else hi = m - 1;
  }
  return nullptr;
}

// 四档字号：0 = 16px（刻度/次要），1 = 20px（顶栏/三个框），
//           2 = 36px（台名/中文大字），3 = 48px（收音频率，粗体 ASCII）
//
//  1.40.0：用户要「频段数字加大加粗」。中文那档不能整体加粗（16px 会糊），
//  所以单独出 48px 粗体、**只收 ASCII**——收音频率只有「FM 98.4」这种，
//  95 个字形 14.5KB，比给 4000 多个汉字加粗省 200KB+。
enum { FS16 = 0, FS20 = 1, FS36 = 2, FS48 = 3 };

static const CnGlyph* glyphS(uint16_t cp, int fs) {
  if (fs == FS48) return findG(font48_idx, FONT48_N, cp);
  if (fs == FS36) return findG(font36_idx, FONT36_N, cp);
  if (fs == FS20) return findG(font20_idx, FONT20_N, cp);
  return findG(font16_idx, FONT16_N, cp);
}
static const uint8_t* blobS(const CnGlyph* g, int fs) {
  if (fs == FS48) return &font48_blob[g->off];
  if (fs == FS36) return &font36_blob[g->off];
  if (fs == FS20) return &font20_blob[g->off];
  return &font16_blob[g->off];
}
static int heightOf(int fs) {
  return (fs == FS48) ? 48 : (fs == FS36) ? 36 : (fs == FS20) ? 20 : 16;
}

static const CnGlyph* glyphOf(uint16_t cp, bool big) {
  return glyphS(cp, big ? FS36 : FS16);
}

static const uint8_t* nextCp(const uint8_t* p, uint16_t& cp) {
  if (p[0] < 0x80) { cp = p[0]; return p + 1; }
  if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); return p + 2; }
  if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return p + 3; }
  cp = 0xFFFD; return p + 4;
}

static int textWS(const char* s, int fs) {
  int w = 0;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp; p = nextCp(p, cp);
    const CnGlyph* g = glyphS(cp, fs);
    w += g ? g->w : heightOf(fs);
  }
  return w;
}
static int textWidth(const char* s, bool big) { return textWS(s, big ? FS36 : FS16); }

static void cnTextS(const char* s, int x, int y, uint16_t fg, uint16_t bg, int fs) {
  const uint8_t* p = (const uint8_t*)s;
  // 1.38.3 修 bug —— 这里原来写死 H = 28，而字库是 32px 高。
  //   drawBitmap 只画了前 28 行，**每个大字的底部 4 行一直被切掉**：
  //   「中国之声」的下半截是缺的，看着又小又怪，也就难怪用户说"字体不像原厂"。
  //   现在按字号真实高度取。
  const int H = heightOf(fs);
  const int fallbackW = H;
  while (*p) {
    uint16_t cp; p = nextCp(p, cp);
    const CnGlyph* g = glyphS(cp, fs);
    int w = g ? g->w : fallbackW;
    P->fillRect(x, y, w, H, bg);
    if (g) P->drawBitmap(x, y, blobS(g, fs), w, H, fg);
    x += w;
  }
}
// 透明文字：只画笔画，不动背后的底色。
//
// 1.40.0 为什么需要它：电池方块里「82」这几个数字，原来是用 cnTextS 画的，
// 而 cnTextS 会先把整块字形区域**填成 bg 色**。填入的参数是"填充色"，
// 于是没被填充覆盖到的那半个方块里，出现一块和填充色一样的长方形 ——
// 真机上看着就是"电池图标显示不正常"（数字底下顶着一条彩条）。
// 改用透明字后，数字直接压在填充和底色上，不再有方块。
static void cnTextT(const char* s, int x, int y, uint16_t fg, int fs) {
  const uint8_t* p = (const uint8_t*)s;
  const int H = heightOf(fs);
  const int fallbackW = H;
  while (*p) {
    uint16_t cp; p = nextCp(p, cp);
    const CnGlyph* g = glyphS(cp, fs);
    const int w = g ? g->w : fallbackW;
    if (g) P->drawBitmap(x, y, blobS(g, fs), w, H, fg);   // 单色重载 = 透明
    x += w;
  }
}

// 带 1px 黑色描边的透明文字：底是深色也读得清、底是蓝/黄/红也读得清。
//   做法就是把同一串字先往右下偏移 1px 用底色描一遍，再原位置画正文。
static void cnTextTS(const char* s, int x, int y, uint16_t fg, uint16_t outline, int fs) {
  cnTextT(s, x + 1, y + 1, outline, fs);
  cnTextT(s, x,     y,     fg,      fs);
}

static void cnText(const char* s, int x, int y, uint16_t fg, uint16_t bg, bool big) {
  cnTextS(s, x, y, fg, bg, big ? FS36 : FS16);
}

static void cnCenterS(const char* s, int y, uint16_t fg, uint16_t bg, int fs) {
  cnTextS(s, (SCR_W - textWS(s, fs)) / 2, y, fg, bg, fs);
}
static void cnCenter(const char* s, int y, uint16_t fg, uint16_t bg, bool big) {
  cnCenterS(s, y, fg, bg, big ? FS36 : FS16);
}

// 超宽就截断加省略号（单行显示，预览器 / App 都是这个规则）
static void cutToS(const char* s, char* out, size_t cap, int maxW, int fs) {
  int acc = 0;
  size_t n = 0;
  out[0] = 0;
  const int pad = heightOf(fs);
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint16_t cp; const uint8_t* q = nextCp(p, cp);
    const CnGlyph* g = glyphS(cp, fs);
    int gw = g ? g->w : pad;
    if (acc + gw > maxW - pad) { strcpy(out + n, "…"); return; }
    size_t len = q - p;
    if (n + len < cap - 4) { memcpy(out + n, p, len); n += len; out[n] = 0; }
    acc += gw;
    p = q;
  }
}
static void cutTo(const char* s, char* out, size_t cap, int maxW, bool big) {
  cutToS(s, out, cap, maxW, big ? FS36 : FS16);
}

// ============================================================
//  1.39.0：台名清洗 —— 去掉名字里的「FM/AM + 数字频段」
//
//  台表里很多台名带着频段：渭南广播FM102.6 / FM882 成都故事广播 /
//  中江综合广播994 / 100.7重庆永川之声 / AM1161无锡新闻综合广播。
//  屏上只有 320px，这些数字既占地方又没用（频率已经在调谐盘上了）。
//
//  规则（按顺序，只作用在**显示**上，不动 g_st 里的原名 —— 调谐盘还要靠原名找频率）：
//    ① 删掉 FM/AM/Fm/aM + 紧跟的数字（可带小数）：FM102.6 → 空
//    ② 删掉独立的频率 token：数字解析后落在
//         调频 64.0~108.0（按 0.1MHz 记就是 640~1080）
//         或 中波 531~1602 / 长波 153~279 / 短波 2300~26100
//       且位数 2~5。这样 CCTV1 / CCTV10 里的 1、10 不会被误删（不在任何波段里）。
//    ③ 清理首尾空格、多余的连字符和括号
//    ④ 全删空了就保留原名（宁可难看，也不能出现空台名）
// ============================================================
static bool freqTok(int v, int digits) {
  if (digits >= 2 && digits <= 5) {
    // 带小数的（89.1）按 0.1MHz 记：整数部分 64~108
    if (v >= 640 && v <= 1080) return true;          // 调频 0.1MHz 单位
    if (v >= 153 && v <= 279)   return true;         // 长波
    if (v >= 531 && v <= 1602) return true;          // 中波
    if (v >= 2300 && v <= 26100) return true;        // 短波
  }
  return false;
}

static void cleanName(const char* in, char* out, size_t cap) {
  out[0] = 0;
  if (!in || !*in) return;

  char tmp[80];
  strncpy(tmp, in, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;

  // ① 去掉 FM/AM + 紧跟的数字
  char buf[80];
  size_t n = 0;
  for (size_t i = 0; tmp[i] && n < sizeof(buf) - 1; ) {
    const char c0 = tmp[i];
    const bool isFm = (c0 == 'F' || c0 == 'f' || c0 == 'A' || c0 == 'a');
    const bool isMm = (tmp[i + 1] == 'M' || tmp[i + 1] == 'm');
    if (isFm && isMm) {
      size_t j = i + 2;
      while (tmp[j] == ' ' || tmp[j] == '-') j++;       // 「FM 102.6」也认
      bool any = false;
      while ((tmp[j] >= '0' && tmp[j] <= '9') || tmp[j] == '.') { j++; any = true; }
      if (any) { i = j; continue; }                     // 有数字才算频段，否则保留字母
    }
    buf[n++] = tmp[i++];
  }
  buf[n] = 0;

  // ② 去掉独立的频率 token
  char out2[80];
  size_t m = 0;
  for (size_t i = 0; buf[i] && m < sizeof(out2) - 2; ) {
    if (buf[i] >= '0' && buf[i] <= '9') {
      size_t j = i;
      long v = 0;
      int digits = 0;
      bool dot = false;
      while (buf[j] && ((buf[j] >= '0' && buf[j] <= '9') || buf[j] == '.')) {
        if (buf[j] == '.') {
          if (dot) break;
          dot = true;
          // 小数点后的第一位算进 v（89.1 → 891，用 0.1MHz 单位）
          j++;
          if (buf[j] >= '0' && buf[j] <= '9') { v = v * 10 + (buf[j] - '0'); j++; }
          while (buf[j] >= '0' && buf[j] <= '9') j++;   // 后面多余的小数位丢掉
          break;
        }
        v = v * 10 + (buf[j] - '0');
        digits++;
        j++;
      }
      // 带小数时 digits 记的是整数部分位数，按 0.1MHz 单位判断
      const int effDigits = digits + (dot ? 1 : 0);
      if (freqTok((int)v, effDigits)) { i = j; continue; }
      // 不是频率：原样搬过去
      while (i < j && m < sizeof(out2) - 2) out2[m++] = buf[i++];
      continue;
    }
    out2[m++] = buf[i++];
  }
  out2[m] = 0;

  // ③ 首尾空白 / 残留标点
  char* p = out2;
  while (*p == ' ' || *p == '-' || *p == '|') p++;
  size_t L = strlen(p);
  while (L > 0 && (p[L - 1] == ' ' || p[L - 1] == '-' || p[L - 1] == '|')) p[--L] = 0;

  // ④ 全空就退回原名
  strncpy(out, (*p ? p : in), cap - 1);
  out[cap - 1] = 0;
}

// ---------------- 小部件 ----------------
// 信号柱（对应预览器顶栏的信号强度）
// 顶栏的四格信号柱（1.37 原样）
static void sigBars(int x, int y, bool ok, int rssi) {
  int bars = 0;
  if (ok) bars = (rssi > -55) ? 4 : (rssi > -67) ? 3 : (rssi > -78) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    const int h = 4 + i * 3;
    P->fillRect(x + i * 6, y + 13 - h, 4, h, (i < bars) ? C_COOL_PAINT : C_BAR_BG);
  }
}

// 音量胶囊（对应预览器 .volpill）：静音时描边和数字转琥珀色
// 1.4.4：胶囊加高到 16px——里面 16px 的字此前挤在 15px 里，上下各溢 1px
// 1.38.0：原来顶栏那个「音量胶囊」换成了**屏幕右侧的竖条**。
//   为什么改：顶栏 21px 高，胶囊占掉 52px 宽，数字挤在里面 16px 字上下各溢 1px；
//   而老人真正需要的是「一眼看出音量有多大」—— 竖向高度差比横向更直观，
//   也不跟顶栏的时钟/波段名抢位置。
// 1.38.3：右侧音量竖条**删除**。
//   1.38.0 用竖条换掉了顶栏的胶囊，但框3（音量 + 进度条）又回来了 ——
//   两者同时存在就是重复显示同一个数，而且横竖两套读数会让老人更糊涂。
//   保留这个空函数只是为了让旧调用点不用全改（下面也不再调它）。
static void drawVolBar() { }

// ---------------- 各分区 ----------------
// 收音模式下「在哪个波段」的两种说法：4 字给全称，2 字给窄框
static const char* bandText() {
  switch (radioBand(A.fmFreq)) {
    case RADIO_LW: return "长波收音";
    case RADIO_MW: return "中波收音";
    case RADIO_SW: return "短波收音";
    default:       return "调频收音";
  }
}
static const char* bandTextShort() {
  switch (radioBand(A.fmFreq)) {
    case RADIO_LW: return "长波";
    case RADIO_MW: return "中波";
    case RADIO_SW: return "短波";
    default:       return "调频";
  }
}

static const char* psText() {
  // 1.43.0：静音功能已删，屏上不再有「已静音」这个状态
  // 1.46.39：静噪把声音压掉时必须说出来。
  //   以前屏上照旧写「广播 FM 98.0」，而一点声音都没有（芯片被硬静音）
  //   —— 用户报的「不联网的频道接收没有声音」就是这种观感：机器看着在播，其实哑了。
  if (A.fmMode && fmSquelched()) return "无信号";
  switch (A.ps) {
    case PS_PLAY: return "播放中";
    case PS_LINK: return "连接中";
    case PS_BUFF: return "缓冲中";
    case PS_ERR:  return "出错了";
    default:      return "已暂停";
  }
}

// 状态词的 2 字版：台数到三位后（如 100/140），框1 里全称 + 序号放不下，退成短词
static const char* psTextShort() {
  if (A.fmMode && fmSquelched()) return "无台";
  if (A.fmMode) return bandTextShort();
  switch (A.ps) {
    case PS_PLAY: return "播放";
    case PS_LINK: return "连接";
    case PS_BUFF: return "缓冲";
    case PS_ERR:  return "出错";
    default:      return "暂停";
  }
}

static uint16_t psColor() {
  if (A.fmMode && fmSquelched()) return C_WARN;   // 1.46.39：无信号用告警色
  if (A.fmMode) return C_COOL_PAINT;      // 收音不靠网络，红点会让人以为坏了
  if (!A.wifiOk) return C_ERR;
  switch (A.ps) {
    case PS_PLAY: return C_COOL_PAINT;
    case PS_LINK: return C_ACCENT;
    case PS_BUFF: return C_WARN;
    case PS_ERR:  return C_ERR;
    default:      return C_SUB;
  }
}

// 电池图标（1.19.0）：20×10 的小方框 + 正极小凸头，里面按百分比填充。
// 电量低 / 没电时整体转成告警橙，充电时中间画一条竖亮条。
// !! 没接电量检测（batteryOk() 为假）时整个函数不画 —— 顶栏布局与 1.18.0 完全一致。
static void battIcon(int x, int y) {
  const int w = 20, h = 10;
  const bool low = batteryLow();
  const uint16_t col = low ? C_ERR : C_SUB;
  P->drawRect(x, y, w, h, col);
  P->fillRect(x + w, y + 3, 2, 4, col);          // 正极凸头
  int pct = batteryPct();
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const int fw = (w - 4) * pct / 100;
  if (fw > 0) P->fillRect(x + 2, y + 2, fw, h - 4, low ? C_ERR : C_COOL_PAINT);
  if (batteryCharging()) P->fillRect(x + w / 2 - 1, y + 2, 2, h - 4, C_ACCENT);
}

// 顶栏：时钟 + 星期 + 标签 | 电量 + WiFi + 信号 + 音量
// ============================================================
//  1.38.2：顶栏电量条（4 格，原厂风格）
//
//  为什么不用百分比数字：见 battery.h 顶部那段。电压法在平台区（3.7~3.9V）
//  差 100mV 就是 30 个百分点，写个「71%」迟早被用户发现不对。
//  4 格 = 满 / 够用 / 少了 / 快没电，配上颜色足够老人判断"要不要充电"。
//  精确的百分比和电压在 App 里看（那里还能用万用表校准）。
//
//  布局：屏幕左上角 x=5，紧挨着它右边是时钟。信号柱在最右，两边不打架。
// ============================================================
// 当前电量对应几格（给「要不要重画顶栏」的比较用，与 drawBatt 里的分档保持一致）
// 1.38.3：电量图标 + 百分比（恢复 1.19.0 的 20×10 方框造型）
//   为什么图标和数字都要：图标一眼判"大概还剩多少"，数字给"到底多少"，
//   老人两种都认；而且数字和 App 里一致，能直接拿来对。
//   画多宽由这里决定，drawTop 靠 baW 往左排。
// ============================================================
//  1.38.5：电量改回「长方块 + 数字写在块里」
//
//  用户原话：「电量改回原来的长方块数字显示在长方块里」。
//  所以不是小电池图标 + 旁边的数字，而是**一个方块，数字在方块内部**：
//    外框（按电量填充）里居中写「82」—— 填充跟着真实电量走，
//    数字压在上面，一眼同时看到"大概还剩多少"和"具体多少"。
//
//  尺寸：48×20。为什么这么宽：要放得下「100」三个 16px 数字（约 27px）
//  再加左右各 6px 内边距。高度 20 与顶栏 24px 协调，也够 16px 字垂直居中。
// ============================================================
#define BATT_W  48
#define BATT_H  20

static int battWidth() { return BATT_W; }

// 电量方块：48×20，数字写在方块里（用户 1.38.5 起一直要的形态）
//
//  1.40.0 重写，修两件事：
//   ① 配色按用户指定分三档：≥40% 蓝 / <40% 黄 / <10% 红
//   ② 数字改**透明字 + 描边**。原来用 cnTextS 传"填充色"当底色，
//      结果方块右半边多出一条和填充同色的长条 —— 就是"图标显示不正常"。
//
//  为什么填充只到 fw、不在最后留 2px：留了会看起来"永远充不满"，
//  用户上一轮已经为此提过一次（4 格算法余 1px 那事）。
static void battIconPct(int x, int y) {
  const bool ok  = batteryOk();
  const int  pct = batteryPct();
  const bool chg = batteryCharging();

  if (!ok) {
    // 没接电池：空框 + 斜线（不写数字，避免出现假 0）
    P->drawRect(x, y, BATT_W, BATT_H, C_SUB);
    for (int i = 2; i < BATT_W - 2; i++)
      P->drawPixel(x + i, y + BATT_H - 2 - (i - 2) * (BATT_H - 4) / (BATT_W - 4), C_SUB);
    return;
  }

  int p = pct;
  if (p < 0) p = 0;
  if (p > 100) p = 100;

  // ---- 三档配色 ----
  uint16_t bar;
  if (p < 10)       bar = C_BATT_LOW;    // <10% 红
  else if (p < 40)  bar = C_BATT_MID;    // <40% 黄
  else              bar = C_BATT_HI_PAINT;     // ≥40% 蓝

  // 槽底（未填充部分）：比纯黑亮一点，方块边界才看得出来
  P->fillRect(x, y, BATT_W, BATT_H, C_BAR_BG);
  P->drawRect(x, y, BATT_W, BATT_H, C_SUB);

  // 填充：按真实电量走，充满时正好顶到右边框里侧
  const int inner = BATT_W - 4;                 // 留左右各 2px 边框内衬
  const int fw = (inner * p + 50) / 100;        // 四舍五入，别在 99% 时看着差一格
  if (fw > 0) P->fillRect(x + 2, y + 2, fw, BATT_H - 4, bar);

  // 数字：白色 + 深色描边，压在任何底色上都读得清
  char b[8];
  snprintf(b, sizeof(b), "%d", p);
  const int tw = textWS(b, FS16);
  cnTextTS(b, x + (BATT_W - tw) / 2, y + (BATT_H - heightOf(FS16)) / 2,
           C_WHITE, C_BG, FS16);

  // 充电：右下角一个小闪电（不挡数字）
  if (chg) {
    P->fillRect(x + BATT_W - 7, y + 3,  2, 3, C_ACCENT);
    P->fillRect(x + BATT_W - 6, y + 6,  2, 2, C_ACCENT);
    P->fillRect(x + BATT_W - 7, y + 8,  2, 2, C_ACCENT);
  }
}


static void drawBatt(int x, int y) {
  const bool hasBatt = batteryOk();
  const int  pct     = batteryPct();          // -1 = 未知
  const bool chg     = batteryCharging();
  const bool full    = batteryFull();

  // 外壳 21×11 + 正极凸头 2×5（原厂也是这么画的）
  //   宽度为什么是 21：内部可用 = 21-2 = 19px，
  //   正好放 4 格 4px + 3 个 1px 间隙 = 19 —— 一个像素都不浪费。
  //   （之前写 22 时内部 20px，整数除法算出 3px 的格子，右边空 3px，看着像没充满）
  const int w = 21, h = 11;
  // 1.38.2：电量很低时**外壳也转红**。
  //   否则 4% 时 0 格 + 白色外壳，跟「没接电池」长得一模一样，
  //   老人分不出"该充电了"和"这台没这功能"。
  const bool veryLow = hasBatt && pct >= 0 && pct <= 15;
  const uint16_t shellC = veryLow ? C_RED : C_WHITE;
  P->drawRect(x, y, w, h, shellC);
  P->fillRect(x + w, y + 3, 2, 5, shellC);

  if (!hasBatt) {
    // 没接电池（或读数不可信）：壳里画一条斜线表示「没有这一项」。
    //   不画会让人以为是 0% 快没电了。
    for (int i = 2; i < w - 2; i++)
      P->drawPixel(x + i, y + h - 2 - (i - 2) * (h - 4) / (w - 4), C_SUB);
    return;
  }

  // 3 格（不是 4 格）。
  //   内部只有 19px，4 格 + 1px 间隙在真机上挤成一整块，数不清。
  //   3 格 5px + 2 个 2px 间隙 = 19，一个像素不浪费，格与格之间清清楚楚。
  //   分档沿用原厂的思路（原厂就是 3 档：>50% / 5~50% / <5%），
  //   多出来的那一档是 25%，让「快没电」比原厂早一点被发现。
  const int gap   = 2;
  const int cellW = 5;
  int lit;
  if (chg)           lit = full ? 3 : 2;      // 充电中 2 格，充满 3 格
  else if (pct < 0)  lit = 3;
  else if (pct > 50) lit = 3;
  else if (pct > 20) lit = 2;
  else if (pct > 5)  lit = 1;
  else               lit = 0;

  const uint16_t c = veryLow ? C_RED : C_WHITE;
  for (int i = 0; i < 3; i++) {
    if (i >= lit) continue;
    P->fillRect(x + 2 + i * (cellW + gap), y + 2, cellW, h - 4, c);
  }

  // 充电：右上角小闪电
  if (chg) {
    P->fillRect(x + w - 7, y + 2, 2, 3, C_WHITE);
    P->fillRect(x + w - 6, y + 5, 2, 2, C_WHITE);
    P->fillRect(x + w - 7, y + 7, 2, 2, C_WHITE);
  }
}

// 顶栏：电量 + 时钟（左）| 波段名（正中）| 信号柱（右）
//   —— 音量已经不在这里了，见右侧 drawVolBar()
// 顶栏：时钟 周日 [网络广播]  ……  WiFi 信号柱 电量图标 82%
//   1.38.3：中间那块黄色徽标显示**广播 / 网络广播**（用户要求）。
//   原来显示的是标签名（广播/评书/历史…），但老人在机器前只关心
//   "我现在听的是收音机还是网络"，标签名留给 App。
// 顶栏：时钟 周日 [网络广播]  ……  WiFi 信号柱 电量 82%
//
//  1.38.4：宽度算过账。320px 里 20px 字全部放不下（实测要 331px），
//  所以「时间/星期」回到 16px —— 数字读得清就行，不需要和标题一样大；
//  「网络广播 / WiFi / 电量」保持 20px，这才是用户要放大的部分。
// 顶栏：时钟 周日 [网络广播]  ……  [WiFi] 信号柱 电量
//
//  1.38.5 用户要求：
//    · 四个信号格改成**广播时也显示**（用 fmRssi）—— 原来只在网络模式有意义
//    · **广播时没有 WiFi 这回事，所以不显示 WiFi 图标**（也不写「断网」，
//      收音机没网是正常的，写「断网」会让老人以为坏了）
//    · 电量改成长方块 + 数字写在块里
static void drawTop() {
  mark(RGN_TOP);
  P->fillRect(0, 0, SCR_W, TOP_H, C_BG);

  // 左：时钟 + 星期（16px）
  char clk[8] = "--:--";
  netClockStr(clk, sizeof(clk));
  int x = MARGIN;
  cnTextS(clk, x, 4, C_TIME, C_BG, FS16);
  x += textWS(clk, FS16) + 4;

  struct tm t;
  if (getLocalTime(&t, 0)) {
    static const char* WD[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    const char* w = WD[t.tm_wday % 7];
    cnTextS(w, x, 4, C_SUB, C_BG, FS16);
    x += textWS(w, FS16) + 5;
  }

  // 黄色徽标：广播 / 网络广播
  // 1.39.0：字号 20 → 16。20px 字塞在 20px 高的块里上下零间距，
  //   真机上就是「文字顶到块的上沿」。16px 放进去 y=4，上下各 2px 才居中。
  const char* md = A.fmMode ? "广播" : "网络广播";
  const int mw = textWS(md, FS16) + 8;
  P->fillRect(x, 2, mw, 20, C_BAND_BG_PAINT);
  cnTextS(md, x + 4, 4, C_BAND_FG, C_BAND_BG_PAINT, FS16);

  // 右：从右往左排
  int rx = SCR_W - MARGIN - battWidth();
  battIconPct(rx, 2);

  // 信号柱：两个模式都显示。广播用芯片 RSSI，网络用路由器 RSSI。
  const bool sigOk = A.fmMode ? true : A.wifiOk;
  const int  sigV  = A.fmMode ? A.fmRssi : A.rssi;
  rx -= 4 + 24;
  sigBars(rx, 3, sigOk, sigV);

  // WiFi 徽标只在网络模式画；广播模式整块让给信号柱
  if (!A.fmMode) {
    const char* wf = A.wifiOk ? "WiFi" : "断网";
    const int ww = textWS(wf, FS16) + 8;
    const uint16_t wbg = A.wifiOk ? C_MODE_BG : C_ERR;
    rx -= 4 + ww;
    P->fillRect(rx, 2, ww, 20, wbg);
    cnTextS(wf, rx + 4, 4, C_MODE_FG, wbg, FS16);   // 16px + y=4 才居中
  }
}

// 台名区：32px 单行（1.4.4 起加高到 80）
// 主区：收音 = 48px 七段大数字（原厂同款）；网络 = 32px 中文台名
// 主区：收音 = 48px 七段大数字；网络 = 36px 台名
// ============================================================
//  主区绘制（1.39.0 按用户要求重排）
//
//  收音：  中间「FM 98.4」点阵大字（**不再用像素字**）
//          左下角「调频 / 中波 / 长波 / 短波」小字
//  网络：  中间台名点阵大字（清洗掉 FM/数字频段），太长就**滚动**
//          左下角「网络」小字
//  有声书：中间「第 3 集 标题」放大居中，左下角还是波段名
//
//  「左下角的小字」是用户明确要的位置：不占中间、不抢台名，
//  但低头一看就知道现在在哪个波段。
// ============================================================
// 主区是否有超长文字在滚（uiLoop 用它决定要不要放宽节流）
static bool mainScrolling() {
  // 1.46.3：学习模式下台名照常画（提示只在第二行），所以这里不再排除它
  if (!A.screenOn || A.fmMode || A.volHold) return false;
  const int idx = (A.preview >= 0) ? A.preview : A.cur;
  if (idx < 0 || idx >= g_stN) return false;
  char nm[80];
  cleanName(g_st[idx].name, nm, sizeof(nm));
  return textWS(nm, FS36) > SCR_W - 12;
}

// ============================================================
//  1.39.0：超长文字滚动
//
//  为什么必须滚动：老人要看的台名常常是「FM882 成都故事广播」这种，
//  清洗掉 FM882 之后还有 7 个字，36px 下要 250px —— 320px 屏勉强，
//  但「中国国际广播电台轻松调频」这种就必然超。
//  截断加省略号（旧做法）的问题是**丢信息**：老人永远不知道后半截是什么。
//
//  做法：整串从左往右匀速移，走到尾停 1.2 秒再跳回开头（循环）。
//    速度 24px/秒：一屏 320px 走完约 13 秒，比人读字的速度慢一点，跟得上。
//    只有**真的超宽**才滚，不超宽就老老实实居中 —— 屏幕上永远有东西在动，
//    会让人分心。
//
//  需要每帧重画主区：uiLoop 里对「正在滚动」的情况放宽节流（见 uiLoop）。
// ============================================================
#define SCROLL_SPEED_PX_S   24
#define SCROLL_HOLD_MS      1200

static uint32_t s_scrollAt = 0;      // 开始滚的时刻
static uint32_t s_scrollKey = 0;     // 当前在滚的是哪条文字（变了就重置）

static void drawScrolling(const char* text, int y, uint16_t fg, int fs) {
  const int avail = SCR_W - 12;
  const int tw = textWS(text, fs);

  if (tw <= avail) {                       // 不超宽：居中，静止
    cnCenterS(text, y, fg, C_BG, fs);
    return;
  }

  // 超宽：算偏移量
  const uint32_t now = millis();
  // 用文字指针 + 长度做个轻量 key，换台/换集时把滚动位置归零
  const uint32_t key = (uint32_t)(uintptr_t)text ^ ((uint32_t)tw << 16) ^ (uint32_t)fs;
  if (key != s_scrollKey) { s_scrollKey = key; s_scrollAt = now; }

  const int span = tw - avail + 24;         // 要走的距离（多留 24px 让它完全出去）
  const uint32_t cycleMs = (uint32_t)(span * 1000 / SCROLL_SPEED_PX_S) + SCROLL_HOLD_MS;
  uint32_t t = (now - s_scrollAt) % cycleMs;
  int off;
  if (t < SCROLL_HOLD_MS) {
    off = 0;                                // 先停一下，让人看清开头
  } else {
    off = (int)((t - SCROLL_HOLD_MS) * SCROLL_SPEED_PX_S / 1000);
    if (off > span) off = span;
  }

  // 逐字画：只画落在可视区里的字（不裁的话会画到屏幕外，浪费并口带宽）
  const int baseX = 6 - off;
  int x = baseX;
  const uint8_t* p = (const uint8_t*)text;
  const int H = heightOf(fs);
  while (*p) {
    uint16_t cp;
    const uint8_t* q = nextCp(p, cp);
    const CnGlyph* g = glyphS(cp, fs);
    const int w = g ? g->w : H;
    if (x + w > 0 && x < SCR_W) {
      P->fillRect(x, y, w, H, C_BG);
      if (g) P->drawBitmap(x, y, blobS(g, fs), w, H, fg);
    }
    x += w;
    p = q;
  }
}

// 1.46.2：上一次 drawMain 真正画出来的那个索引。
//   snapshot 原来记的是"存快照那一刻的 A.cur"，而 A.cur 由 audioTask 并发改。
//   若它在 drawMain 之后、snapshot 之前变了，快照就记成新值 ⇒ 下一帧认为
//   "已经画过了"不再重画，屏上永久停在旧台名，声音却已是新台。
//   记"画出来的那个"就自然闭合这个竞态。
static int  s_paintIdx = -99;

static void drawMain() {
  s_paintIdx = -99;          // 1.46.2：先清，只有走到画台名那一步才赋值
  mark(RGN_MAIN);
  P->fillRect(0, MAIN_Y, SCR_W, MAIN_H, C_BG);

  // ---- 按住旋钮调音量：整块换成「音量 7」----
  //   1.39.0 起音量是 1..10 的档位，不再显示 0..63/100
  if (A.volHold) {
    char vb[16];
    const int showV = A.fmMode ? A.fmVol : A.vol;
    snprintf(vb, sizeof(vb), "音量 %d", showV);
    cnCenterS(vb, MAIN_Y + (MAIN_H - BIG_PX) / 2, C_NAME, C_BG, FS36);
    return;
  }

  // ---- 收音 ----
  if (A.fmMode) {
    if (fmScanBusy()) {
      char sb[24];
      // 1.46.42：把「已经找到几个」也写出来 —— 光一个百分比，用户没法判断
      //   机器是在干活还是卡住了；有数字在涨，就知道它在搜。
      snprintf(sb, sizeof(sb), "正在搜台 %d%%  已找到 %d 个",
               fmScanProgress(), fmScanCount());
      cnCenterS(sb, MAIN_Y + (MAIN_H - BIG_PX) / 2, C_NAME, C_BG, FS36);
      return;
    }
    // 1.46.59：**删掉「没有台」这一屏**。
    //   它原来判的是"搜台表里一个台都没有"，而搜台现在改手动、不再自动跑；
    //   留着它的话，一台没搜过的机器手动调到 98.0 正听着，屏上却写着"没有台"，
    //   用户会以为机器坏了。收音本来就不需要台表：频率是多少就显示多少。
    // 「FM 98.4」—— 点阵大字，FM 和数字之间一个空格
    const int band = radioBand(A.fmFreq);
    char f[20];
    if (band == RADIO_FM)
      snprintf(f, sizeof(f), "FM %d.%d", A.fmFreq / 1000, (A.fmFreq % 1000) / 100);
    else
      snprintf(f, sizeof(f), "%s %d", bandTextShort(), A.fmFreq);

    // 1.40.0：48px 粗体（用户要「频段数字加大加粗」）。
    //   主区 80px 高，48px 字纵向居中后上下各 16px；
    //   左下角那个 16px 波段名在 y=82..98，和居中的大字横向不重叠
    //   （大字居中后 x 约 66..254，波段名在 x=6..40）。
    cnCenterS(f, MAIN_Y + (MAIN_H - 48) / 2 - 2, C_NAME, C_BG, FS48);

    // 左下角：波段名（16px 小字）
    cnTextS(bandTextShort(), 6, MAIN_Y + MAIN_H - 22, C_SUB, C_BG, FS16);
    return;
  }

  // ---- 网络：台名（清洗 + 超长滚动）----
  int idx = (A.preview >= 0) ? A.preview : A.cur;
  s_paintIdx = idx;          // 1.46.2：记下这一帧画的是哪个台，snapshot 用它
  const char* raw = (idx >= 0 && idx < g_stN) ? g_st[idx].name : "没有电台";

  // 1.46.3：遥控器学习模式**不再顶掉台名**。原来 raw 被换成学习提示，
  //   屏上第一行只剩「按 下一台 (1/7)」—— 用户看到的就是「没有台名了」。
  //   现在台名照旧，学习提示挪到第二行小字（原来放「第N集」的位置）。
  const bool learn = inputLearnActive();

  char nm[80];
  cleanName(raw, nm, sizeof(nm));

  const uint16_t col = (A.preview >= 0) ? C_SUB : C_NAME;

  // 主区左上留 6px，右边给 6px —— 滚动时文字从右边出去
  const int avail = SCR_W - 12;
  const int tw = textWS(nm, FS36);

  // ---- 遥控器学习模式：第一行仍是台名，第二行才是「按 下一台 (1/7)」----
  if (learn) {
    drawScrolling(nm, MAIN_Y + 8, col, FS36);
    drawScrolling(inputLearnHint(), MAIN_Y + MAIN_H - 20, C_ACCENT, FS16);
    return;
  }

  // ---- 有声书：书名 + 集标题（名称放大居中，波段名在左下角）----
  const int bk = episBookOfStation(idx);
  if (bk >= 0) {
    const int e = episCur(bk);
    const char* tt = episTitle(bk, e);
    char sub2[96];
    if (tt && *tt) snprintf(sub2, sizeof(sub2), "第%d集 %s", e + 1, tt);
    else           snprintf(sub2, sizeof(sub2), "第%d集", e + 1);

    // 书名（36px，超长滚动）
    drawScrolling(nm, MAIN_Y + 8, col, FS36);
    // 集标题（16px，超长也滚动，但那是第二行）
    drawScrolling(sub2, MAIN_Y + MAIN_H - 20, C_SUB, FS16);
    cnTextS("网络", 6, MAIN_Y + MAIN_H - 22, C_SUB, C_BG, FS16);
    return;
  }

  // ---- 普通网络台：台名放大居中，超长滚动 ----
  drawScrolling(nm, MAIN_Y + (MAIN_H - BIG_PX) / 2 - 4, col, FS36);
  cnTextS("网络", 6, MAIN_Y + MAIN_H - 22, C_SUB, C_BG, FS16);

  // 1.46.6：右下角小字「跳过 N」。
  //   为什么非要有：台被自动藏起来之后，用户看到「12/300」却转不到某些台，
  //   没有这行字他会以为台被删了。放在第二行右端、16px 灰字，不抢台名。
  //   只在普通台显示（有声书那一行是集标题，整行都在滚动）。
  const int offN = offairHidden();
  if (offN > 0) {
    char ob[16];
    snprintf(ob, sizeof(ob), "跳过%d", offN);
    cnTextS(ob, SCR_W - 6 - textWS(ob, FS16), MAIN_Y + MAIN_H - 22, C_SUB, C_BG, FS16);
  }
}

// 三个小框：状态 + 台序 / 信号 / 音量（原名称条、仪表、参数框压缩而成）
// 1.38.0：三个小方框（状态/信号/音量）整个去掉了 —— 用户明确要求。
//   它们各占 24px 高，而老人真正看的是「频率够不够大、有没有声音」。
//   状态信息没有丢：静音看右侧竖条变红，没网看顶栏波段名 + 信号柱。
// 三个框：状态+序号 | 信号柱+dB | 音量+进度条（1.38.3 恢复成 1.37 的样子）
//   字号从 16 提到 20：24px 的框里放 20px 字，上下各留 2px，比原来饱满。
// 三个框：状态+序号 | 信号柱+dB | 音量+进度条
//   1.38.4：字回到 16px。1.38.3 用 20px 是想「放大」，但 24px 高的框里
//   20px 的「播放」和「1/307」之间只剩 1px，反而更难读。
//   要放大的是**台名和波段名**，这三个框是次要信息，保持原样才对。
// 1.46.54：这一格要显示的那串「3/246」（有声书是「3/22」）。
//   抽成函数只有一个目的：**needBox 也拿它比较**。
//   以前「要不要重画」是按 idx 比的；一旦出现「idx 没变但该显示的字变了」
//   或反过来，屏上就停在旧数字，等下一次别的重绘才突然变对 —— 正是用户报的
//   「x台/x台 有时候不变，换了好几个台又突然变正确」。
static void boxOrdText(char* b, size_t n) {
  if (!n) return;
  int idx = (A.preview >= 0) ? A.preview : A.cur;
  if (idx < 0) idx = 0;
  if (A.fmMode) { b[0] = 0; return; }              // 收音模式这一格不显示台序
  const int bk1 = episBookOfStation(idx);
  if (bk1 >= 0) snprintf(b, n, "%d/%d", episCur(bk1) + 1, episCount(bk1));
  else          snprintf(b, n, "%d/%d", (g_stN > 0) ? (idx + 1) : 0, g_stN);
}

static void drawBoxes() {
  mark(RGN_BOX);
  const int y = BOX_Y, h = BOX_H;

  // ---- 框1：状态圆点 + 状态词 + 台序 ----
  P->fillRect(BOX1_X, y, BOX1_W, h, C_BAR_BG);
  int idx = (A.preview >= 0) ? A.preview : A.cur;
  if (idx < 0) idx = 0;
  char b[20];
  boxOrdText(b, sizeof(b));          // 1.46.54：和 needBox 用同一份逻辑
  const int bk1 = episBookOfStation(idx);
  int nw = textWS(b, FS16);

  const char* st = (A.fmMode || A.wifiOk) ? psText() : "断网";
  if (18 + textWS(st, FS16) + 8 + nw + 6 > BOX1_W) st = psTextShort();
  if (18 + textWS(st, FS16) + 8 + nw + 6 > BOX1_W && bk1 >= 0) {
    snprintf(b, sizeof(b), "%d", episCur(bk1) + 1);
    nw = textWS(b, FS16);
  }

  P->fillCircle(BOX1_X + 10, y + h / 2, 4, psColor());
  cnTextS(st, BOX1_X + 18, y + 4, C_NAME, C_BAR_BG, FS16);
  if (nw > 0) cnTextS(b, BOX1_X + BOX1_W - 6 - nw, y + 4, C_SUB, C_BAR_BG, FS16);

  // ---- 框2：信号柱 + dB ----
  P->fillRect(BOX2_X, y, BOX2_W, h, C_BAR_BG);
  int bars = 0;
  if (A.wifiOk) bars = (A.rssi > -55) ? 4 : (A.rssi > -67) ? 3 : (A.rssi > -78) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    const int bh = 4 + i * 3;
    P->fillRect(BOX2_X + 8 + i * 7, y + 19 - bh, 5, bh, (i < bars) ? C_COOL_PAINT : C_BG);
  }
  char r[12];
  snprintf(r, sizeof(r), "%d", A.rssi);
  cnTextS(r, BOX2_X + BOX2_W - 7 - textWS(r, FS16), y + 4, C_SUB, C_BAR_BG, FS16);

  // ---- 框3：音量 + 进度条（当前这一路的真实音量）----
  P->fillRect(BOX3_X, y, BOX3_W, h, C_BAR_BG);
  const int showVol = A.fmMode ? A.fmVol : A.vol;
  cnTextS("音量", BOX3_X + 6, y + 4, C_SUB, C_BAR_BG, FS16);
  snprintf(b, sizeof(b), "%d", showVol);
  cnTextS(b, BOX3_X + BOX3_W - 6 - textWS(b, FS16), y + 4, C_NAME, C_BAR_BG, FS16);

  const int vx = BOX3_X + 6, vw = BOX3_W - 12;
  P->fillRect(vx, y + 21, vw, 2, C_BG);
  // 1.44.0：两路量程不同（FM 0..63 / 网络 0..21），条子要按当前那一路画，
  //   否则「音量 40」在 FM 下能画满、在网络下会溢出框外。
  const int vmax = A.fmMode ? FVOL_MAX : NVOL_MAX;
  int fw = (showVol * vw) / vmax;
  if (fw > 0) P->fillRect(vx, y + 21, fw, 2, C_COOL_PAINT);
}

// 台序 → FM 频率（返回 ×10 的整数，1042 表示 104.2）
// 只给「台名里没写频率」的网络电台用：按组内序号摊到 87.5~108.0，图个逐台往前挪的手感
static int freq10(int i, int n) {
  if (n <= 1) return 975;                       // 单台放正中间
  if (i < 0) i = 0;
  if (i > n - 1) i = n - 1;
  return 875 + (int)((long)(1080 - 875) * i / (n - 1));
}

// 台名里写着真实频率的，盘上就按它摆指针：FM89.1 / 电台914 / 1008 / FM100
//   · 带小数点：89.1 → 891、100.8 → 1008（只取一位小数）
//   · 纯数字：先按「末位是十分位」算（994 → 99.4、1008 → 100.8），
//     落不进波段再按「整兆赫」算（100 → 100.0）
//   · 落在 64.0~108.0 之外的一律不认：AM1161、集数 001 这些不会被当成频率
static int nameFreq10(const char* s) {
  if (!s) return -1;
  const char* p = s;
  while (*p) {
    if (*p < '0' || *p > '9') { p++; continue; }
    const char* q = p;
    long v = 0;
    int  nd = 0;
    while (*q >= '0' && *q <= '9') {            // 整段数字
      if (nd < 5) { v = v * 10 + (*q - '0'); nd++; }
      q++;
    }
    if (nd > 4) { p = q; continue; }            // 5 位以上：年份 / 流水号，不是频率
    int f = -1;
    if (*q == '.') {                            // 89.1 / 100.8
      const char* r = q + 1;
      if (*r >= '0' && *r <= '9') {
        f = (int)v * 10 + (*r - '0');
        q = r + 1;
        while (*q >= '0' && *q <= '9') q++;     // 后面的小数位直接丢掉
      }
    }
    if (f < 0) {                                // 纯数字：两种读法挑一个能落进波段的
      const int tenths = (int)v;                // 994 → 99.4
      const int whole  = (int)v * 10;           // 100 → 100.0
      if (tenths >= FM_BAND_BOTTOM && tenths <= FM_BAND_TOP)    f = tenths;
      else if (whole >= FM_BAND_BOTTOM && whole <= FM_BAND_TOP) f = whole;
    }
    if (f >= FM_BAND_BOTTOM && f <= FM_BAND_TOP) return f;
    p = (q > p) ? q : p + 1;
  }
  return -1;
}

// 这台在盘上指到哪：台名写了真实频率就用它，没写才按序号编
static int stationFreq10(int idx, int n) {
  if (idx >= 0 && idx < n) {
    const int f = nameFreq10(g_st[idx].name);
    if (f > 0) return f;
  }
  return freq10(idx, n);
}

// 调谐盘（1.4.4 真机调谐样式）：指针钉在屏幕正中不动，刻度条随频率滚动
//   · 1.10.0：收音模式按「当前波段的频道栅格」铺刻度 —— 中波/长波 9kHz、短波 5kHz、
//     调频 0.1MHz。一个刻度就是一格，转一格刻度正好挪 DIAL_PX 像素；
//     跨波段时栅格自动换一套（长波 9kHz → 中波 9kHz → 调频 0.1MHz → 短波 5kHz）
//   · 网络台仍按「台名里写的 FM 频率」铺（那 65 个带频率的台）
// ============================================================
//  调谐刻度（1.38.0：照原厂 drawSprite 的刻度条重做）
//
//  原厂做法（320×170 屏）：
//    40 条刻度，每 8px 一条（正好铺满 320），刻度宽 2px
//    i==20 那条（屏幕正中）是红的，其余 0xC638 灰
//    temp%10==0 → 长刻度 30px 高 + 数字；%5==0 → 20px；其余 → 10px
//    指针是红三角 + 一根红竖线从三角下面一直扎到屏底
//
//  我们原来那版的三个毛病，这次一起修掉：
//    ① 深青绿"蓝条"底色 → 改纯黑（原厂就是黑底）
//    ② 刻度 1px 太细、间距 5px 太密 → 改 2px / 8px
//    ③ 数字每 2MHz 一个、16px 宽 24px 挤在 50px 间距里 → 会重叠。
//       现在按「数字实际宽度」做重叠保护，画之前量一遍，放不下就跳过。
// ============================================================
// ============================================================
//  刻度条（1.38.3：改回 1.37 的滚动式，但稀疏、加粗、加短线、数字转灰）
//
//  用户原话：「底部刻度改回原来的，刻度稀疏一点加粗，短线加，数字灰一点不要纯白」
//
//  三个"改回"的点：
//    · 指针钉在屏幕正中，刻度随频率滚动（1.38.0 改成原厂那种固定 40 格是错的）
//    · 每格代表一个频道（调频 0.1MHz / 中波 9kHz / 短波 5kHz）
//    · 波段两头之外不画假刻度
//
//  三个"加强"的点：
//    · 稀疏：DIAL_PX 5 → 8（1MHz 从 50px 变 80px，数字有了呼吸空间）
//    · 加粗：刻度线 1px → 2px（并口屏 1px 竖线在强光下几乎看不见）
//    · 加短线：从三档（3/6/16）变四档（5/10/16/26），最密的 0.1MHz 也有 5px 短线
//    · 数字灰：C_DIAL_FG（近白）→ C_SUB（灰）。纯白数字压过台名，喧宾夺主。
// ============================================================
// 刻度条（滚动式）：指针居中、刻度随频率滚动
//   用户要求：稀疏（DIAL_PX 8）、加粗（2px 宽）、加短线（5px 那档）、
//             数字转灰（C_SUB，不用纯白）
// 刻度条（滚动式）：指针居中、刻度随频率滚动
//   1.38.5：去掉倒三角；刻度线 2px → **3px**（用户要求「线条加粗」）
static void drawTuner() {
  mark(RGN_TUN);
  P->fillRect(0, TUN_Y, SCR_W, TUN_H, C_BG);

  const int numY = TUN_NUM_Y;
  const int base = SCR_H - 1;
  const int LW   = 3;                    // 刻度线宽度（1.38.5：2 → 3）

  if (A.fmMode) {
    const int band = radioBand(A.fmFreq);
    const int step = radioBandStep(band);
    const int lo   = radioBandLo(band);
    const int hi   = radioBandHi(band);
    const int curT = (A.fmFreq - lo) / step;
    const int half = DIAL_CX / DIAL_PX + 1;

    for (int t = curT - half; t <= curT + half; t++) {
      const int k = lo + t * step;
      if (k < lo || k > hi) continue;
      const int x = DIAL_CX + (t - curT) * DIAL_PX - LW / 2;   // 线宽变了，左边界跟着移
      if (x < 0 || x + LW > SCR_W) continue;
      int hh;
      if      (k % (step * 10) == 0) hh = 16;
      else if (k % (step * 5)  == 0) hh = 10;
      else if (k % (step * 2)  == 0) hh = 6;
      else                           hh = 3;
      P->fillRect(x, base - hh + 1, LW, hh, (k % (step * 10) == 0) ? C_DIAL_MK : C_TICK);
    }

    int lastRight = -999;
    for (int t = curT - half; t <= curT + half; t++) {
      const int k = lo + t * step;
      if (k < lo || k > hi) continue;
      if (k % (step * 10) != 0) continue;
      char b[10];
      if (band == RADIO_FM) snprintf(b, sizeof(b), "%d.%d", k / 1000, (k % 1000) / 100);
      else                  snprintf(b, sizeof(b), "%d", k);
      const int tw = textWS(b, FS16);
      const int tx = DIAL_CX + (t - curT) * DIAL_PX - tw / 2;
      if (tx < 1 || tx + tw > SCR_W - 1) continue;
      if (tx < lastRight + 4) continue;
      cnTextS(b, tx, numY, C_SUB, C_BG, FS16);
      lastRight = tx + tw;
    }

    // 指针：只有一根竖线（三角已去掉）
    P->fillRect(DIAL_CX - 1, TUN_PTR_Y, 3, base - TUN_PTR_Y + 1, C_DIAL_PTR);
    return;
  }

  // 网络台：按台名里的 FM 频率铺
  const int n = g_stN;
  int idx = (A.preview >= 0) ? A.preview : A.cur;
  if (idx < 0) idx = 0;
  if (idx > n - 1) idx = n - 1;
  const int fcur = (n > 0) ? stationFreq10(idx, n) : 975;

  const int half = DIAL_CX / DIAL_PX + 1;
  for (int t = -half; t <= half; t++) {
    const int f10 = fcur + t;
    const int x = DIAL_CX + t * DIAL_PX - LW / 2;
    if (x < 0 || x + LW > SCR_W) continue;
    int hh;
    if      ((f10 % 10) == 0) hh = 16;
    else if ((f10 % 5)  == 0) hh = 10;
    else if ((f10 % 2)  == 0) hh = 6;
    else                      hh = 3;
    P->fillRect(x, base - hh + 1, LW, hh, ((f10 % 10) == 0) ? C_DIAL_MK : C_TICK);
  }

  int lastRight = -999;
  for (int t = -half; t <= half; t++) {
    const int f10 = fcur + t;
    if ((f10 % 10) != 0) continue;
    if (f10 < FM_BAND_BOTTOM || f10 > FM_BAND_TOP) continue;
    char b[10];
    snprintf(b, sizeof(b), "%d.%d", f10 / 10, f10 % 10);
    const int tw = textWS(b, FS16);
    const int tx = DIAL_CX + t * DIAL_PX - tw / 2;
    if (tx < 1 || tx + tw > SCR_W - 1) continue;
    if (tx < lastRight + 4) continue;
    cnTextS(b, tx, numY, C_SUB, C_BG, FS16);
    lastRight = tx + tw;
  }

  if (n > 0) {
    const uint16_t pc = (A.preview >= 0) ? C_SUB : C_DIAL_PTR;
    P->fillRect(DIAL_CX - 1, TUN_PTR_Y, 3, base - TUN_PTR_Y + 1, pc);
  }
}

// ---------------- 增量刷新 ----------------
static int  l_cur = -99, l_prev = -99, l_ps = -99, l_vol = -1;

static int  l_wifi = -1, l_rssi = -99, l_muted = -1, l_learn = -1, l_cnt = -1, l_grp = -1;
static int  l_fm = -1, l_fmFreq = -1;
static int  l_offN = -1;      // 1.46.6：上一帧「跳过 N」里的 N
static int  l_volHold = -1;    // 1.26.0：上一帧是否处于「按住调音量」，及当时的数字
static int  l_volHoldVal = -1;
static int  l_batt = -999;     // 1.19.0：电量（按 10% 一档 + 低电/充电标志位）
static char l_name[40] = {0};
static char l_clk[8]   = {0};
// 1.46.33：上一帧画的「第几集」。用户报「有声书名字底下的第 X 集不跟着变」——
//   换集时台没变（A.cur 一样）、台名也没变，needFreq 全都不成立，
//   于是增量重绘把整块台名区跳过去了。这里单独记一个集号参与比较。
static int  l_epi = -2;
// 1.46.43：上一帧画的「搜台进度 / 已找到个数」。
//   搜台期间 A.cur、A.fmMode、A.fmFreq 全都不变，needFreq 一个都不成立，
//   于是「正在搜台 0%」永远停在第一帧 —— 用户报的「一直百分之 0」就是这个。
static int  l_scanPct = -1, l_scanN = -1;
// 1.46.54：左边小框画出去的台序文字（"3/246"），用来判断「该不该重画」
static char l_ord[20] = "";

// 1.22.0：顶栏显示的是「当前模式那一路」的音量，所以快照和比较都要按同一路取。
//   否则在收音里调音量，comparison 用的是网络那个值 ⇒ 顶栏数字不刷新。
static inline int shownVol() { return A.fmMode ? A.fmVol : A.vol; }

// 1.46.33：当前该画的集号（0 基）；不是有声书返回 -1
static int curEpi() {
  const int i = (s_paintIdx >= 0) ? s_paintIdx : A.cur;
  if (i < 0 || i >= g_stN) return -1;
  const int pb = episBookOfStation(i);
  return (pb >= 0) ? episCur(pb) : -1;
}

static void snapshot(const char* clk) {
  l_ps = A.ps; l_vol = shownVol(); l_muted = A.muted; l_wifi = A.wifiOk;
  l_rssi = A.rssi;
  // 1.46.2：记"画出来的那个"，不是"现在的 A.cur"（A.cur 由 audioTask 并发改）
  l_cur = (s_paintIdx >= 0) ? s_paintIdx : A.cur;
  l_prev = A.preview;
  l_learn = inputLearnActive(); l_cnt = g_stN; l_grp = A.group;
  l_fm = A.fmMode ? 1 : 0;
  l_volHold = A.volHold ? 1 : 0;
  l_volHoldVal = A.volHold ? (A.fmMode ? A.fmVol : A.vol) : -1;
  l_fmFreq = A.fmFreq;
  l_offN = offairHidden();
  // 1.38.2：电量条只有 4 档，所以按「档位」比较就够 ——
  //   按 10% 比会在 71%→69% 时白重画一次顶栏（并口屏重画是贵的）。
  // 图标按真实百分比填充，所以快照也按 5% 一档 —— 再粗就会出现
  // 「电量掉了一点但图标不动」，用户会以为坏了。
  l_batt = batteryOk() ? (batteryPct() / 5 * 5 + (batteryLow() ? 1000 : 0)
                                                + (batteryCharging() ? 500 : 0))
                       : -999;
  strncpy(l_name, (l_cur >= 0 && l_cur < g_stN) ? g_st[l_cur].name : "", sizeof(l_name) - 1);
  {
    const int pb = (l_cur >= 0 && l_cur < g_stN) ? episBookOfStation(l_cur) : -1;
    l_epi = (pb >= 0) ? episCur(pb) : -1;
  }
  boxOrdText(l_ord, sizeof(l_ord));      // 1.46.54：记下这一帧画出去的台序
  strncpy(l_clk, clk, sizeof(l_clk) - 1);
  l_clk[sizeof(l_clk) - 1] = 0;
  // 1.46.43：搜台进度也要进快照，否则 needFreq 永远为真（每 40ms 重画一次）
  l_scanPct = fmScanProgress();
  l_scanN   = fmScanCount();
}

static void redrawAll() {
  // 注意：这里用 fillRect(0,0,SCR_W,SCR_H) 而不是 fillScreen()。
  //   TFT_eSPI::fillScreen 用的是基类的 _width/_height（170×320，是**真屏**的
  //   长宽），画到 sprite 上会按错的尺寸抹，留下一片没清干净的地方。
  P->fillRect(0, 0, SCR_W, SCR_H, C_BG);
  drawTop(); drawMain(); drawBoxes(); drawTuner();
  char clk[8] = "--:--";
  netClockStr(clk, sizeof(clk));
  snapshot(clk);
  s_nFull++;
  pushAll();
}

void uiBegin() {
  Serial.print("[ui] tft.init() ... ");
  tft.init();
  Serial.println("done");
  tft.setRotation(SCR_ROT);

  // 1.38.1：立刻 DISPLAY OFF。
  //   ST7789 上电后显存是随机内容，init 序列并不会把它清掉。
  //   只要这时屏还在显示（DISPON），看到的就是一片雪花 —— 这就是「开屏闪白」。
  //   先关显示，等首帧画完再开（见 uiShow）。
  tft.writecommand(0x28);          // DISPOFF
  Serial.println("[ui] 显示已关（等首帧画完再开，避免上电白闪）");

  // 背光：先把脚设成「灭」的静态电平，再交给 LEDC，之后不要再动 GPIO
  // （1.27.2 曾在这里用 pinMode+digitalWrite 把 LEDC 顶掉，导致背光被按死）
  digitalWrite(PIN_TFT_BL, BL_ON_LEVEL == HIGH ? LOW : HIGH);  // 先写锁存器
  pinMode(PIN_TFT_BL, OUTPUT);                                // 再定方向
  delay(2);
  ledcSetup(BL_CH, 5000, 8);
  // 1.43.0：先按「灭」把占空比写进 LEDC，再接引脚。
  //   ledcAttachPin 的瞬间会把通道里**残留**的占空比直接输出到脚上；
  //   先接后写的话，中间那一小段就是按旧值亮的（也算一次闪）。
  s_brightApplied = -1;
  ledcWrite(BL_CH, (BL_ON_LEVEL == HIGH) ? 0 : 255);
  ledcAttachPin(PIN_TFT_BL, BL_CH);
  setBright(0);                    // 1.38.1：这里不再开 100，等 uiShow() 才亮
  Serial.printf("[ui] 背光 IO%d 已接管（%s 有效），等首帧画完再亮\n",
                PIN_TFT_BL, BL_ON_LEVEL == HIGH ? "高" : "低");

  // ---- 1.36.0：建离屏帧缓存 ----
  //   这台机器是 N16R8（16MB flash + 8MB **八线 OPI** PSRAM），但 board json
  //   用的是 esp32-s3-devkitc-1（N8，声明「无 PSRAM」），所以 Arduino 的
  //   psramFound() 一上来是假的 —— 实测串口打出「PSRAM 共 0 KB」。
  //   而 platformio.ini 的 memory_type=qio_opi 让链接器用的是 qio_opi 那套库，
  //   里面 CONFIG_SPIRAM_BOOT_INIT=1 ⇒ **开机时 IDF 已经把 OPI PSRAM 初始化好
  //   并挂进堆了**，只是 Arduino 层不认。
  //   所以这里先主动 psramInit() 认领一次，再按「内部堆有没有被吃掉」验证：
  //   108,800 字节若真从内部 RAM 扣，内部堆会立刻掉一百多 KB，那就说明 PSRAM
  //   没生效 —— 这种情况宁可回退直绘（闪，但稳），也不能把 WiFi/音频的内存吃了。
  Serial.printf("[ui] PSRAM 认领前：found=%d 总量=%u KB 空闲=%u KB\n",
                psramFound(),
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  if (!psramFound()) psramInit();
  Serial.printf("[ui] PSRAM 认领后：found=%d 总量=%u KB 空闲=%u KB\n",
                psramFound(),
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  spr.setColorDepth(16);
  const size_t inBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  void* fb = spr.createSprite(SCR_W, SCR_H);
  const size_t inAfter  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t inUsed   = (inBefore > inAfter) ? (inBefore - inAfter) : 0;
  // 内部堆吃掉超过 8KB 就说明这块画布落在内部 RAM 上 —— 不要它
  s_fb = (fb != nullptr) && (inUsed < 8192);
  if (fb && !s_fb) spr.deleteSprite();
  P = s_fb ? (TFT_eSPI*)&spr : (TFT_eSPI*)&tft;
  Serial.printf("[ui] 帧缓存 %s（画布 %s，内部堆占用 %u KB；内部空闲 %u KB）\n",
                s_fb ? "已启用" : "不可用，退回直绘",
                fb ? "拿到" : "没拿到",
                (unsigned)(inUsed / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

  P->fillRect(0, 0, SCR_W, SCR_H, C_BG);
  pushAll();
}

// ============================================================
//  1.38.1：点亮屏幕（首帧画完之后才调）
//
//  为什么要单独一个函数、为什么要在 setup 里手动调：
//    开屏那一下的正确顺序是
//        关显示 → 清显存 → 画首帧 → 推屏 → **开显示** → 背光渐亮
//    原来把「开背光」放在了 uiBegin 里，等于顺序反了：
//    背光一亮，显存还是随机的，看到的就是一片白，然后才被清黑。
//
//  背光做成 8 级渐亮（约 130ms）：直接跳到最亮会很刺眼，
//  而且电流突变会让电源晃一下（这块板子电池供电时能看出来）。
// ============================================================
void uiShow() {
  if (s_show) return;
  s_show = true;

  tft.writecommand(0x29);          // DISPON：现在屏上已经是首帧了，可以显示
  s_brightApplied = -1;            // 强制重设一次亮度

  const int target = A.bright > 0 ? A.bright : 100;
  for (int i = 1; i <= 8; i++) {
    setBright(target * i / 8);
    delay(16);
  }
  Serial.printf("[ui] 屏幕点亮（亮度 %d）\n", target);
}

// ============================================================
//  1.43.0：开机亮度「一次到位」
//
//  uiShow() 是在 storeLoadCfg() **之前**调的（那时屏上还是启动画面，
//  早点亮看起来自然），所以它渐亮到的是 A.bright 的默认值 100。
//  等配置读进来，A.brightMax 可能是用户在 App 里设的 60 —— 于是开机 1.5 秒后
//  第一帧 uiLoop 会把背光从 100 一步砸到 60，看着又是「闪了一下」。
//
//  这里在配置和电源基线都就绪之后，把亮度对齐到最终值。**用同样的渐变节奏**
//  （130ms）：差得多的时候一刀切下去，眼睛看到的就是「闪了一下」，渐变才算"调光"。
//  同时把空闲计时清零：setup 里 LittleFS 挂载、WiFi 起、收音芯片初始化花掉的
//  时间不该被算成「无操作」，否则一进主界面就可能直接触发省电降亮。
// ============================================================
void uiBrightSettle() {
  if (!s_show) return;
  int b = (A.brightMax > 0) ? A.brightMax : 100;
  const int cap = powerBrightCap();
  if (b > cap) b = cap;
  if (b < 1) b = 1;
  const int from = (s_brightApplied < 0) ? b : s_brightApplied;
  A.bright = b;
  if (b > from + 8 || b < from - 8) {
    for (int i = 1; i <= 8; i++) {          // 8 级 × 16ms ≈ 130ms，和开机渐亮一致
      setBright(from + (b - from) * i / 8);
      delay(16);
    }
  } else {
    s_brightApplied = -1;                   // 差得少：直接写，不吃去重
    setBright(b);
  }
  A.lastAct = millis();
  Serial.printf("[ui] 开机亮度锁定 %d%%（电源上限 %d%%，从 %d%% 渐变）\n", b, cap, from);
}

void uiForceRedraw() { redrawAll(); }

void uiSplash(const char* line1, const char* line2) {
  P->fillRect(0, 0, SCR_W, SCR_H, C_BG);
  cnCenter(line1, 62, C_WHITE, C_BG, true);
  if (line2) cnCenter(line2, 104, C_SUB, C_BG, false);
  pushAll();
}

// 首次开机引导：标题 + 四行说明，字大、行距宽
void uiInfoPage(const char* title, const char* l1, const char* l2, const char* l3, const char* l4) {
  P->fillRect(0, 0, SCR_W, SCR_H, C_BG);
  P->fillRect(0, 0, SCR_W, 3, C_ACCENT);
  cnCenter(title, 12, C_WHITE, C_BG, true);
  const char* ls[4] = { l1, l2, l3, l4 };
  int y = 52;
  for (int i = 0; i < 4; i++) {
    if (!ls[i] || !ls[i][0]) continue;
    P->fillCircle(14, y + 8, 3, C_COOL_PAINT);
    cnText(ls[i], 24, y, (i == 3) ? C_ACCENT : C_SUB, C_BG, false);
    y += 26;
  }
  pushAll();
}

// ---------------- 配对码 ----------------
// 平时永远不出现。只有用户在 App 里打开「配对码保护」后，
// 新手机要绑定时，屏幕上才给 6 个数字。
static char     s_pc[8]  = {0};
static uint32_t s_pcAt   = 0;
static bool     s_pcDrawn = false;

void uiPairCode(const char* code) {
  strncpy(s_pc, code ? code : "", sizeof(s_pc) - 1);
  s_pc[sizeof(s_pc) - 1] = 0;
  s_pcAt = millis();
  s_pcDrawn = false;
}

void uiPairCodeClear() {
  if (s_pc[0]) { s_pc[0] = 0; s_pcDrawn = false; A.dirty = true; }
}

bool uiPairCodeActive() { return s_pc[0] != 0; }

static void drawPairPage() {
  P->fillRect(0, 0, SCR_W, SCR_H, C_BG);
  P->fillRect(0, 0, SCR_W, 3, C_ACCENT);
  cnCenter("配对码", 16, C_SUB, C_BG, false);
  cnCenter(s_pc, 44, C_WHITE, C_BG, true);          // 32px 大字，1 米外也读得清
  // 1.46.5：这页也用于 4 位「手机连接码」，文案跟着位数走
  cnCenter(strlen(s_pc) == 4 ? "在手机上输入这 4 个数字"
                             : "在手机上输入这 6 个数字", 100, C_SUB, C_BG, false);
  cnCenter("输完就能控制收音机", 124, 0x8C71, C_BG, false);
  pushAll();
}

// 1.35.0 起的绘制节流（1.36.0 保留）。
//
//   1.35.0 的误判：以为「闪 = redrawAll() 被反复调用」（它是唯一 fillScreen 的地方）。
//   于是加了限速 —— 用户反馈「频率低了，但还是闪」。限速只改了节奏，没改动作：
//   只要还在「先抹底色、后画字」地直写屏，25Hz 就闪 25 次，7Hz 就闪 7 次。
//
//   1.36.0 才是正解：见文件上方「离屏帧缓存」那段 —— 抹黑只发生在内存里。
//   （今天这层限速留着不吃亏：它把「同一帧里重复画」的浪费挡掉了。）
//     整屏重绘（redrawAll）：最快 7 次/秒
//     局部重绘（顶栏/频率/状态框/调谐盘）：最快 25 次/秒
static uint32_t s_lastFull = 0;
static uint32_t s_lastPaint = 0;

void uiLoop() {
  if (!A.screenOn) {
    if (s_brightApplied != 0) setBright(0);
    return;
  }
  // 每次都试一下：setBright 内部有去重，只有真的变了才动 ledcWrite。
  // 早先只判 s_brightApplied==0 才写，省电变暗和 App 改亮度都不会生效。
  // 1.21.0：再套一层电源上限 —— 过热或满电时背光自己降下来，不碰 A.bright
  //         （用户设的值留着，温度回落 / 拔掉充电器就恢复）。
  {
    int b = A.bright;
    const int cap = powerBrightCap();
    if (b > cap) b = cap;
    // 1.38.1：首帧没画好之前，背光一直按 0 —— 否则 uiLoop 一跑就把背光拉到 A.bright，
    //   又回到「先亮后背、显存随机」那个白闪现场。
    setBright(s_show ? b : 0);
  }

  // 配对码页盖住主界面，60 秒没人理就退回
  if (s_pc[0]) {
    if (millis() - s_pcAt > 60000) {
      s_pc[0] = 0;
      s_pcDrawn = false;
      A.dirty = true;
    } else {
      if (!s_pcDrawn) { s_pcDrawn = true; drawPairPage(); }
      return;
    }
  }

  const uint32_t nowMs = millis();

  // 整屏重绘限速：0.14s 内最多一次。A.dirty 保持不清，等时间到了再画，
  // 所以不会丢更新（只是晚一点）。
  if (A.dirty) {
    if (nowMs - s_lastFull >= 140) {
      A.dirty = false;
      s_lastFull = nowMs;
      s_lastPaint = nowMs;
      redrawAll();
    }
    return;
  }

  // 局部重绘限速：40ms 内最多一次。转旋钮时一格一次也会被合并到 ~25Hz，
  // 既不拖手感，也不会一片闪。
  //
  // 1.39.0：台名滚动时不能等 40ms —— 24px/秒 × 0.04s 还不到 1px，
  // 看上去会一顿一顿。滚动期间走 16ms 的快路（约 60fps）：
  // 只重画主区，顶栏/三个框/刻度都不碰，开销很小。
  if (mainScrolling()) {
    if (nowMs - s_lastPaint >= 16) {
      drawMain();
      flushRegions();
      s_lastPaint = nowMs;
    }
    return;
  }
  if (nowMs - s_lastPaint < 40) return;

  char clk[8] = "--:--";
  netClockStr(clk, sizeof(clk));
  bool clockChanged = (strcmp(clk, l_clk) != 0);

  const int battNow = batteryOk() ? (batteryPct() / 5 * 5 + (batteryLow() ? 1000 : 0)
                                                      + (batteryCharging() ? 500 : 0))
                                   : -999;
  bool needTop  = clockChanged || (l_wifi != (int)A.wifiOk) || (l_rssi / 8 != A.rssi / 8) ||
                  (l_vol != shownVol()) || (l_muted != (int)A.muted) || (l_grp != A.group) ||
                  (l_batt != battNow);
  bool needFreq = (l_cur != A.cur) || (l_prev != A.preview) ||
                  (l_offN != offairHidden()) ||
                  (l_learn != (int)inputLearnActive()) ||
                  (l_fm != (int)A.fmMode) || (A.fmMode && l_fmFreq != A.fmFreq) ||
                  ((A.cur >= 0 && A.cur < g_stN) && strcmp(l_name, g_st[A.cur].name) != 0) ||
                  // 1.46.33：同一台换集 → 台名/索引都没变，只有「第 N 集」变了
                  (l_epi != curEpi()) ||
                  // 1.46.43：搜台期间只有这两个数在变（见 l_scanPct 的说明）
                  (A.fmMode && (l_scanPct != fmScanProgress() || l_scanN != fmScanCount()));
  // 1.26.0：进出「音量」状态、以及音量数字变了，都只重画台名区。
  //   不能走 A.dirty（那是整屏 redrawAll），转旋钮时会一片闪。
  {
    const int vh = A.volHold ? 1 : 0;
    const int vv = A.volHold ? (A.fmMode ? A.fmVol : A.vol) : -1;
    if (vh != l_volHold || vv != l_volHoldVal) needFreq = true;
  }
  // 1.46.54：台序文字本身变了也要重画（见 boxOrdText 的说明）
  char ordNow[20];
  boxOrdText(ordNow, sizeof(ordNow));
  const bool ordChanged = (strcmp(l_ord, ordNow) != 0);
  bool needBox  = needTop || needFreq || ordChanged || (l_ps != (int)A.ps) || (l_cnt != g_stN);
  bool needTun  = needFreq || (l_cnt != g_stN);

  if (needTop)  drawTop();
  if (needFreq) drawMain();
  if (needBox)  drawBoxes();
  if (needTun)  drawTuner();

  if (needTop || needFreq || needBox || needTun) {
    flushRegions();          // 1.36.0：画完才推屏，屏上永远只有完整画面
    snapshot(clk);
    s_lastPaint = nowMs;
  }

  // 诊断：每 10 秒报一次「这一段时间里整屏重绘几次 / 推屏几次」。
  //   如果还闪，看这两个数就知道是「重绘太频繁」还是「根本不是重绘的事」。
  if (nowMs - s_diagAt > 10000) {
    s_diagAt = nowMs;
    if (s_nFull != s_nFullSeen || s_nPush != s_nPushSeen) {
      Serial.printf("[ui] 10s 内整屏重绘 %lu 次 / 推屏 %lu 次（帧缓存 %s）\n",
                    (unsigned long)(s_nFull - s_nFullSeen),
                    (unsigned long)(s_nPush - s_nPushSeen),
                    s_fb ? "开" : "关");
      s_nFullSeen = s_nFull;
      s_nPushSeen = s_nPush;
    }
  }
}
