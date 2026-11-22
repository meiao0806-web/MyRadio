// ============================================================
//  history.cpp —— 收听记录
//
//  计时模型：把「连着听同一台」看成一「段」。
//    · 每秒 tick 时，若正在播放且（组,台）没变 → 该段 +1 秒
//    · 换台 / 换组 / 停下来 → 结算这一段，累到条目里
//  这样不用在 audio 任务里插钩子，也不怕漏记。
// ============================================================
#include "history.h"
#include "store.h"
#include <LittleFS.h>
#include <time.h>

static const char* F_HIST = "/history.tsv";

static HistItem s_it[HIST_MAX];
static int      s_n = 0;

static DayStat  s_days[HIST_DAYS];     // s_days[0] = 今天
static int      s_dayN = 1;

// 当前这一段
static char     s_segName[HIST_NAME_LEN] = {0};
static char     s_segGrp[HIST_GRP_LEN]   = {0};
static uint32_t s_segSec  = 0;
static uint32_t s_segAt   = 0;         // 上一秒的时间戳
static bool     s_segOpen = false;

static bool     s_dirty   = false;
static uint32_t s_dirtyAt = 0;

// ------------------------------------------------------------
static uint32_t todayKey() {
  time_t t = time(nullptr);
  if (t < 1600000000) return 0;        // 还没同步时间
  struct tm tm;
  localtime_r(&t, &tm);
  return (uint32_t)(tm.tm_year + 1900) * 10000u + (tm.tm_mon + 1) * 100u + tm.tm_mday;
}

static uint32_t nowUnix() {
  time_t t = time(nullptr);
  return (t < 1600000000) ? 0 : (uint32_t)t;
}

// 跨天了就把今天插到最前面，旧的往后挪（只留 7 天）
static void rollDay() {
  uint32_t k = todayKey();
  if (k == 0) return;
  if (s_days[0].key == k) return;

  if (s_days[0].key == 0) {              // 第一次拿到时间
    s_days[0].key = k;
    s_dayN = 1;
    return;
  }
  for (int i = HIST_DAYS - 1; i > 0; i--) s_days[i] = s_days[i - 1];
  memset(&s_days[0], 0, sizeof(s_days[0]));
  s_days[0].key = k;
  if (s_dayN < HIST_DAYS) s_dayN++;
  s_dirty = true;
  s_dirtyAt = millis();
}

// 找条目：组 + 台名
static int findItem(const char* grp, const char* name) {
  for (int i = 0; i < s_n; i++)
    if (strcmp(s_it[i].name, name) == 0 && strcmp(s_it[i].grp, grp) == 0) return i;
  return -1;
}

static void copyStr(char* dst, size_t n, const char* src) {
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
  for (char* p = dst; *p; p++) if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
}

// 满了就淘汰「听得最少 + 最久没听」的那条
static int victim() {
  int w = 0;
  for (int i = 1; i < s_n; i++) {
    if (s_it[i].sec < s_it[w].sec ||
        (s_it[i].sec == s_it[w].sec && s_it[i].last < s_it[w].last)) w = i;
  }
  return w;
}

static int ensureItem(const char* grp, const char* name) {
  int i = findItem(grp, name);
  if (i >= 0) return i;
  if (s_n < HIST_MAX) i = s_n++;
  else i = victim();                  // 覆盖最没价值的一条
  memset(&s_it[i], 0, sizeof(s_it[i]));
  copyStr(s_it[i].name, sizeof(s_it[i].name), name);
  copyStr(s_it[i].grp,  sizeof(s_it[i].grp),  grp);
  return i;
}

// 按 last 降序排一遍（条目最多 48，选择排序足够）
static void sortRecent() {
  for (int i = 0; i < s_n - 1; i++) {
    int m = i;
    for (int j = i + 1; j < s_n; j++)
      if (s_it[j].last > s_it[m].last) m = j;
    if (m != i) { HistItem t = s_it[i]; s_it[i] = s_it[m]; s_it[m] = t; }
  }
}

// ------------------------------------------------------------
//  结算 / 开段
// ------------------------------------------------------------
static void closeSeg() {
  if (!s_segOpen) return;
  s_segOpen = false;
  if (s_segSec == 0) return;
  int i = ensureItem(s_segGrp, s_segName);
  s_it[i].sec += s_segSec;
  s_it[i].last = nowUnix() ? nowUnix() : s_it[i].last;
  s_segSec = 0;
  s_dirty = true;
  s_dirtyAt = millis();
}

static void openSeg(const char* grp, const char* name) {
  copyStr(s_segGrp,  sizeof(s_segGrp),  grp);
  copyStr(s_segName, sizeof(s_segName), name);
  s_segSec  = 0;
  s_segAt   = millis();
  s_segOpen = true;

  int i = ensureItem(s_segGrp, s_segName);
  s_it[i].plays++;
  s_it[i].last = nowUnix() ? nowUnix() : s_it[i].last;
  s_days[0].plays++;
  s_dirty = true;
  s_dirtyAt = millis();
}

