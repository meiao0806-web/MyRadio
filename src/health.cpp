// ============================================================
//  health.cpp —— 台的健康档案与实时出声判断（1.17.0）
//
//  线程模型（重要）：
//    · 所有「写」都发生在音频任务里（audioTask → startIndex / loop 回调）
//    · 「读」发生在网络任务里（/api/health 拼 JSON）
//    两边只交换 1 字节量级的标志，不加锁：读到半旧的值只会让 App 晚 1.5 秒
//    看到结果，不会有坏处。
// ============================================================
#include "health.h"
#include "state.h"
#include "store.h"

#include <LittleFS.h>
#include <string.h>
#include <time.h>

#define HL_FILE     "/health.bin"
#define HL_MAGIC    0x314C4821u    // "!HL1"：结构一变就换这个数，旧文件自动作废
#define HL_MAX      512            // 最多记多少台（台表上限 MAX_STATIONS=500）

// 1.46.1：台表上限一旦超过健康档案的容量，超出的台会**静默地没有档案**
//   （glFind 到 HL_MAX 就返回 nullptr），App 上只显示「未测过」——
//   这种降级最难查，所以干脆编译期拦住。
static_assert(MAX_STATIONS <= HL_MAX, "台表上限超过了健康档案 HL_MAX，超出的台会没有档案");

#define HL_PK_TH    900            // 16 位 PCM 峰值超过这个算「这一块有声」（≈ -31dBFS）
#define HL_LOUD_N   10             // 累计 10 个有声块（≈250ms 音频）就判「真的出声了」
#define HL_SILENT   12000UL        // 有数据流但 12 秒一声不出 → 可疑
#define HL_CONN     25000UL        // 25 秒还没解码出任何东西 → 连不上

struct HlRec {
  uint32_t key;      // FNV-1a(地址)
  uint32_t lastOk;   // 上次正常出声（unix 秒，0 = 没同步过时间）
  uint32_t lastChk;  // 上次判定时间
  uint16_t fail;     // 累计失败次数
  uint8_t  st;       // HlState
  uint8_t  lv;       // 上次听到的电平峰值
};

static HlRec    g_hl[HL_MAX];
static int      g_hlN = 0;
static bool     g_dirty = false;
static uint32_t g_dirtyAt = 0;

// 当前这一次试台（一次 startIndex 算一次）
static bool     s_on = false;
static int      s_idx = -1;
static uint32_t s_t0 = 0;
static bool     s_meta = false;
static int      s_loud = 0;        // 有声块的累计数
static uint8_t  s_st = 0;          // 0 测试中 / 1 有声 / 2 连不上 / 3 没声音
static bool     s_hold = false;    // 暂停中：电平照画，但不做「没声音」的判定
static uint32_t s_pk = 0;          // 本毫秒内见过的最大 PCM 峰值
static uint8_t  s_lv = 0;          // 平滑后的电平 0..100（给 App 画实时条）

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------
static uint32_t hlHash(const char* s) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
    h ^= *p;
    h *= 16777619u;
  }
  return h ? h : 1u;
}

// 有 NTP 就用真实时间（跨重启也能显示「3 天前」），没有就记 0
static uint32_t hlNow() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (uint32_t)t : 0;
}

static HlRec* hlFind(uint32_t key, bool create) {
  for (int i = 0; i < g_hlN; i++) if (g_hl[i].key == key) return &g_hl[i];
  if (!create || g_hlN >= HL_MAX) return nullptr;
  HlRec* r = &g_hl[g_hlN++];
  memset(r, 0, sizeof(*r));
  r->key = key;
  r->st = HL_UNK;
  return r;
}

static HlRec* hlOf(int idx) {
  if (idx < 0 || idx >= g_stN) return nullptr;
  return hlFind(hlHash(g_st[idx].url), false);
}

static void hlMark() {
  if (!g_dirty) g_dirtyAt = millis();
  g_dirty = true;
}

static void hlSet(int idx, uint8_t st, bool bumpFail) {
  HlRec* r = hlFind(hlHash((idx >= 0 && idx < g_stN) ? g_st[idx].url : ""), true);
  if (!r) return;
  r->st = st;
  r->lastChk = hlNow();
  if (st == HL_OK) {
    r->fail = 0;
    r->lastOk = r->lastChk;
    r->lv = s_lv;
  } else if (bumpFail && r->fail < 999) {
    r->fail++;
  }
  hlMark();
}

