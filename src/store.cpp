// ============================================================
//  store.cpp —— 台表 / 标签 / 设置
//
//  /stations.tsv 每行：标签 <TAB> 台号或地址 <TAB> 台名
//
//  1.15.0 起**不再分组**：
//    文件的先后就是唯一的播放顺序 —— 旋钮从第 0 台一路转到最后一台，
//    没有「组」这一维，也就没有「点一下换组」这回事。
//    第一栏降级成**标签**：只给手机 App 做筛选 / 管理用，
//    不参与播放顺序，也不再要求「同标签的行必须连续」。
//
//  为什么现在敢把整表读进内存：
//    以前是 1038 台 / 31 个地区，全塞 RAM 要 250KB（S3 只有 512KB），
//    所以按组切片、只读当前组。现在出厂表 307 台（1.46.1：上限 500），
//    g_st 走 PSRAM，索性整表读进来 ——
//    换来的是「换台 / 排序 / 改标签」全都只动内存，不用再流式重写文件。
//
//  标签表 /tags.txt：一行一个标签名，顺序就是 App 里 chips 的顺序。
//    单独存一份的原因：① 标签可以暂时没有台（新建的空标签也得显示）
//    ② 顺序不该被「哪台先出现」决定。
// ============================================================
#include "store.h"
#include <mbedtls/md.h>      // 1.46.33：蜻蜓节目地址要算 HMAC-MD5 签名
#include <LittleFS.h>
#include <Preferences.h>          // 1.46.29：音量单独走 NVS
#include "audio.h"                // 1.46.29：audioWriteSafe()
#include "preset_stations.h"

static const char* F_ST  = "/stations.tsv";
static const char* F_TMP = "/stations.tmp";
static const char* F_TAGS= "/tags.txt";
static const char* F_CFG = "/cfg.txt";
static const char* F_TOK = "/token.txt";
static const char* F_STAMP     = "/stamp.txt";       // 台表版本号（1.8.0）
static const char* F_STAMP_BAK = "/stations.bak";    // 台表升级前的备份

// 标签表（内存里就这一份；s_tagOf 是每台的下标，改名时所有台自动跟着走）
static char    s_tag[MAX_GROUPS][GROUP_NAME_LEN];
static int     s_tagN = 0;
static uint8_t s_tagOf[MAX_STATIONS];

static char     s_token[17] = {0};
static bool     s_dirty = false;
static uint32_t s_dirtyAt = 0;

// ---------------- 工具 ----------------
static void trimCr(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
}

// .tmp 换名成正式表
static void commitTable() {
  LittleFS.remove(F_ST);
  LittleFS.rename(F_TMP, F_ST);
}

bool storeBegin() {
  // 1.27.0 修正 —— 这是一个一直都在的 P0，只是之前屏不亮、没声音，谁也看不见它：
  //   Arduino 的 LittleFS.begin() 默认去挂 **标签叫 "spiffs"** 的分区
  //   （LittleFS.h: partitionLabel="spiffs"），而我们的 partitions.csv 里
  //   这个分区标签写的是 **"littlefs"** —— 名字对不上，于是：
  //       E esp_littlefs: partition "spiffs" could not be found
  //       [main] 单列表 0 台        ← 台表读不到，整机一台台都没有
  //   表现就是「能开机、能连 WiFi、但没台可放」。
  //   两个修法里选**显式传标签**：改分区表标签会让已经刷过机器的 flash 布局对不上，
  //   而传标签只改代码、对所有版本都安全。true = 挂不上就格式化（首次开机会灌出厂台表）。
  return LittleFS.begin(true, "/littlefs", 10, "littlefs");
}

// ---------------- 标签 ----------------
static int findTag(const char* n) {
  for (int i = 0; i < s_tagN; i++)
    if (strcmp(s_tag[i], n) == 0) return i;
  return -1;
}

