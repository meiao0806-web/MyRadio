// ============================================================
//  offair.cpp —— 「这台现在不播」台账（1.46.6）
//
//  线程模型和 health.cpp 一样：
//    · 写：audio.cpp 的音频任务（判定） + 主循环（到点/开关）
//    · 读：主循环（导航、画屏） + 网络任务（/api/offair）
//  只交换整数标志，不加锁 —— 读到半旧的值最多让屏上数字晚一帧，没有坏处。
//
//  为什么按「地址指纹」而不是台序号：和 health 一样，App 里拖排序 / 增删台
//  都不会把「这台停播」记到别人头上。
// ============================================================
#include "offair.h"
#include "config.h"
#include "state.h"
#include "store.h"
#include "album.h"
#include "epis.h"
#include "health.h"

#include <LittleFS.h>
#include <string.h>

#define OFF_FILE     "/offair.bin"
#define OFF_MAGIC    0x31465021u    // "!PF1"：结构一变就换这个数，旧文件自动作废

struct OffSlot {
  uint32_t key;      // 地址指纹（与 health 同一份）
  uint32_t endMs;    // 跳过到什么时候（millis 比较，用有符号差值判过期）
  uint8_t  miss;     // 连着判了几次（0 = 这个槽是空的/已恢复正常）
  uint8_t  pad;
};

static OffSlot  g_sl[OFFAIR_MAX_N];
static int      g_n = 0;
static bool     g_on = (OFFAIR_ON != 0);
static bool     g_dirty = false;
static uint32_t g_dirtyAt = 0;
static uint32_t g_saveAt = 0;

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------
static OffSlot* ofFind(uint32_t key) {
  for (int i = 0; i < g_n; i++) if (g_sl[i].key == key) return &g_sl[i];
  return nullptr;
}

// 台账写满了就顶掉「最早到期、而且已经连续判过多次」的那条 ——
// 那种通常是早就恢复正常、只是一直没被转到的台。
static OffSlot* ofEvict() {
  int best = 0;
  for (int i = 1; i < g_n; i++) {
    const int32_t a = (int32_t)(g_sl[i].endMs - millis());
    const int32_t b = (int32_t)(g_sl[best].endMs - millis());
    if (a < b) best = i;
  }
  return &g_sl[best];
}

// 退避：15 / 30 / 45 / 60 分钟封顶。
//   为什么是这几档：凌晨停播的台大多 5:00~6:00 恢复，一小时的回看间隔
//   最多让它晚一小时回到列表，老人听不出差别；而间隔再短会让"转到就跳"变频繁。
static uint32_t ofBackoffMin(uint8_t miss) {
  if (miss <= 1) return OFFAIR_BASE_MIN;
  if (miss == 2) return OFFAIR_BASE_MIN * 2;
  if (miss == 3) return OFFAIR_BASE_MIN * 3;
  return OFFAIR_MAX_MIN;
}

static inline bool ofAlive(const OffSlot& s) {
  return s.miss > 0 && (int32_t)(s.endMs - millis()) > 0;
}

// ------------------------------------------------------------
//  落盘 / 读盘（手写字节序，不依赖结构体对齐）
// ------------------------------------------------------------
static void ofSave() {
  g_dirty = false;
  g_dirtyAt = millis();
  g_saveAt  = millis();

  File f = LittleFS.open(OFF_FILE, "w");
  if (!f) return;

  uint16_t cnt = (uint16_t)g_n;
  const uint8_t hdr[12] = {
    (uint8_t)(OFF_MAGIC), (uint8_t)(OFF_MAGIC >> 8),
    (uint8_t)(OFF_MAGIC >> 16), (uint8_t)(OFF_MAGIC >> 24),
    (uint8_t)(cnt), (uint8_t)(cnt >> 8), 0, 0,
    (uint8_t)(g_on ? 1 : 0), 0, 0, 0
  };
  f.write(hdr, sizeof(hdr));

  const uint32_t now = millis();
  for (int i = 0; i < g_n; i++) {
    const int32_t left = (int32_t)(g_sl[i].endMs - now);
    uint32_t min = (left > 0) ? ((uint32_t)left + 59999u) / 60000u : 0u;
    if (min > 60000u) min = 60000u;
    const uint8_t b[8] = {
      (uint8_t)(g_sl[i].key), (uint8_t)(g_sl[i].key >> 8),
      (uint8_t)(g_sl[i].key >> 16), (uint8_t)(g_sl[i].key >> 24),
      (uint8_t)(min), (uint8_t)(min >> 8),
      g_sl[i].miss, 0
    };
    f.write(b, sizeof(b));
  }
  f.close();
}