// ------------------------------------------------------------
//  落盘 / 读盘
// ------------------------------------------------------------
void healthBegin() {
  g_hlN = 0;
  g_dirty = false;
  s_on = false;
  s_idx = -1;

  if (!LittleFS.exists(HL_FILE)) return;
  File f = LittleFS.open(HL_FILE, "r");
  if (!f) return;

  uint32_t hdr[2] = {0, 0};
  if (f.read((uint8_t*)hdr, sizeof(hdr)) != sizeof(hdr) || hdr[0] != HL_MAGIC) {
    f.close();
    return;
  }
  int n = (int)hdr[1];
  if (n <= 0 || n > HL_MAX) { f.close(); return; }

  for (int i = 0; i < n; i++) {
    HlRec r;
    if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
    if (r.st != HL_OK && r.st != HL_BAD && r.st != HL_DIM) continue;   // 坏数据丢掉
    g_hl[g_hlN++] = r;
  }
  f.close();
  Serial.printf("[health] 读回 %d 条体检记录\n", g_hlN);
}

void healthTick() {
  if (!g_dirty) return;
  if (millis() - g_dirtyAt < 8000) return;    // 合并写，别一秒一次磨 flash
  g_dirty = false;
  g_dirtyAt = millis();

  File f = LittleFS.open(HL_FILE, "w");
  if (!f) return;
  uint32_t hdr[2] = {HL_MAGIC, (uint32_t)g_hlN};
  f.write((const uint8_t*)hdr, sizeof(hdr));
  if (g_hlN) f.write((const uint8_t*)g_hl, sizeof(HlRec) * (size_t)g_hlN);
  f.close();
}

// ------------------------------------------------------------
//  播放任务里调
// ------------------------------------------------------------
void healthStart(int idx) {
  s_on = true;
  s_idx = idx;
  s_t0 = millis();
  s_meta = false;
  s_loud = 0;
  s_st = 0;
  s_hold = false;
  s_pk = 0;
  s_lv = 0;
}

// 暂停：人按了暂停，不是台坏了。恢复时把计时清零，重新给 12 秒
void healthHold(bool on) {
  s_hold = on;
  if (!on) { s_t0 = millis(); s_loud = 0; s_st = 0; }
}

void healthStop() {
  s_on = false;
  s_idx = -1;
  s_hold = false;
  s_lv = 0;
}

void healthMeta() { s_meta = true; }

// PCM 回调：算这一块的峰值。buf 是交错立体声，len 是「每声道样本数」，
// 只扫前 len 个 int16（够判断有没有声音，也不越界）。
void healthSamples(const int16_t* buf, uint16_t n) {
  if (!buf || !n) return;
  s_meta = true;                       // 能走到这儿就说明真的在解码
  uint16_t lim = (n > 1024) ? 1024 : n;
  int32_t pk = 0;
  for (uint16_t i = 0; i < lim; i++) {
    int32_t v = buf[i];
    if (v < 0) v = -v;
    if (v > pk) pk = v;
  }
  if ((uint32_t)pk > s_pk) s_pk = (uint32_t)pk;
  if (s_on && pk >= HL_PK_TH && s_loud < 32000) s_loud++;
}

uint8_t healthTakePeak() {
  uint32_t raw = s_pk;
  s_pk = 0;
  uint32_t lv = raw * 100u / 32768u;
  return (uint8_t)(lv > 100 ? 100 : lv);
}

void healthOk() {
  if (!s_on || s_st == 1) return;
  s_st = 1;
  hlSet(s_idx, HL_OK, false);
}

void healthFail(const char* why) {
  (void)why;                            // 屏上不显示原因，App 只看状态码
  if (!s_on || s_st == 2) return;
  s_st = 2;
  hlSet(s_idx, HL_BAD, true);
}

void healthSilent() {
  if (!s_on || s_st == 3) return;
  s_st = 3;
  hlSet(s_idx, HL_DIM, false);
}