// 有就返回下标，没有就追加到末尾。返回 -1 = 标签太多 / 名字非法
static int ensureTag(const char* n) {
  if (!n || !*n) return -1;
  int i = findTag(n);
  if (i >= 0) return i;
  if (s_tagN >= MAX_GROUPS) return -1;
  strncpy(s_tag[s_tagN], n, GROUP_NAME_LEN - 1);
  s_tag[s_tagN][GROUP_NAME_LEN - 1] = 0;
  return s_tagN++;
}

static void saveTags() {
  File f = LittleFS.open(F_TAGS, "w");
  if (!f) return;
  for (int i = 0; i < s_tagN; i++) { f.print(s_tag[i]); f.print('\n'); }
  f.close();
}

static void loadTags() {
  s_tagN = 0;
  if (!LittleFS.exists(F_TAGS)) return;
  File f = LittleFS.open(F_TAGS, "r");
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (ln.length() && ln.length() < GROUP_NAME_LEN) ensureTag(ln.c_str());
  }
  f.close();
}

int         storeTagCount()          { return s_tagN; }
const char* storeTagName(int t)      { return (t >= 0 && t < s_tagN) ? s_tag[t] : ""; }
int         storeTagOf(int i)        { return (i >= 0 && i < g_stN) ? (int)s_tagOf[i] : -1; }

const char* storeTag(int i) {
  int t = storeTagOf(i);
  return (t < 0) ? "" : s_tag[t];
}

int storeTagStations(int t) {
  if (t < 0 || t >= s_tagN) return 0;
  int n = 0;
  for (int i = 0; i < g_stN; i++) if (s_tagOf[i] == t) n++;
  return n;
}

// ---------------- 载入整表 ----------------
// 1.15.0：不再按组切片，整份读进 g_st。文件顺序 = 播放顺序。
static bool loadAll() {
  g_stN = 0;
  A.group = 0;              // 「组」这一维已经没了，恒为 0（接口还留着，见下面 storeGroup*）
  A.count = 0;

  loadTags();

  File f = LittleFS.open(F_ST, "r");
  if (!f) return false;

  static char line[512];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    trimCr(line);
    if (line[0] == 0 || line[0] == '#') continue;

    char* t1 = strchr(line, '\t');       if (!t1) continue;
    *t1 = 0;                                   // line = 标签
    char* t2 = strchr(t1 + 1, '\t');     if (!t2) continue;
    *t2 = 0;                                   // t1+1 = 地址，t2+1 = 台名
    if (t1[1] == 0) continue;                  // 空地址 = 空标签的占位行，跳过
    if (g_stN >= MAX_STATIONS) {
      // 1.21.2：原来这里静默 break —— 表比 MAX_STATIONS 长时，后面的台**直接消失**，
      // 用户只会觉得"我的台不见了"，查不到原因。至少要在串口喊一声。
      Serial.printf("[store] !! 台表超过上限 %d，后面的全部忽略（原文件比这个长）\n",
                    MAX_STATIONS);
      break;
    }

    Station& s = g_st[g_stN];
    strncpy(s.url,  t1 + 1, sizeof(s.url)  - 1); s.url[sizeof(s.url)  - 1] = 0;
    strncpy(s.name, t2 + 1, sizeof(s.name) - 1); s.name[sizeof(s.name) - 1] = 0;
    trimCr(s.name); trimCr(s.url);
    if (!s.url[0]) continue;

    int ti = ensureTag(line);
    s_tagOf[g_stN] = (uint8_t)(ti < 0 ? 0 : ti);
    g_stN++;
  }
  f.close();

  A.count = g_stN;
  if (A.cur < 0 || A.cur >= g_stN) A.cur = g_stN ? 0 : -1;
  return g_stN > 0;
}

// ---------------- 分组接口（1.15.0 起退化）----------------
// 调用点还不少（App / 云端 / 屏上），保留签名但恒为「只有一组」，
// 这样它们全都不需要改，也不会出现「切组」这种已经不存在的行为。
int         storeGroupCount()        { return 1; }
const char* storeGroupName(int)      { return ""; }
int         storeGroup()             { return 0; }
int         storeGroupStations(int)  { return g_stN; }
bool        storeSetGroup(int)       { return false; }
bool        storeShiftGroup(int)     { return false; }