void offairBegin() {
  g_n = 0;
  g_dirty = false;
  g_saveAt = millis();

  if (!LittleFS.exists(OFF_FILE)) {
    Serial.println("[offair] 没有台账，从零开始");
    return;
  }
  File f = LittleFS.open(OFF_FILE, "r");
  if (!f) return;

  uint8_t hdr[12];
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return; }
  const uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                         ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
  if (magic != OFF_MAGIC) { f.close(); Serial.println("[offair] 台账版本不符，忽略"); return; }

  const int n = (int)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8));
  g_on = (hdr[8] != 0);
  const uint32_t now = millis();
  for (int i = 0; i < n && i < OFFAIR_MAX_N; i++) {
    uint8_t b[8];
    if (f.read(b, sizeof(b)) != sizeof(b)) break;
    OffSlot s;
    s.key = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    const uint32_t min = (uint32_t)b[4] | ((uint32_t)b[5] << 8);
    s.miss = b[6];
    s.pad = 0;
    if (!s.key) continue;
    // 断电期间的时间不算 —— 开机按「还剩多少分钟」重新起算。
    //   为什么不用 unix 时间：没网（纯收音）时根本没有时钟，
    //   而且凌晨停播这件事用相对倒计时就够了。
    s.endMs = now + min * 60000u;
    g_sl[g_n++] = s;
  }
  f.close();

  int alive = 0;
  for (int i = 0; i < g_n; i++) if (ofAlive(g_sl[i])) alive++;
  Serial.printf("[offair] 读回 %d 条台账，其中 %d 台现在仍在跳过（总开关 %s）\n",
                g_n, alive, g_on ? "开" : "关");
}

void offairTick() {
  const uint32_t now = millis();
  // 每 5 分钟把倒计时刷一次盘 —— 否则断电重启后按「判停播那一刻」的
  // 剩余分钟重算，会把一个 60 分钟的窗口又续到 60 分钟，越用越长。
  if (g_n > 0 && now - g_saveAt > (uint32_t)OFFAIR_SAVE_MIN * 60000u) {
    g_dirty = true;
    ofSave();
    return;
  }
  if (!g_dirty) return;
  if (now - g_dirtyAt < 8000) return;     // 合并写，别一秒一次磨 flash
  ofSave();
}

// ------------------------------------------------------------
//  判定
// ------------------------------------------------------------
bool offairMark(int idx) {
  if (!g_on) return false;
  if (idx < 0 || idx >= g_stN) return false;
  // 有声书是点播文件，连不上是"这一集坏了"，处理方式是换下一集（audio.cpp 已经做了），
  // 不该按"这个台停播"藏起来 —— 否则整本书会连着几天不出现。
  if (episBookOfStation(idx) >= 0) return false;

  const uint32_t key = healthKeyOf(idx);
  if (!key) return false;

  OffSlot* s = ofFind(key);
  if (!s) {
    if (g_n < OFFAIR_MAX_N) s = &g_sl[g_n++];
    else {
      s = ofEvict();
      memset(s, 0, sizeof(*s));
      s->key = key;
    }
  }
  if (s->miss < 6) s->miss++;
  const uint32_t min = ofBackoffMin(s->miss);
  s->endMs = millis() + min * 60000u;
  g_dirty = true;
  g_dirtyAt = millis();
  Serial.printf("[offair] #%d %s 判为「现在不播」，跳过 %lu 分钟（第 %d 次）\n",
                idx, g_st[idx].name, (unsigned long)min, (int)s->miss);
  return true;
}

