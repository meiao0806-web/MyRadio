// ============================================================
//  epis.cpp —— 有声书集表 / 循环播放 / 进度记忆
//  见 epis.h。集表本身是编译进固件的常量（preset_episodes.h），
//  这里只负责「每部书听到第几集」这一件可变状态。
// ============================================================
#include "epis.h"
#include "preset_episodes.h"
#include "state.h"

#include <LittleFS.h>
#include <string.h>

#define EPIS_FILE    "/epis.txt"
#define EPIS_SAVE_MS 2000

static uint16_t s_cur[EPIS_BOOK_N];      // 每部书听到第几集（0 基）
// 1.46.33：App 里设的「下次调到这个台时从第 X 集起」。-1 = 没设。
//   跟 s_cur 分开存：s_cur 是「听到哪儿了」，这个是「下次从哪儿起」，
//   混用一个变量的话，用户在 App 里一设就把他听到的进度改掉了。
static int16_t  s_next[EPIS_BOOK_N];
// 1.10.0 起集表不再只有 15 集：听书网的书动辄 300+ 集，原来用 int8_t 存会溢出到负数，
// 现在用 uint16_t（够到 65535 集）。
// 1.46.56 起落盘格式是「书名\t集号\t下次集号」—— 以前是「书号\t集号」，
//   中间插一本书就会让后面所有书的号后移，进度会串到别的书上（详见 episFlush）。
static bool     s_dirty = false;
static uint32_t s_dirtyAt = 0;

// ---------------- 名字 → 书号 ----------------
// 台表里的台名与书名完全相同（gen_preset_curated.py 保证），
// 但也允许台名后面跟一个空格再接说明，免得手工改过名字就认不出。
int episBookOfName(const char* name) {
  if (!name || !*name) return -1;
  for (int b = 0; b < EPIS_BOOK_N; b++) {
    const char* n = kEpisBook[b].name;
    if (!n || !*n) continue;
    const size_t L = strlen(n);
    if (strncmp(name, n, L) != 0) continue;
    if (name[L] == 0 || name[L] == ' ') return b;
  }
  return -1;
}

int episBookOfStation(int stIdx) {
  if (stIdx < 0 || stIdx >= g_stN) return -1;
  return episBookOfName(g_st[stIdx].name);
}

int episBookCount() { return EPIS_BOOK_N; }

const char* episBookName(int b) {
  if (b < 0 || b >= EPIS_BOOK_N) return "";
  return kEpisBook[b].name;
}

int episCount(int b) {
  if (b < 0 || b >= EPIS_BOOK_N) return 0;
  return (int)kEpisBook[b].count;
}

int episCur(int b) {
  if (b < 0 || b >= EPIS_BOOK_N) return -1;
  const int n = episCount(b);
  if (n <= 0) return 0;
  int e = s_cur[b];
  if (e < 0 || e >= n) e = 0;
  return e;
}

void episSetCur(int b, int e) {
  if (b < 0 || b >= EPIS_BOOK_N) return;
  const int n = episCount(b);
  if (n <= 0) return;
  if (e < 0) e = 0;
  if (e >= n) e = n - 1;
  if (s_cur[b] != (uint16_t)e) {
    s_cur[b] = (uint16_t)e;
    s_dirty = true;
    s_dirtyAt = millis();
  }
}

// 循环：最后一集之后回第一集，第一集之前回最后一集
int episStep(int b, int dir) {
  const int n = episCount(b);
  if (n <= 1) return 0;
  int e = episCur(b) + (dir >= 0 ? 1 : -1);
  e = ((e % n) + n) % n;
  return e;
}

// ---------------- 1.46.33：下次从第 X 集 ----------------
int episNext(int b) {
  if (b < 0 || b >= EPIS_BOOK_N) return -1;
  const int n = episCount(b);
  if (n <= 0) return -1;
  const int e = s_next[b];
  if (e < 0 || e >= n) return -1;
  return e;
}

void episSetNext(int b, int e) {
  if (b < 0 || b >= EPIS_BOOK_N) return;
  const int n = episCount(b);
  if (n <= 0) return;
  if (e < 0) e = -1;
  else {
    if (e >= n) e = n - 1;
  }
  if (s_next[b] != (int16_t)e) {
    s_next[b] = (int16_t)e;
    s_dirty = true;
    s_dirtyAt = millis();
  }
}