// ---------------- 台表读写 ----------------
bool storeLoad() {
  return loadAll();
}

// 整份重写：直接按内存里的顺序写，一次搞定（不再需要「其他组原样搬」）。
// 先写 .tmp 再改名，掉电也不会把台表写坏。
void storeSave() {
  File out = LittleFS.open(F_TMP, "w");
  if (!out) return;
  for (int i = 0; i < g_stN; i++) {
    int t = s_tagOf[i];
    out.printf("%s\t%s\t%s\n", (t >= 0 && t < s_tagN) ? s_tag[t] : "", g_st[i].url, g_st[i].name);
  }
  out.close();
  commitTable();
  saveTags();
}

// 首次开机：把编译进固件的出厂台表灌进 LittleFS
void storeSeedIfEmpty() {
  // 光看「文件在不在」不够 —— 老机器烧了新固件，文件一直在，
  // 台表就永远停在出厂那版。所以比一个版本号：固件里的 PRESET_VER 变了就重灌。
  int have = -1;
  if (LittleFS.exists(F_STAMP)) {
    File s = LittleFS.open(F_STAMP, "r");
    if (s) { have = s.readStringUntil('\n').toInt(); s.close(); }
  }
  if (LittleFS.exists(F_ST) && have == PRESET_VER) return;

  // 重灌前先把用户可能改过的台表整份备份，别一句话不说就覆盖掉
  if (LittleFS.exists(F_ST)) {
    File src = LittleFS.open(F_ST, "r");
    File bak = LittleFS.open(F_STAMP_BAK, "w");
    if (src && bak) {
      uint8_t buf[512];
      size_t n;
      while ((n = src.read(buf, sizeof(buf))) > 0) bak.write(buf, n);
      Serial.println("[store] 台表版本升级，旧表已备份到 /stations.bak");
    }
    if (src) src.close();
    if (bak) bak.close();
  }

  // 标签表先写（顺序 = kPresetTag 的先后，App 的 chips 按这个排）
  s_tagN = 0;
  for (int t = 0; t < PRESET_TAG_N; t++) ensureTag(kPresetTag[t].name);
  saveTags();

  File f = LittleFS.open(F_ST, "w");
  if (!f) return;
  for (int i = 0; i < PRESET_ST_N; i++) {
    const PresetSt& S = kPreset[i];
    const char* tag = (kPresetTagOf[i] < PRESET_TAG_N)
                      ? kPresetTag[kPresetTagOf[i]].name : "";
    f.printf("%s\t%s\t%s\n", tag, S.id, S.name);
  }
  f.close();

  File s2 = LittleFS.open(F_STAMP, "w");
  if (s2) { s2.printf("%d\n", PRESET_VER); s2.close(); }

  Serial.printf("[store] 灌入出厂台表 %d 台 / %d 个标签（版本 %d）\n",
                PRESET_ST_N, PRESET_TAG_N, PRESET_VER);
}

int storeFind(const char* name) {
  for (int i = 0; i < g_stN; i++)
    if (strcmp(g_st[i].name, name) == 0) return i;
  return -1;
}

// 判空 + 纯数字 = 蜻蜓台号
static bool isQtfmId(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; p++) if (*p < '0' || *p > '9') return false;
  return strlen(s) <= 9;
}

// ------------------------------------------------------------
//  1.46.33：蜻蜓「节目页」地址
//  用户从蜻蜓 App / 网页复制过来的是这种：
//      https://www.qtfm.cn/channels/102974/programs/2808001
//  那是一个网页，播放器抓下来是一坨 HTML，所以「加了台却放不出声」。
//  真正的点播流在 audio.qtfm.cn 上，但必须带签名：
//      sign = HMAC-MD5(key, "/audiostream/redirect/<ch>/<pg>")   （hex）
//  签名只覆盖路径、不过期，所以每次起播现算一次就行。
// ------------------------------------------------------------
#define QTFM_SIGN_KEY  "7l8CZ)SgZgM_bkrw"