void offairOk(int idx) {
  if (idx < 0 || idx >= g_stN) return;
  OffSlot* s = ofFind(healthKeyOf(idx));
  if (!s || s->miss == 0) return;
  s->miss = 0;
  s->endMs = 0;
  g_dirty = true;
  g_dirtyAt = millis();
  Serial.printf("[offair] #%d %s 又出声了，从台账里放回\n", idx, g_st[idx].name);
}

// ------------------------------------------------------------
//  查询 / 导航
// ------------------------------------------------------------
bool offairSkip(int idx) {
  if (!g_on || idx < 0 || idx >= g_stN) return false;
  OffSlot* s = ofFind(healthKeyOf(idx));
  return s && ofAlive(*s);
}

bool offairProbe(int idx) {
  if (!g_on || idx < 0 || idx >= g_stN) return false;
  OffSlot* s = ofFind(healthKeyOf(idx));
  if (!s || s->miss == 0) return false;
  return (int32_t)(s->endMs - millis()) <= 0;
}

int offairHidden() {
  if (!g_on) return 0;
  int c = 0;
  for (int i = 0; i < g_n; i++) if (ofAlive(g_sl[i])) c++;
  return c;
}

bool offairEnabled() { return g_on; }

int offairStep(int idx, int dir) {
  if (g_stN <= 0) return idx;
  if (!g_on) return albumStep(idx, dir >= 0 ? 1 : -1);
  int i = idx;
  for (int guard = 0; guard < g_stN; guard++) {
    i = albumStep(i, dir >= 0 ? 1 : -1);
    if (!offairSkip(i)) return i;
  }
  // 整张表都被跳过（比如整网断了，把所有台都判了一遍）：
  //   退回普通一格。宁可走回一个停播台，也不能让旋钮"转不动"。
  Serial.println("[offair] 整张表都在跳过 —— 退回普通换台（网络可能整个断了）");
  return albumStep(idx, dir >= 0 ? 1 : -1);
}

int offairFirstAudible(int idx) {
  if (g_stN <= 0) return idx;
  if (idx < 0 || idx >= g_stN) return 0;
  if (!g_on || !offairSkip(idx)) return idx;
  int i = idx;
  for (int guard = 0; guard < g_stN; guard++) {
    i = albumStep(i, +1);
    if (!offairSkip(i)) return i;
  }
  return idx;
}

// ------------------------------------------------------------
//  管理
// ------------------------------------------------------------
void offairClearAll() {
  for (int i = 0; i < g_n; i++) { g_sl[i].miss = 0; g_sl[i].endMs = 0; }
  g_dirty = true;
  g_dirtyAt = millis();
  Serial.println("[offair] 台账清空（所有台立刻回到列表）");
}

void offairSetEnabled(bool on) {
  if (g_on == on) return;
  g_on = on;
  g_dirty = true;
  g_dirtyAt = millis();
  Serial.printf("[offair] 总开关 → %s\n", on ? "开" : "关");
}

void offairJson(String& o) {
  o = "{\"on\":";
  o += g_on ? 1 : 0;
  o += ",\"n\":";
  o += offairHidden();
  o += ",\"max\":";
  o += OFFAIR_MAX_N;
  o += ",\"list\":[";
  bool first = true;
  const int n = g_n;
  for (int i = 0; i < n && i < g_n; i++) {
    if (!ofAlive(g_sl[i])) continue;
    // 反查台号：指纹是按地址算的，删台/改地址后台号会变，所以每次现算
    int idx = -1;
    for (int k = 0; k < g_stN; k++) {
      if (healthKeyOf(k) == g_sl[i].key) { idx = k; break; }
    }
    if (idx < 0) continue;               // 这台的地址已经不在表里了
    if (!first) o += ',';
    first = false;
    o += "{\"i\":";
    o += idx;
    o += ",\"n\":\"";
    for (const char* p = g_st[idx].name; *p; p++) {
      const char c = *p;
      if (c == '"' || c == '\\') o += '\\';
      o += c;
    }
    o += "\",\"min\":";
    o += (long)(((int32_t)(g_sl[i].endMs - millis()) + 59999) / 60000);
    o += ",\"miss\":";
    o += (int)g_sl[i].miss;
    o += "}";
  }
  o += "]}";
}
