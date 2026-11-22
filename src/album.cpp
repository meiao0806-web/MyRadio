// ============================================================
//  album.cpp —— 专辑识别 / 单元跳转 / 进度记忆
//  见 album.h 的说明。这里不碰台表格式，全靠台名后缀。
// ============================================================
#include "album.h"
#include "state.h"
#include <LittleFS.h>

#define ALB_MAX       24                        // 最多记 24 部书，够用且只占 ~1KB
#define ALB_NAME_LEN  40                        // 与 Station.name 同宽
#define ALB_FILE      "/album.txt"
#define ALB_SAVE_MS   2000                      // 合并写盘的间隔

struct AlbProg {
  char    name[ALB_NAME_LEN];
  int16_t idx;                                  // 组内台索引（播到第几集）
};

static AlbProg  s_p[ALB_MAX];
static int      s_pN = 0;
static bool     s_dirty = false;
static uint32_t s_dirtyAt = 0;

// ---------------- 名字 → 专辑名 ----------------
// 尾部「空格 + 2~4 位数字」才算集号。要求前面至少 2 字节，
// 这样 "FM998 舟山新闻综合广播"、"中江综合广播994" 都不会被误认。
bool albumSplit(const char* stName, char* out, size_t n) {
  if (!stName || !out || n < 3) return false;

  const int len = (int)strlen(stName);
  int d = 0;
  while (d < 4 && (len - 1 - d) >= 0 &&
         stName[len - 1 - d] >= '0' && stName[len - 1 - d] <= '9') d++;
  if (d < 2) return false;                       // 至少要两位数字

  const int sp = len - d - 1;                    // 空格的位置
  if (sp < 2 || stName[sp] != ' ') return false;

  // 数字后面不能还有别的东西（上面 while 已在尾部，这里只兜底）
  size_t cp = (size_t)sp;
  if (cp > n - 1) cp = n - 1;
  memcpy(out, stName, cp);
  out[cp] = 0;
  return cp >= 2;
}

// ---------------- 单元（一部书 = 一段连续的台）----------------
static bool sameAlbum(int a, int b) {
  if (a < 0 || b < 0 || a >= g_stN || b >= g_stN) return false;
  char x[ALB_NAME_LEN], y[ALB_NAME_LEN];
  if (!albumSplit(g_st[a].name, x, sizeof(x))) return false;
  if (!albumSplit(g_st[b].name, y, sizeof(y))) return false;
  return strcmp(x, y) == 0;
}

int albumUnitStart(int i) {
  if (i < 0 || i >= g_stN) return -1;
  char nm[ALB_NAME_LEN];
  if (!albumSplit(g_st[i].name, nm, sizeof(nm))) return i;   // 普通台自成单元
  int s = i;
  while (s > 0 && sameAlbum(s - 1, i)) s--;
  return s;
}

static int albumUnitEnd(int i) {
  if (i < 0 || i >= g_stN) return -1;
  char nm[ALB_NAME_LEN];
  if (!albumSplit(g_st[i].name, nm, sizeof(nm))) return i;
  int e = i;
  while (e + 1 < g_stN && sameAlbum(e + 1, i)) e++;
  return e;
}

int albumStep(int i, int dir) {
  const int n = g_stN;
  if (n <= 0) return -1;
  if (i < 0 || i >= n) i = 0;

  const int s = albumUnitStart(i);
  const int e = albumUnitEnd(i);
  int j = (dir >= 0) ? (e + 1) : (s - 1);
  j = ((j % n) + n) % n;                          // 绕回，不会越界
  return albumUnitStart(j);
}

// ---------------- 进度记忆 ----------------
static int findProg(const char* nm) {
  for (int k = 0; k < s_pN; k++)
    if (strcmp(s_p[k].name, nm) == 0) return k;
  return -1;
}

static int getProg(const char* nm) {
  int k = findProg(nm);
  return (k >= 0) ? s_p[k].idx : -1;
}

int albumEntry(int i) {
  const int s = albumUnitStart(i);
  if (s < 0) return -1;
  char nm[ALB_NAME_LEN];
  if (!albumSplit(g_st[s].name, nm, sizeof(nm))) return s;

  const int rec = getProg(nm);
  // 记录必须在同一部书的连续段里，否则（台表被改过）回到首集
  if (rec >= s && rec < g_stN && sameAlbum(rec, s)) return rec;
  return s;
}

void albumRemember(int i) {
  if (i < 0 || i >= g_stN) return;
  char nm[ALB_NAME_LEN];
  if (!albumSplit(g_st[i].name, nm, sizeof(nm))) return;

  int k = findProg(nm);
  if (k < 0) {
    if (s_pN < ALB_MAX) {
      k = s_pN++;
    } else {
      k = 0;                                    // 满了就顶掉第一条，不报错
    }
    strncpy(s_p[k].name, nm, sizeof(s_p[k].name) - 1);
    s_p[k].name[sizeof(s_p[k].name) - 1] = 0;
    s_p[k].idx = -1;
  }
  if (s_p[k].idx != i) {
    s_p[k].idx = (int16_t)i;
    s_dirty = true;
    s_dirtyAt = millis();
  }
}

static void albumFlush() {
  File f = LittleFS.open(ALB_FILE, "w");
  if (!f) return;
  for (int k = 0; k < s_pN; k++) {
    if (s_p[k].idx < 0) continue;
    f.printf("%s\t%d\n", s_p[k].name, s_p[k].idx);
  }
  f.close();
}

void albumBegin() {
  s_pN = 0;
  s_dirty = false;
  if (!LittleFS.exists(ALB_FILE)) return;
  File f = LittleFS.open(ALB_FILE, "r");
  if (!f) return;
  while (f.available() && s_pN < ALB_MAX) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int tab = ln.lastIndexOf('\t');
    if (tab <= 0) continue;
    String nm = ln.substring(0, tab);
    int idx = ln.substring(tab + 1).toInt();
    if (nm.length() == 0) continue;
    AlbProg& p = s_p[s_pN++];
    strncpy(p.name, nm.c_str(), sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = 0;
    p.idx = (int16_t)idx;
  }
  f.close();
  Serial.printf("[album] 载入 %d 部书的进度\n", s_pN);
}

void albumTick() {
  if (s_dirty && millis() - s_dirtyAt > ALB_SAVE_MS) {
    s_dirty = false;
    albumFlush();
  }
}

int albumCount() { return s_pN; }