// 从各种蜻蜓节目页 URL 里抠出 (频道号, 节目号)；不是节目页返回 false
static bool parseQtfmPage(const char* u, char* ch, size_t chn, char* pg, size_t pgn) {
  if (!u || !chn || !pgn) return false;
  static const char* KW = "qtfm.cn/channels/";
  const char* p = strstr(u, KW);
  if (!p) return false;
  // 1.46.33b：这里原来写死 16，而 strlen("qtfm.cn/channels/") 是 17 ——
  //   指针停在 channels 后面那个 '/' 上，接着解析数字第一个字符就是 '/',
  //   于是判成「不是节目页」，签名一次都没算过，等于白加。
  //   现在用 strlen，不再手数。
  p += strlen(KW);
  size_t i = 0;
  while (*p >= '0' && *p <= '9' && i + 1 < chn) ch[i++] = *p++;
  ch[i] = 0;
  if (i == 0) return false;
  if (strncmp(p, "/programs/", 10) != 0) return false;
  p += 10;
  size_t j = 0;
  while (*p >= '0' && *p <= '9' && j + 1 < pgn) pg[j++] = *p++;
  pg[j] = 0;
  return j > 0;
}

static void hmacMd5Hex(const char* key, const char* msg, char* out, size_t n) {
  if (!out || n < 33) { if (n) out[0] = 0; return; }
  unsigned char mac[16];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  if (!info || mbedtls_md_hmac(info, (const unsigned char*)key, strlen(key),
                               (const unsigned char*)msg, strlen(msg), mac) != 0) {
    out[0] = 0;
    return;
  }
  static const char* H = "0123456789abcdef";
  size_t k = 0;
  for (int i = 0; i < 16; i++) { out[k++] = H[mac[i] >> 4]; out[k++] = H[mac[i] & 15]; }
  out[k] = 0;
}

// 拼出签名后的真实地址；不是节目页返回 false
static bool qtfmPageUrl(const char* u, char* out, size_t n) {
  char ch[12], pg[16];
  if (!parseQtfmPage(u, ch, sizeof(ch), pg, sizeof(pg))) return false;
  char path[48];
  snprintf(path, sizeof(path), "/audiostream/redirect/%s/%s", ch, pg);
  char sign[36];
  hmacMd5Hex(QTFM_SIGN_KEY, path, sign, sizeof(sign));
  if (!sign[0]) return false;
  snprintf(out, n, "https://audio.qtfm.cn%s?sign=%s", path, sign);
  return true;
}

// 把台表里存的东西还原成真正的播放地址
void storeUrl(int i, char* out, size_t n) {
  if (i < 0 || i >= g_stN || n == 0) { if (n) out[0] = 0; return; }
  const char* u = g_st[i].url;
  if (isQtfmId(u)) { snprintf(out, n, QTFM_PREFIX "%s" QTFM_SUFFIX, u); return; }
  // 1.46.33：节目页地址在这里现算签名（存的是原样，方便用户自己看/改）
  if (qtfmPageUrl(u, out, n)) return;
  snprintf(out, n, "%s", u);
}

// 存之前把地址收拾一下：节目页地址原样保留（要给人看得懂），
// 只把「带空格 / 前后引号」这类复制粘贴带进来的脏东西去掉。返回值表示能不能收。
bool storeUrlOk(const char* u) {
  if (!u || !*u) return false;
  return isQtfmId(u) || strncmp(u, "http", 4) == 0;
}

bool storeIsQtfm(int i) {
  return (i >= 0 && i < g_stN) && isQtfmId(g_st[i].url);
}

