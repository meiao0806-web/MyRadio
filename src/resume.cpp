// ============================================================
//  resume.cpp —— 有声书断点记忆（听到第几集、第几秒、码率）
//  见 resume.h。只占 8 条 × 24 B，写盘同样是「改动后 2 秒合并」，
//  跟 epis / album 一个节奏，不会因为一直听就把 flash 写坏。
// ============================================================
#include "resume.h"
#include "state.h"

#include <LittleFS.h>
#include <string.h>

#define RS_MAX      8
#define RS_FILE     "/resume.txt"
#define RS_SAVE_MS  2000

struct RsRec {
  int16_t book;      // 书号（epis 的书号，不是台号）
  int16_t ep;        // 集号（0 基）
  int32_t sec;       // 这一集听到第几秒
  int32_t kbps;      // 这一集的码率（续播定位用，0 = 不知道）
  // 1.46.38：m4a 的解码参数（ASC）。从中间起播时文件头（moov）被跳过，
  //   这三个数就是解码器唯一的依据；没记住就不能续，只能从头放。
  int16_t aot;
  int16_t ch;
  int32_t rate;
};

static RsRec    s_r[RS_MAX];
static int      s_n = 0;
static bool     s_dirty = false;
static uint32_t s_dirtyAt = 0;

static int findBook(int book) {
  for (int i = 0; i < s_n; i++)
    if (s_r[i].book == book) return i;
  return -1;
}

void resumeRemember(int book, int ep, int sec, int kbps, int aot, int ch, int rate) {
  if (book < 0 || ep < 0 || sec < 0) return;
  int i = findBook(book);
  if (i < 0) {
    if (s_n < RS_MAX) {
      i = s_n++;
    } else {
      i = 0;                                  // 满了就顶掉最老的一条，不报错
      for (int k = 1; k < s_n; k++)
        if (s_r[k].sec < s_r[i].sec) i = k;
    }
    s_r[i].book = (int16_t)book;
    s_r[i].ep = (int16_t)ep;
    s_r[i].sec = -1;
    s_r[i].kbps = 0;
    s_r[i].aot = 0;
    s_r[i].ch = 0;
    s_r[i].rate = 0;
  }
  // 0.5 秒以内的抖动不写盘，省 flash
  const bool changed = (s_r[i].ep != (int16_t)ep) ||
                       (s_r[i].sec < 0) ||
                       (sec - s_r[i].sec >= 1) ||
                       (s_r[i].sec - sec >= 2);
  s_r[i].ep = (int16_t)ep;
  s_r[i].sec = sec;
  if (kbps > 0) s_r[i].kbps = kbps;
  // ASC 一旦知道就一直留着（改集也不用重学，同一本书编码参数通常一致）
  if (aot > 0)      s_r[i].aot = (int16_t)aot;
  if (ch == 1 || ch == 2) s_r[i].ch = (int16_t)ch;
  if (rate > 0)     s_r[i].rate = rate;
  if (changed) {
    s_dirty = true;
    s_dirtyAt = millis();
  }
}

bool resumeGet(int book, int* ep, int* sec, int* kbps, int* aot, int* ch, int* rate) {
  const int i = findBook(book);
  if (i < 0) return false;
  if (ep)   *ep = s_r[i].ep;
  if (sec)  *sec = s_r[i].sec;
  if (kbps) *kbps = s_r[i].kbps;
  if (aot)  *aot = s_r[i].aot;
  if (ch)   *ch = s_r[i].ch;
  if (rate) *rate = s_r[i].rate;
  return s_r[i].sec >= 0;
}

int resumeSecOf(int book, int ep) {
  const int i = findBook(book);
  if (i < 0 || s_r[i].ep != (int16_t)ep) return -1;
  return s_r[i].sec;
}

void resumeClear(int book) {
  const int i = findBook(book);
  if (i < 0) return;
  s_r[i].sec = -1;
  s_r[i].kbps = 0;
  // ASC 不清：同一本书下一集编码参数通常一样，留着能省一次「从头放到学会」
  
  s_dirty = true;
  s_dirtyAt = millis();
}

static void rsFlush() {
  File f = LittleFS.open(RS_FILE, "w");
  if (!f) return;
  for (int i = 0; i < s_n; i++) {
    if (s_r[i].sec < 0) continue;
    // 1.46.38：后三列是 ASC。老固件读前四列就够，不会坏。
    f.printf("%d\t%d\t%d\t%d\t%d\t%d\t%d\n", (int)s_r[i].book, (int)s_r[i].ep,
             (int)s_r[i].sec, (int)s_r[i].kbps,
             (int)s_r[i].aot, (int)s_r[i].ch, (int)s_r[i].rate);
  }
  f.close();
}

void resumeBegin() {
  s_n = 0;
  s_dirty = false;
  if (!LittleFS.exists(RS_FILE)) return;
  File f = LittleFS.open(RS_FILE, "r");
  if (!f) return;
  while (f.available() && s_n < RS_MAX) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (!ln.length()) continue;
    int t1 = ln.indexOf('\t');
    if (t1 <= 0) continue;
    int t2 = ln.indexOf('\t', t1 + 1);
    if (t2 <= 0) continue;
    int t3 = ln.indexOf('\t', t2 + 1);
    if (t3 <= 0) continue;
    RsRec& r = s_r[s_n];
    r.book = (int16_t)ln.substring(0, t1).toInt();
    r.ep   = (int16_t)ln.substring(t1 + 1, t2).toInt();
    r.sec  = ln.substring(t2 + 1, t3).toInt();
    // 第 4 列之后可能还有 ASC（第 5/6/7 列）；老文件没有就当 0
    int rest = t3 + 1;
    int t4 = ln.indexOf('\t', rest);
    r.kbps = (t4 > 0 ? ln.substring(rest, t4) : ln.substring(rest)).toInt();
    r.aot = 0; r.ch = 0; r.rate = 0;
    if (t4 > 0) {
      int t5 = ln.indexOf('\t', t4 + 1);
      r.aot = (int16_t)(t5 > 0 ? ln.substring(t4 + 1, t5) : ln.substring(t4 + 1)).toInt();
      if (t5 > 0) {
        int t6 = ln.indexOf('\t', t5 + 1);
        r.ch = (int16_t)(t6 > 0 ? ln.substring(t5 + 1, t6) : ln.substring(t5 + 1)).toInt();
        if (t6 > 0) r.rate = ln.substring(t6 + 1).toInt();
      }
    }
    if (r.book >= 0 && r.ep >= 0 && r.sec >= 0) s_n++;
  }
  f.close();
  Serial.printf("[resume] 载入 %d 条断点\n", s_n);
}

void resumeTick() {
  if (s_dirty && millis() - s_dirtyAt > RS_SAVE_MS) {
    s_dirty = false;
    rsFlush();
  }
}