// ------------------------------------------------------------
//  落盘
// ------------------------------------------------------------
static void saveNow() {
  s_dirty = false;
  File f = LittleFS.open(F_HIST, "w");
  if (!f) return;
  for (int d = 0; d < s_dayN; d++) {
    if (!s_days[d].key) continue;
    f.printf("#day\t%u\t%u\t%u\n", s_days[d].key, s_days[d].sec, s_days[d].plays);
  }
  for (int i = 0; i < s_n; i++) {
    if (!s_it[i].name[0]) continue;
    f.printf("%s\t%s\t%u\t%u\t%u\n",
             s_it[i].name, s_it[i].grp, s_it[i].sec, s_it[i].last, s_it[i].plays);
  }
  f.close();
}

void historySaveNow() {
  closeSeg();                 // 先把这段算进去，别丢
  saveNow();
}

static void load() {
  s_n = 0;
  s_dayN = 1;
  memset(s_days, 0, sizeof(s_days));
  if (!LittleFS.exists(F_HIST)) return;
  File f = LittleFS.open(F_HIST, "r");
  if (!f) return;

  static char line[128];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    int L = strlen(line);
    while (L && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
    if (!line[0]) continue;

    if (!strncmp(line, "#day\t", 5)) {
      unsigned k = 0, ts = 0, tp = 0;
      if (sscanf(line + 5, "%u\t%u\t%u", &k, &ts, &tp) == 3 && k && s_dayN < HIST_DAYS) {
        s_days[s_dayN].key   = k;
        s_days[s_dayN].sec   = ts;
        s_days[s_dayN].plays = (uint16_t)(tp > 65535 ? 65535 : tp);
        s_dayN++;
      }
      continue;
    }

    if (s_n >= HIST_MAX) continue;
    char* t1 = strchr(line, '\t');            if (!t1) continue;
    char* t2 = strchr(t1 + 1, '\t');          if (!t2) continue;
    char* t3 = strchr(t2 + 1, '\t');          if (!t3) continue;
    char* t4 = strchr(t3 + 1, '\t');          if (!t4) continue;
    *t1 = 0; *t2 = 0; *t3 = 0; *t4 = 0;

    HistItem& h = s_it[s_n];
    memset(&h, 0, sizeof(h));
    copyStr(h.name, sizeof(h.name), line);
    copyStr(h.grp,  sizeof(h.grp),  t1 + 1);
    h.sec   = (uint32_t)strtoul(t2 + 1, nullptr, 10);
    h.last  = (uint32_t)strtoul(t3 + 1, nullptr, 10);
    unsigned p = (unsigned)strtoul(t4 + 1, nullptr, 10);
    h.plays = (uint16_t)(p > 65535 ? 65535 : p);
    if (h.name[0]) s_n++;
  }
  f.close();
  sortRecent();
}

// ------------------------------------------------------------
//  对外
// ------------------------------------------------------------
void historyBegin() {
  load();
  rollDay();
}

void historyTick() {
  rollDay();

  const uint32_t now = millis();

  // 熄屏了还在播（老人听着睡着了也算在听），所以不看 A.screenOn
  if (A.ps == PS_PLAY && A.cur >= 0 && A.cur < g_stN) {
    // 1.15.0：原来记的是「分组名」，分组没了 —— 改记这台所属的**标签**
    const char* grp  = storeTag(A.cur);
    const char* name = g_st[A.cur].name;
    if (!s_segOpen || strcmp(name, s_segName) != 0 || strcmp(grp, s_segGrp) != 0) {
      closeSeg();
      openSeg(grp, name);
    }
    // 每满 1 秒 +1（用 millis 差值，别用「循环次数」）
    if (now - s_segAt >= 1000) {
      uint32_t add = (now - s_segAt) / 1000;
      if (add > 30) add = 30;       // 卡过 / 睡眠过，别一次记一大截
      s_segSec   += add;
      s_segAt    += add * 1000;
      s_days[0].sec += add;
      s_dirty = true;
    }
  } else if (s_segOpen) {
    closeSeg();
  }

  // 60 秒合并一次写盘；写盘本身只要几毫秒
  if (s_dirty && now - s_dirtyAt > 60000) saveNow();
}

int historyCount() { return s_n; }

const HistItem* historyAt(int i) {
  return (i >= 0 && i < s_n) ? &s_it[i] : nullptr;
}

uint32_t historyTotalSec() {
  uint32_t t = 0;
  for (int i = 0; i < s_n; i++) t += s_it[i].sec;
  return t + s_segSec;
}

uint32_t historyTodaySec()   { return s_days[0].sec; }
int      historyTodayPlays() { return (int)s_days[0].plays; }

int historyDayCount() {
  int n = 0;
  for (int i = 0; i < s_dayN; i++) if (s_days[i].key) n++;
  return n;
}

const DayStat* historyDayAt(int i) {
  if (i < 0 || i >= s_dayN) return nullptr;
  return s_days[i].key ? &s_days[i] : nullptr;
}

void historyClear() {
  s_n = 0;
  s_segSec = 0;
  s_segOpen = false;
  memset(s_days, 0, sizeof(s_days));
  s_dayN = 1;
  s_days[0].key = todayKey();
  s_dirty = false;
  LittleFS.remove(F_HIST);
}