// tag：标签下标；<0 = 用「广播」（下标 0）
int storeAdd(const char* name, const char* url, int tag) {
  if (g_stN >= MAX_STATIONS) return -1;
  if (!url || (!isQtfmId(url) && strncmp(url, "http", 4) != 0)) return -1;
  Station& s = g_st[g_stN];
  strncpy(s.name, (name && *name) ? name : "未命名", sizeof(s.name) - 1);
  s.name[sizeof(s.name) - 1] = 0;
  strncpy(s.url, url, sizeof(s.url) - 1);
  s.url[sizeof(s.url) - 1] = 0;
  for (char* p = s.name; *p; p++) if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
  s_tagOf[g_stN] = (uint8_t)((tag >= 0 && tag < s_tagN) ? tag : 0);
  g_stN++;
  storeSave();
  A.count = g_stN;
  A.dirty = true;
  return g_stN - 1;
}

bool storeDel(int i) {
  if (i < 0 || i >= g_stN) return false;
  for (int k = i; k < g_stN - 1; k++) {
    g_st[k] = g_st[k + 1];
    s_tagOf[k] = s_tagOf[k + 1];
  }
  g_stN--;
  if (A.cur > i) A.cur--;
  else if (A.cur == i && A.cur >= g_stN) A.cur = g_stN ? g_stN - 1 : -1;
  storeSave();
  A.count = g_stN;
  A.dirty = true;
  return true;
}

bool storeMove(int i, int dir) {
  int j = i + dir;
  if (i < 0 || i >= g_stN || j < 0 || j >= g_stN) return false;
  Station t = g_st[i]; g_st[i] = g_st[j]; g_st[j] = t;
  uint8_t tt = s_tagOf[i]; s_tagOf[i] = s_tagOf[j]; s_tagOf[j] = tt;
  if (A.cur == i) A.cur = j;
  else if (A.cur == j) A.cur = i;
  storeSave();
  A.dirty = true;
  return true;
}

// ============================================================
//  1.4.0 手机端编辑
// ============================================================

// 改名 / 改地址
bool storeEdit(int i, const char* name, const char* url) {
  if (i < 0 || i >= g_stN) return false;
  if (url && *url) {
    if (!isQtfmId(url) && strncmp(url, "http", 4) != 0) return false;
    strncpy(g_st[i].url, url, sizeof(g_st[i].url) - 1);
    g_st[i].url[sizeof(g_st[i].url) - 1] = 0;
  }
  if (name && *name) {
    strncpy(g_st[i].name, name, sizeof(g_st[i].name) - 1);
    g_st[i].name[sizeof(g_st[i].name) - 1] = 0;
    for (char* p = g_st[i].name; *p; p++)
      if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
  }
  storeSave();
  A.dirty = true;
  return true;
}

// 拖到第 to 位（0 基）。用「抽出再插入」，一次写盘。
bool storeMoveTo(int i, int to) {
  if (i < 0 || i >= g_stN) return false;
  if (to < 0) to = 0;
  if (to >= g_stN) to = g_stN - 1;
  if (i == to) return true;

  Station t = g_st[i];
  uint8_t tt = s_tagOf[i];
  if (to > i) {
    for (int k = i; k < to; k++) { g_st[k] = g_st[k + 1]; s_tagOf[k] = s_tagOf[k + 1]; }
  } else {
    for (int k = i; k > to; k--) { g_st[k] = g_st[k - 1]; s_tagOf[k] = s_tagOf[k - 1]; }
  }
  g_st[to] = t;
  s_tagOf[to] = tt;

  // 当前正在播的那台跟着走
  if (A.cur == i) A.cur = to;
  else if (i < A.cur && to >= A.cur) A.cur--;
  else if (i > A.cur && to <= A.cur) A.cur++;

  storeSave();
  A.dirty = true;
  return true;
}

// 换标签：1.15.0 起**只改标签、不动位置**。
// 以前是「从当前组挪到目标组末尾」，那套流式重写整表的代码已经删了。
bool storeMoveToGroup(int i, int gTo, const char* newGroup) {
  if (i < 0 || i >= g_stN) return false;

  int t = gTo;
  if (gTo < 0) {                       // 用新名字建标签
    if (!newGroup || !*newGroup) return false;
    t = ensureTag(newGroup);
    if (t < 0) return false;
  }
  if (t < 0 || t >= s_tagN) return false;
  if (s_tagOf[i] == (uint8_t)t) return true;

  s_tagOf[i] = (uint8_t)t;
  storeSave();                         // 里面会顺带 saveTags()
  A.dirty = true;
  return true;
}