void episClearNext(int b) {
  if (b < 0 || b >= EPIS_BOOK_N) return;
  if (s_next[b] == -1) return;
  s_next[b] = -1;
  s_dirty = true;
  s_dirtyAt = millis();
}

// 起播时调用：取一次就作废（「下次」只生效一次）
int episTakeNext(int b) {
  const int e = episNext(b);
  if (e < 0) return -1;
  s_next[b] = -1;
  s_dirty = true;
  s_dirtyAt = millis();
  return e;
}

bool episUrl(int b, int e, char* out, size_t n) {
  if (!out || n == 0) return false;
  out[0] = 0;
  if (b < 0 || b >= EPIS_BOOK_N) return false;
  const EpisBook& B = kEpisBook[b];
  if (e < 0 || e >= (int)B.count) return false;
  const EpisItem& it = kEpis[B.first + e];
  if (!it.u || !*it.u) return false;
  snprintf(out, n, "%s", it.u);
  return true;
}

const char* episTitle(int b, int e) {
  if (b < 0 || b >= EPIS_BOOK_N) return "";
  const EpisBook& B = kEpisBook[b];
  if (e < 0 || e >= (int)B.count) return "";
  const char* t = kEpis[B.first + e].t;
  return t ? t : "";
}

// ---------------- 进度落盘 ----------------
// 1.46.56：进度改成**按书名**存（原来是按「书号」）。
//   书号是编译期顺序，中间插一本新书，后面每部书的号都会后移 ——
//   老文件里的「书号」会直接对到别的书上（听到第 50 集的《鬼吹灯》，
//   开机变成另一本书的第 50 集）。2026-11-04 加 8 部节目时正好插在中间，必踩。
//   生成器保证书名不重复（同名去重），所以书名可以当键。
//   老格式（首列是数字）认不出是哪本书，只能丢弃 —— 宁可从头开始，也别串台。
static void episFlush() {
  File f = LittleFS.open(EPIS_FILE, "w");
  if (!f) return;
  // 第三列是「下次从第 X 集」（1.46.33）。
  for (int b = 0; b < EPIS_BOOK_N; b++)
    f.printf("%s\t%d\t%d\n", kEpisBook[b].name, (int)s_cur[b], (int)s_next[b]);
  f.close();
}

void episBegin() {
  for (int b = 0; b < EPIS_BOOK_N; b++) { s_cur[b] = 0; s_next[b] = -1; }
  s_dirty = false;
  if (!LittleFS.exists(EPIS_FILE)) return;

  File f = LittleFS.open(EPIS_FILE, "r");
  if (!f) return;
  int got = 0, stale = 0;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (!ln.length()) continue;
    const int tab = ln.indexOf('\t');
    if (tab <= 0) continue;
    String key = ln.substring(0, tab);
    key.trim();
    if (!key.length()) continue;
    // 老格式（1.46.55 及以前）首列是书号，不是书名：整行作废
    if (key[0] >= '0' && key[0] <= '9') { stale++; continue; }
    const int b = episBookOfName(key.c_str());
    if (b < 0) continue;                             // 这本书已经不在固件里
    const int tab2 = ln.indexOf('\t', tab + 1);
    const int e = (tab2 > 0 ? ln.substring(tab + 1, tab2) : ln.substring(tab + 1)).toInt();
    const int nx = (tab2 > 0) ? ln.substring(tab2 + 1).toInt() : -1;
    if (e < 0 || e >= episCount(b)) continue;        // 这本书改过集数：回到第 1 集
    s_cur[b] = (uint16_t)e;
    // 第三列坏了/缺了都当「没设」，不要因为这个把整行丢了
    s_next[b] = (nx >= 0 && nx < episCount(b)) ? (int16_t)nx : -1;
    got++;
  }
  f.close();
  if (stale) Serial.printf("[epis] 丢弃 %d 行老格式（按书号存的）进度\n", stale);
  Serial.printf("[epis] 载入 %d 部书的进度（共 %d 集）\n", got, EPIS_ITEM_N);
}

void episTick() {
  if (s_dirty && millis() - s_dirtyAt > EPIS_SAVE_MS) {
    s_dirty = false;
    episFlush();
  }
}