// 每毫秒一次：把峰值抹成「升快降慢」的电平，并做三件判定
void healthLevel(uint8_t pk) {
  if (pk >= s_lv) s_lv = pk;
  else if (s_lv) s_lv--;

  if (!s_on || s_st != 0 || s_hold) return;

  const uint32_t now = millis();
  const uint32_t dur = now - s_t0;

  if (s_loud >= HL_LOUD_N) {                 // 真的听见了
    healthOk();
  } else if (!s_meta && dur > HL_CONN) {     // 这么久还没任何数据 → 地址死了
    healthFail("连不上");
  } else if (s_meta && dur > HL_SILENT) {    // 数据在流，就是没声音
    healthSilent();
  }
}

// ------------------------------------------------------------
//  查询
// ------------------------------------------------------------
uint8_t healthState(int idx) {
  HlRec* r = hlOf(idx);
  return r ? r->st : HL_UNK;
}

// 1.46.6：把「地址 → 指纹」开放出去给停播台账用（同一个 hlHash，别另写一份）
uint32_t healthKeyOf(int idx) {
  return hlHash((idx >= 0 && idx < g_stN) ? g_st[idx].url : "");
}

uint8_t healthPeak() { return s_lv; }
int     healthSession() { return s_on ? s_idx : -1; }

static void hlEsc(String& o, const char* s) {
  for (const char* p = s ? s : ""; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { o += ' '; }
    else o += c;
  }
}

static const char* hlVerdict(uint8_t st) {
  switch (st) {
    case HL_OK:  return "ok";
    case HL_BAD: return "bad";
    case HL_DIM: return "dim";
    default:     return "unk";
  }
}

void healthJson(String& o, int one) {
  // ---- 单台详情 ----
  if (one >= 0 && one < g_stN) {
    HlRec* r = hlOf(one);
    o = "{\"i\":";
    o += one;
    o += ",\"n\":\"";
    hlEsc(o, g_st[one].name);
    o += "\",\"st\":";
    o += (int)(r ? r->st : HL_UNK);
    o += ",\"v\":\"";
    o += hlVerdict(r ? r->st : HL_UNK);
    o += "\",\"fail\":";
    o += (int)(r ? r->fail : 0);
    o += ",\"lastOk\":";
    o += (long)(r ? r->lastOk : 0);
    o += ",\"lastChk\":";
    o += (long)(r ? r->lastChk : 0);
    o += ",\"lv\":";
    o += (int)(r ? r->lv : 0);
    o += ",\"ep\":";
    o += storeIsQtfm(one) ? 1 : 0;
    o += ",\"live\":";
    o += (s_on && s_idx == one) ? 1 : 0;
    o += "}";
    return;
  }

  // ---- 整表摘要 ----
  int ok = 0, bad = 0, dim = 0, unk = 0;
  String sS, sF;
  sS.reserve(g_stN + 8);
  sF.reserve(g_stN + 8);
  for (int i = 0; i < g_stN; i++) {
    HlRec* r = hlOf(i);
    uint8_t st = r ? r->st : HL_UNK;
    if (st > HL_DIM) st = HL_UNK;
    if (st == HL_OK) ok++;
    else if (st == HL_BAD) bad++;
    else if (st == HL_DIM) dim++;
    else unk++;
    sS += (char)('0' + st);
    uint16_t f = r ? r->fail : 0;
    sF += (char)('0' + (f > 9 ? 9 : f));
  }

  o = "{\"n\":";
  o += g_stN;
  o += ",\"s\":\"";
  o += sS;
  o += "\",\"f\":\"";
  o += sF;
  o += "\",\"c\":{\"ok\":";
  o += ok;
  o += ",\"bad\":";
  o += bad;
  o += ",\"dim\":";
  o += dim;
  o += ",\"unk\":";
  o += unk;
  o += "},\"cur\":{\"i\":";
  o += s_on ? s_idx : -1;
  o += ",\"st\":";
  o += (int)s_st;
  o += ",\"lv\":";
  o += (int)s_lv;
  o += ",\"sec\":";
  o += s_on ? (long)((millis() - s_t0) / 1000) : 0;
  o += ",\"why\":\"";
  if (s_st == 2) hlEsc(o, "连不上");
  else if (s_st == 3) hlEsc(o, "没声音");
  o += "\"}}";
}