// ---------------- 标签管理（App 的「标签管理」页）----------------
bool storeGroupAdd(const char* name) {
  if (!name || !*name) return false;
  if (strlen(name) >= GROUP_NAME_LEN) return false;
  if (findTag(name) >= 0) return false;
  if (ensureTag(name) < 0) return false;
  saveTags();
  A.dirty = true;
  return true;
}

int storeGroupEnsure(const char* name) {
  int g = findTag(name);
  if (g >= 0) return g;
  if (!storeGroupAdd(name)) return -1;
  return findTag(name);
}

// 改名：s_tagOf 存的是下标，所以所有台自动跟着走，只需要重写台表 + 标签表
bool storeGroupRename(int g, const char* name) {
  if (g < 0 || g >= s_tagN || !name || !*name) return false;
  if (strlen(name) >= GROUP_NAME_LEN) return false;
  if (strcmp(s_tag[g], name) == 0) return false;
  if (findTag(name) >= 0) return false;          // 不合并，交给 App 提示

  strncpy(s_tag[g], name, GROUP_NAME_LEN - 1);
  s_tag[g][GROUP_NAME_LEN - 1] = 0;
  storeSave();
  A.dirty = true;
  return true;
}

// 删标签：**不删台**，这些台退回第 0 个标签（广播）。
// 以前是「连台一起删」—— 现在 268 个电台全在「广播」一个标签下，
// 一个误操作就能把整台机器的台清空，太危险。
// 第 0 个标签是「退回目标」，删了它这些台就无处可退（还会被下一行的名字顶掉），
// 所以 g == 0 一律拒绝 —— App 里那颗删除键对第一个标签也是灰的。
bool storeGroupDel(int g) {
  if (g <= 0 || g >= s_tagN || s_tagN <= 1) return false;

  for (int i = 0; i < g_stN; i++) {
    if (s_tagOf[i] == (uint8_t)g) s_tagOf[i] = 0;
    else if (s_tagOf[i] > (uint8_t)g) s_tagOf[i]--;
  }
  for (int k = g; k < s_tagN - 1; k++) strcpy(s_tag[k], s_tag[k + 1]);
  s_tagN--;

  storeSave();
  A.dirty = true;
  return true;
}

// 标签排序：只动 chips 的先后，**不影响播放顺序**
bool storeGroupMove(int g, int dir) {
  int j = g + dir;
  if (g < 0 || g >= s_tagN || j < 0 || j >= s_tagN) return false;

  char t[GROUP_NAME_LEN];
  strcpy(t, s_tag[g]);
  strcpy(s_tag[g], s_tag[j]);
  strcpy(s_tag[j], t);

  for (int i = 0; i < g_stN; i++) {
    if (s_tagOf[i] == (uint8_t)g)      s_tagOf[i] = (uint8_t)j;
    else if (s_tagOf[i] == (uint8_t)j) s_tagOf[i] = (uint8_t)g;
  }
  storeSave();
  A.dirty = true;
  return true;
}

// ---------------- 设置 ----------------
// 1.44.0：音量刻度整个换了。1.39.0～1.43.0 存的是「3..10 档」，
//   直接当原始值读进来会变成「音量 5」—— 网络那一路 5/21 只有 24%，
//   比原来 5/10 小一半，用户的第一感受就是「升级后变小声了」。
//   所以老配置按比例迁移一次，写 volscale=2 做标记，之后不再迁。
static int lvToNet(int lv) {         // 老 1..10 档 → 现在的 6..21
  if (lv < 1)  lv = 1;
  if (lv > 10) lv = 10;
  return NVOL_MIN + (lv - 1) * (NVOL_MAX - NVOL_MIN) / 9;
}
static int lvToFm(int lv) {          // 老 1..10 档 → 现在的 6..63
  if (lv < 1)  lv = 1;
  if (lv > 10) lv = 10;
  return FVOL_MIN + (lv - 1) * (FVOL_MAX - FVOL_MIN) / 9;
}

bool storeLoadCfg() {
  A.vol = NVOL_DEFAULT;
  A.fmVol = FVOL_DEFAULT;            // 1.22.0
  A.squelch = SQUELCH_DEFAULT;       // 1.22.0
  A.halfstep = HALFSTEP_DEFAULT;     // 1.22.0
  A.bright = 100;
  A.brightMax = 100;
  A.scrOffMin = SCR_OFF_MS / 60000;
  if (!LittleFS.exists(F_CFG)) return false;
  File f = LittleFS.open(F_CFG, "r");
  if (!f) return false;
  bool scale2 = false;               // 新刻度标记（1.44.0）
  int  rawVol = -1, rawFm = -1;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int eq = ln.indexOf('=');
    if (eq <= 0) continue;
    String k = ln.substring(0, eq), v = ln.substring(eq + 1);
    // 1.44.0：先原样记下来，等认完整个文件再决定「这是档位还是原始值」
    if (k == "vol")      rawVol = v.toInt();
    if (k == "volscale") scale2 = (v.toInt() >= 2);
    // 1.22.0：收音音量 / 静噪 / 半步进
    if (k == "fmvol")    rawFm = v.toInt();
    if (k == "sq")     A.squelch = constrain(v.toInt(), 0, 100);
    if (k == "half")   A.halfstep = (v.toInt() != 0);
    if (k == "bright") A.bright = constrain(v.toInt(), 0, 100);
    if (k == "bmax")   A.brightMax = constrain(v.toInt(), 5, 100);
    if (k == "offmin") A.scrOffMin = constrain(v.toInt(), 0, 60);
    if (k == "cur")    A.cur = v.toInt();
    // 1.40.1：上次开的是收音还是网络。
    //   存下来的唯一用途：没有网的机器开机时**第一帧就画收音界面**，
    //   不再先画「网络广播」再闪一下切成「广播」（用户原话：开机闪一下网络广播页面）。
    if (k == "fmmode") A.fmModeBoot = (v.toInt() != 0);
    // 1.46.59：上次调到的收音频率（kHz）。自动搜台删掉之后，开机要回到这台。
    if (k == "fmfreq") A.fmFreq = v.toInt();
    // 1.15.0 起没有 grp 了（不再分组）。老配置里那行读到时直接忽略 ——
    // 而且台表版本号变了会重灌，A.cur 在 storeLoad 里已经被夹进范围。
  }
  f.close();
  // 1.44.0：新配置直接用；老配置（没有 volscale 行）按比例迁到新刻度
  if (rawVol >= 0) A.vol   = scale2 ? constrain(rawVol, NVOL_MIN, NVOL_MAX) : lvToNet(rawVol);
  if (rawFm  >= 0) A.fmVol = scale2 ? constrain(rawFm,  FVOL_MIN, FVOL_MAX) : lvToFm(rawFm);
  if (A.cur < 0 || A.cur >= g_stN) A.cur = g_stN ? 0 : -1;
  return true;
}

void storeSaveCfg() {
  File f = LittleFS.open(F_CFG, "w");
  if (!f) return;
  // 1.22.0：加 fmvol / sq / half。老固件读到未知键会忽略，新固件读老配置会用默认值，
  //   两个方向都不会坏。
  // 1.44.0：第一行 volscale=2 标记刻度。老固件读到未知键会忽略（无害），
  //   新固件看到它就按原始值读，不再迁移。
  f.printf("volscale=2\nvol=%d\nfmvol=%d\nsq=%d\nhalf=%d\nbright=%d\nbmax=%d\noffmin=%d\ncur=%d\nfmmode=%d\nfmfreq=%d\n",
           A.vol, A.fmVol, A.squelch, A.halfstep ? 1 : 0,
           A.bright, A.brightMax, A.scrOffMin, A.cur,
           A.fmModeBoot ? 1 : 0, A.fmFreq);
  f.close();
}

void storeSaveDebounced() {
  s_dirty = true;
  s_dirtyAt = millis();
}

// ------------------------------------------------------------
//  1.46.29：治「调完音量空一会才继续播」
//
//  原来：音量一改 → storeSaveDebounced() → 停手 1.5 秒后 storeSaveCfg()。
//  而 storeSaveCfg 是「LittleFS 改一个文件」= 擦一个 4KB 块 + 写元数据，
//  这几十毫秒里 **flash cache 是停的**，音频任务的代码也在 flash 上，
//  解码停一下 → I2S 没有新数据 → 用户听到「空一下才继续播」。
//  时间点又刚好卡在「刚调完音量、手一松」那一刻，所以听起来像是音量的锅。
//
//  现在两件事分开：
//    · 音量（4 个字节）→ NVS。NVS 是「往已擦好的页里追加一条 32 字节记录」，
//      不擦块，实测 1~2ms，听不出来；只有页写满才压一次（约每 100 次调音量）。
//    · 其余设置（当前台 / 亮度 / 静噪 / 半步进…）→ 仍旧 LittleFS，
//      但只在 audioWriteSafe() 为真的时刻写：换台连接中、暂停、念播报、沙沙、FM。
//  ⇒ 听节目的时候再也不写 LittleFS，也就不会再出现空档。
// ------------------------------------------------------------
#define STORE_DEBOUNCE_MS 1500

void storeSaveVolFast() {
  Preferences p;
  if (p.begin("myradio", false)) {
    uint8_t b[2];
    b[0] = (uint8_t)constrain(A.vol,   0, 255);
    b[1] = (uint8_t)constrain(A.fmVol, 0, 255);
    p.putBytes("v", b, sizeof(b));        // 一次写两个字节，省一次落盘
    p.end();
  }
  // cfg 里那份仍旧更新（老固件回滚时还认得），只是挪到静音窗口再写
  s_dirty = true;
  s_dirtyAt = millis();
}

void storeLoadVolNvs() {
  Preferences p;
  if (!p.begin("myradio", true)) return;   // 还没有这个命名空间 = 第一次跑新固件
  uint8_t b[2] = { 0, 0 };
  const size_t n = p.getBytes("v", b, sizeof(b));
  p.end();
  if (n != sizeof(b)) return;
  A.vol   = constrain((int)b[0], NVOL_MIN, NVOL_MAX);
  A.fmVol = constrain((int)b[1], FVOL_MIN, FVOL_MAX);
  Serial.printf("[store] 音量取自 NVS：网络 %d / 收音 %d\n", A.vol, A.fmVol);
}

void storeFlush() {
  s_dirty = false;
  storeSaveCfg();
}

void storeTick() {
  if (!s_dirty) return;
  if (millis() - s_dirtyAt <= STORE_DEBOUNCE_MS) return;
  // 正在出声就先不写 —— 写完这一笔，用户的节目会断一下（见上面那段注释）
  if (!audioWriteSafe()) return;
  s_dirty = false;
  storeSaveCfg();
}

// ---------------- 配对令牌 ----------------
const char* storeToken() {
  if (s_token[0]) return s_token;
  if (LittleFS.exists(F_TOK)) {
    File f = LittleFS.open(F_TOK, "r");
    if (f) {
      String t = f.readStringUntil('\n');
      f.close();
      t.trim();
      if (t.length() >= 6) {
        strncpy(s_token, t.c_str(), sizeof(s_token) - 1);
      }
    }
  }
  return s_token[0] ? s_token : nullptr;
}

void storeSetToken(const char* t) {
  strncpy(s_token, t, sizeof(s_token) - 1);
  s_token[sizeof(s_token) - 1] = 0;
  File f = LittleFS.open(F_TOK, "w");
  if (f) { f.print(s_token); f.print('\n'); f.close(); }
}
