// ============================================================
//  net.cpp —— WiFi / 配网 / REST API（给手机 App 用）
//
//  1.4.0 的 App 比 1.2.0 能做更多事，所以这一层也厚了：
//    /api/screen   屏幕快照（App 主页那块「实时屏幕」）
//    /api/history  收听记录 + 每日时长
//    /api/diag     体检（网络/内存/存储/音频/时钟）
//    /api/fix      一键修复（重开台、重连、重启、恢复出厂…）
//    /api/edit /api/move /api/mvgroup /api/groupop  台与分类的增删改序
//    /api/pair     配对与绑定管理（可开 6 位配对码）
//    /api/wifi     改成「异步试连 + 失败回退」，换网不用先重启
//
//  设计底线没变：老人只要会转旋钮；这些接口都是子女用的。
// ============================================================
#include "net.h"
#include "health.h"
#include "offair.h"        // 1.46.6：停播台账（/api/offair）
#include "power.h"          // 1.21.0：/api/batt 里带温度与降载档位
#include "audio.h"
#include "store.h"
#include "history.h"
#include "evlog.h"       // 1.46.39：/api/log
#include "main.h"        // 1.46.43：fmTestBegin
#include "input.h"
#include "ui.h"
#include "album.h"
#include "fm.h"
#include "api.h"
#include "cloud.h"
#include "battery.h"
#include "voice.h"
#include "webapp.h"
#include "provpage.h"      // 1.45.0：配网页（原厂那套版式）
#include "pairpin.h"       // 1.46.5：4 位手机连接码

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <DNSServer.h>     // 1.45.0：热点下把所有域名指回本机（自动弹配网页）
#include <ESP.h>
#include <esp_system.h>
#include <ctype.h>
#include <time.h>

static WebServer server(80);

static bool  s_apMode  = false;
static DNSServer s_dns;                 // 1.45.0：captive portal 用的 DNS
static bool     s_dnsOn = false;
static bool  s_haveCred = false;
static char  s_ip[20]  = "-";
static char  s_tok[17] = {0};

static uint32_t s_lastTry = 0;
// 1.46.36：删掉「当前正在用的那组」WiFi 之后，等回包出去再断开
static uint32_t s_wifiDropAt = 0;
static uint32_t s_lastNtp = 0;
static uint32_t s_bootAt  = 0;

// ------------------------------------------------------------
//  配对码（可选保护）
//    默认关：手机打开 App 就自动绑定，跟 1.2.0 一样。
//    打开后：新手机必须抄屏幕上那 6 个数字。
// ------------------------------------------------------------
static const char* F_PAIR = "/pair.txt";
static bool s_locked = false;
static char s_code[8] = {0};

// ------------------------------------------------------------
//  WiFi 换网：先试连，成功才写盘；失败自动退回原来的
// ------------------------------------------------------------
static String   s_pendSsid, s_pendPass;
static bool     s_pend = false;
// 1.46.51：这次换网成功**不要**把它置顶（App 点「连这个」用的，顺序由用户排）
static bool     s_pendKeepOrder = false;
static int      s_wifiState = 0;        // 0 空闲 1 连接中 2 成功 3 失败
static char     s_wifiMsg[56] = "";
static uint32_t s_tryAt = 0;
static String   s_oldSsid, s_oldPass;

// ---- 1.22.0：多组 WiFi 的前向声明 ----
//   真正的实现在文件后半（WiFi 底层那一节），但 HTTP handler 在前面就要用。
//   struct 必须先完整定义，不能只前置声明 —— handler 里要按值建数组。
#define WIFI_MAX_SLOTS 4     // 1.46.51：3 -> 4（原厂配网页是 3 组，这里多留一组）
struct WifiCred { String ssid, pass; };
static int  readWifiSlots(WifiCred out[WIFI_MAX_SLOTS]);
static void saveWifiFile(const String& ssid, const String& pass);
static int  forgetWifiSlot(const String& ssid);   // 1.46.36：1/0/-1 三态

// ---- 1.22.0：多组 WiFi 的自动轮换 ----
//   s_slotIdx   正在试第几组（0..n-1）
//   s_slotN     一共存了几组
//   s_everUp    本次上电有没有成功连上过（连上过就不再轮换）
//   s_slotTryAt 这一组从什么时候开始试
#define NET_CRED_TRY_MS 18000        // 一组试 18 秒（DHCP 慢的也要够）
static int      s_slotIdx  = 0;
static int      s_slotN    = 0;
static bool     s_everUp   = false;
static uint32_t s_slotTryAt = 0;

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------
// 轻量随机（配对令牌用，不需要密码学强度）
static uint32_t s_rng = 0;
static uint32_t rnd() {
  if (!s_rng) s_rng = (uint32_t)micros() ^ (uint32_t)ESP.getEfuseMac() ^ 0x9E3779B9u;
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

static void jsonEsc(String& out, const char* s) {
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
    else if ((uint8_t)*p < 0x20) out += ' ';
    else out += *p;
  }
}

static String argStr(const char* k, const char* def = "") {
  return server.hasArg(k) ? server.arg(k) : String(def);
}

static bool authed() {
  if (!s_tok[0]) return false;                       // 还没配对，只认配网接口
  String h = server.header("X-Token");
  if (h.length() && h == s_tok) return true;
  String q = server.arg("t");
  return q.length() && q == s_tok;
}

static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "X-Token,Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Cache-Control", "no-store");
}

static void denied() {
  cors();
  server.send(401, "application/json; charset=utf-8", "{\"err\":\"unauthorized\"}");
}

static void okJson(const char* body) {
  cors();
  server.send(200, "application/json; charset=utf-8", body);
}

static void errJson(int code, const char* e) {
  cors();
  String o = "{\"err\":\"";
  o += e;
  o += "\"}";
  server.send(code, "application/json; charset=utf-8", o);
}

// 一个 JSON 字符串字段（自动转义）
static void kv(String& o, const char* k, const char* v) {
  o += '"'; o += k; o += "\":\"";
  jsonEsc(o, v ? v : "");
  o += '"';
}

static const char* tipText() {
  if (!A.wifiOk) return "等下我重连";
  if (A.ps == PS_ERR) return "换个台试试";
  if (A.ps == PS_BUFF) return "稍等缓冲";
  return "旋钮换台";            // 1.15.0：不再有「左右换组」这句
}

static const char* resetText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "程序崩溃";
    case ESP_RST_INT_WDT:   return "看门狗";
    case ESP_RST_TASK_WDT:  return "任务卡死";
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_BROWNOUT:  return "电压不足";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_EXT:       return "外部复位";
    default: break;
  }
  // 1.46.23：以前一律写「未知」，出问题时等于没有信息。现在把码打出来：
  //   0=判不出来 / 2=？ / 9=电压不足…… 对照 esp_reset_reason_t 就能定位。
  static char s_buf[24];
  snprintf(s_buf, sizeof(s_buf), "未知(%d)", (int)r);
  return s_buf;
}

// ------------------------------------------------------------
//  配对码
// ------------------------------------------------------------
static void newCode() {
  for (int i = 0; i < 6; i++) s_code[i] = (char)('0' + (rnd() % 10));
  s_code[6] = 0;
}

static void savePair() {
  File f = LittleFS.open(F_PAIR, "w");
  if (!f) return;
  f.printf("lock=%d\ncode=%s\n", s_locked ? 1 : 0, s_code);
  f.close();
}

static void loadPair() {
  if (!LittleFS.exists(F_PAIR)) return;
  File f = LittleFS.open(F_PAIR, "r");
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int eq = ln.indexOf('=');
    if (eq <= 0) continue;
    String k = ln.substring(0, eq), v = ln.substring(eq + 1);
    if (k == "lock") s_locked = (v.toInt() != 0);
    if (k == "code") { strncpy(s_code, v.c_str(), sizeof(s_code) - 1); s_code[6] = 0; }
  }
  f.close();
  if (s_locked && strlen(s_code) != 6) { newCode(); savePair(); }
}

// ------------------------------------------------------------
//  基础信息
// ------------------------------------------------------------
static void hInfo() {
  cors();
  String o = "{";
  kv(o, "fw", FW_NAME " " FW_VER);
  o += ',';
  kv(o, "name", FW_NAME);
  o += ',';
  o += "\"paired\":"; o += s_tok[0] ? 1 : 0;
  o += ",\"needSetup\":"; o += netNeedSetup() ? 1 : 0;
  o += ",\"ap\":"; o += s_apMode ? 1 : 0;
  o += ",\"locked\":"; o += s_locked ? 1 : 0;
  o += ',';
  kv(o, "ip", s_ip);
  o += ",\"count\":"; o += g_stN;
  o += ",\"up\":"; o += (millis() - s_bootAt) / 1000;
  // 1.8.0：手机要拿这两个码才能在外面连上这台机器
  o += ",\"devid\":\""; o += cloudDevId(); o += '"';
  o += ",\"rcode\":\""; o += cloudCode(); o += '"';
  o += ",\"cloud\":"; o += cloudOn() ? 1 : 0;
  o += ",\"cloudConn\":"; o += cloudConnected() ? 1 : 0;
  // 1.19.0：免配对开关 + 派生码（App 在局域网里也能拿到它，跨网时就自己算）
  o += ",\"open\":"; o += cloudOpen() ? 1 : 0;
  o += ",\"dcode\":\""; o += cloudDerivedCode(); o += '"';
  o += ',';
  batteryJson(o);
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  1.8.0 跨网远程控制
//    GET  /api/cloud   看状态（开关 / 设备码 / 遥控码 / 服务器 / 连没连上）
//    POST /api/cloud   on=0|1 开关
//                      code=123456 手动指定遥控码
//                      newcode=1    随机换一个
//                      host=xxx:1883  换中转服务器
// ------------------------------------------------------------
static void hCloud() {
  if (!authed()) { denied(); return; }
  cors();
  if (server.method() == HTTP_POST) {
    if (server.hasArg("on")) cloudSetOn(argStr("on", "1").toInt() != 0);
    if (server.hasArg("newcode")) cloudNewCode();
    if (server.hasArg("code")) {
      if (!cloudSetCode(argStr("code").c_str())) { errJson(400, "code"); return; }
    }
    if (server.hasArg("host")) {
      if (!cloudSetHost(argStr("host").c_str())) { errJson(400, "host"); return; }
    }
    // 1.19.0：免配对（广播 + 认派生码）
    if (server.hasArg("open")) cloudSetOpen(argStr("open", "1").toInt() != 0);
  }
  String o = "{";
  cloudJson(o);
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

// 自动配对：手机打开 App 就来取令牌，不需要任何配对码。
// 令牌首次生成后固定存在 flash 里，所以「这台机器 + 这个 App」是绑死的。
// 开了配对码保护后，必须带上 ?code=123456 才给。
static void hAuth() {
  cors();
  String code = argStr("code");
  if (s_locked) {
    if (!code.length() || code != s_code) {
      // 屏幕给出 6 位码，家里人对一下就能输入（60 秒后退回主界面）
      if (!uiPairCodeActive()) uiPairCode(s_code);
      okJson("{\"locked\":1}");
      return;
    }
  }

  if (!s_tok[0]) {
    char t[17];
    for (int i = 0; i < 16; i++) t[i] = "0123456789abcdef"[rnd() & 0x0F];
    t[16] = 0;
    storeSetToken(t);
    strncpy(s_tok, t, sizeof(s_tok) - 1);
    A.dirty = true;
    Serial.printf("[net] token created %s\n", s_tok);
  }
  uiPairCodeClear();
  String o = "{\"token\":\"";
  o += s_tok;
  o += "\",\"locked\":0}";
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  状态 / 屏幕快照
// ------------------------------------------------------------
static void hState() {
  if (!authed()) { denied(); return; }
  cors();
  String o = "{\"cur\":";
  o += A.cur;
  o += ',';
  kv(o, "name", (A.cur >= 0 && A.cur < g_stN) ? g_st[A.cur].name : "");
  o += ",\"count\":"; o += g_stN;
  o += ",\"group\":"; o += A.group;
  o += ",\"gcount\":"; o += storeGroupCount();
  o += ',';
  // 1.15.0：gname 改成「当前这台所属的标签」
  kv(o, "gname", storeTag(A.cur));
  o += ",\"ps\":"; o += (int)A.ps;
  o += ",\"vol\":"; o += A.vol;
  o += ",\"mute\":"; o += A.muted ? 1 : 0;
  o += ",\"wifi\":"; o += A.wifiOk ? 1 : 0;
  o += ",\"rssi\":"; o += A.rssi;
  o += ",\"mode\":\""; o += A.fmMode ? "fm" : "net"; o += "\"";
  o += ",\"fmfreq\":"; o += (A.fmMode && radioBand(A.fmFreq) == RADIO_FM) ? (A.fmFreq / 10) : 0;
  o += ",\"fmrssi\":"; o += A.fmRssi;
  o += ",\"khz\":"; o += A.fmFreq;
  o += ",\"band\":\""; o += radioBandCode(A.fmFreq); o += "\"";
  // 1.38.0：自动搜台状态，App 首页能显示「正在搜台 45% / 共 12 个台」
  o += ",\"fmscan\":"; o += fmScanBusy() ? 1 : 0;
  o += ",\"fmprog\":"; o += fmScanProgress();
  o += ",\"fmstn\":";  o += fmScanCount();
  o += ",\"on\":"; o += A.screenOn ? 1 : 0;
  o += ",\"bmax\":"; o += A.brightMax;
  o += ",\"offmin\":"; o += A.scrOffMin;
  // 1.41.0：App 设置页要显示「收音机那一路」的真实值。
  //   A.squelch / A.halfstep / A.fmVol 本来就存在（1.22.0 加的），只是从来没往 App 送过，
  //   于是 App 只能瞎猜或者干脆不做这两项设置（原厂的 currentSquelch / halfstep 就是这么丢的）。
  o += ",\"sq\":";  o += A.squelch;
  o += ",\"hs\":";  o += A.halfstep ? 1 : 0;
  o += ",\"fmvol\":"; o += A.fmVol;
  // 1.41.0：把量程一起送出去。App 的滑条原来写死 min=0 max=100，
  //   而 1.39.0 起音量是 3..10 档 ⇒ 拖到中间就被夹回 10，数字会突然跳回。
  //   App 拿到这两个数才能把滑条画对（老固件没这两个字段时有兜底）。
  // 1.44.0：两路量程不同了（原厂 volume 0..63 / netvolume 0..21），分别送。
  //   volmin/volmax 一直是「网络那一路」，老 App 照旧能用；
  //   fvolmin/fvolmax 是新增，App 的设置页用它画「音量 · 收音机」那条滑条。
  o += ",\"volmin\":"; o += NVOL_MIN;
  o += ",\"volmax\":"; o += NVOL_MAX;
  o += ",\"fvolmin\":"; o += FVOL_MIN;
  o += ",\"fvolmax\":"; o += FVOL_MAX;
  o += ',';
  kv(o, "ip", s_ip);
  char clk[8] = "--:--";
  netClockStr(clk, sizeof(clk));
  o += ',';
  kv(o, "clock", clk);
  o += ",\"fw\":\"" FW_VER "\"";
  o += ',';
  batteryJson(o);              // 1.19.0：设置页/主页要显示电量
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

// App 主页那块屏：字段和 ui.cpp 六个分区一一对应，前端照着画就行
static void hScreen() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  apiScreenJson(o);            // 1.8.0：与跨网那条通道共用同一份 JSON
  server.send(200, "application/json; charset=utf-8", o);
}

// 整张台表 + 标签清单（App 顶部那排可点的标签靠它；1.15.0 起返回全表）
static void hStations() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  apiStationsJson(o);          // 1.8.0：与跨网那条通道共用同一份 JSON
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  播放控制
// ------------------------------------------------------------
static void hPlay() {
  if (!authed()) { denied(); return; }
  cors();
  int id = argStr("id", "-1").toInt();
  if (id >= 0 && id < g_stN) { audioPlayIndex(id); actTouch(); }
  okJson("{\"ok\":1}");
}

static void hStep() {
  if (!authed()) { denied(); return; }
  cors();
  audioNext(argStr("dir", "1").toInt() >= 0 ? 1 : -1);
  actTouch();
  okJson("{\"ok\":1}");
}

// 有声书（1.9.0）：一部书一个台，集表与进度都归 epis 管
//   POST id=3           → 这一台的集表 {"prog","n","cur","list":[{i,t}]}
//   POST dir=-1|1       → 正在播的这个台 上一集/下一集
//   POST e=5            → 直接跳到第 6 集
static void hEpis() {
  if (!authed()) { denied(); return; }
  cors();

  if (server.hasArg("dir") || server.hasArg("e")) {
    // 1.46.33：e 可以带 id（App 的选集弹层不再先把机器切过去，点某一集时才说台号）
    bool ok;
    if (server.hasArg("e")) {
      const int st = server.hasArg("id") ? argStr("id", "-1").toInt() : A.cur;
      ok = apiEpisJumpAt(st, argStr("e", "0").toInt());
    } else {
      ok = apiEpisStep(argStr("dir", "1").toInt() >= 0 ? 1 : -1);
    }
    okJson(ok ? "{\"ok\":1}" : "{\"ok\":0}");
    return;
  }

  int id = server.hasArg("id") ? argStr("id", "-1").toInt() : A.cur;
  // 1.18.0：集表按页取（官场现形记 1167 集，一次发不完）
  int from = server.hasArg("from") ? argStr("from", "0").toInt() : 0;
  String o;
  if (id < 0 || id >= g_stN || !apiEpisJson(id, o, from))
    o = "{\"prog\":\"\",\"n\":0,\"cur\":-1,\"from\":0,\"list\":[]}";
  server.send(200, "application/json; charset=utf-8", o);
}

// 1.46.33：「下次调到这个台时，从第 X 集起」
//   POST id=3&e=42  → 设成第 43 集（0 基 42）
//   POST id=3&e=-1  → 取消这个设置
//   !! 只是写个标记：不换台、不起播、不动听到的进度（用户明确要求）。
static void hEpNext() {
  if (!authed()) { denied(); return; }
  cors();
  const int st = argStr("id", "-1").toInt();
  const int e  = server.hasArg("e") ? argStr("e", "-1").toInt() : -1;
  if (st < 0 || st >= g_stN) { errJson(400, "id"); return; }
  if (!apiEpNext(st, e)) { errJson(409, "notbook"); return; }
  String o = "{\"ok\":1,\"next\":";
  o += (e < 0) ? -1 : e;
  o += "}";
  okJson(o.c_str());
}

// 1.15.0：分组没了，这个接口只留个空壳 —— 老 App / 老远程页还会调它，
// 回 ok:0 让它们知道「没切成功」，不要静默当成成功。
static void hGroup() {
  if (!authed()) { denied(); return; }
  cors();
  String o = "{\"ok\":0,\"group\":0,\"msg\":\"1.15.0 起不再分组\"}";
  server.send(200, "application/json; charset=utf-8", o);
}

// 设置项（1.4.4）：App 设置页 → 收音机，改完立刻生效并存进 cfg
static void hSet() {
  if (!authed()) { denied(); return; }
  cors();
  String k = argStr("k");
  String v = argStr("v");

  if (k == "bright") {                       // 亮度上限：立刻生效，之后自动省电按比例压
    A.brightMax = constrain(v.toInt(), 5, 100);
    A.bright = A.brightMax;
    A.lastAct = millis();                    // 别刚设完就被省电逻辑压回去
  } else if (k == "offmin") {                // 熄屏延时（分钟，0=从不）
    A.scrOffMin = constrain(v.toInt(), 0, 60);
  } else if (k == "screen") {                // 0=立即熄屏，1=点亮
    if (v == "0") { A.screenOn = false; A.dirty = true; }
    else          { A.lastAct = millis(); if (!A.screenOn) { A.screenOn = true; A.dirty = true; } }
  } else {
    errJson(400, "k");
    return;
  }
  storeSaveCfg();
  String o = "{\"ok\":1,\"bmax\":"; o += A.brightMax;
  o += ",\"offmin\":"; o += A.scrOffMin; o += "}";
  okJson(o.c_str());
}

static void hVol() {
  if (!authed()) { denied(); return; }
  cors();
  const int v = argStr("v", "5").toInt();
  // 1.41.0：原厂是 volume / netvolume 两套。App 设置页把两条滑条分开给，
  //   所以这里允许指定改哪一路（ch=net|fm）。
  //   目标那一路**不是当前模式**时只写状态不碰硬件 —— 等切过去时
  //   enterFm() / audioPlayIndex() 会自己用这个值。
  //
  // 1.44.0：两路的量程不一样了（FM 6..63 / 网络 6..21），必须**分开夹**。
  //   原来这里先夹了一刀共用区间（3..10），FM 现在能到 63 —— 先夹就没了。
  String ch = argStr("ch");
  if (ch == "fm") {
    A.fmVol = constrain(v, FVOL_MIN, FVOL_MAX);
    if (A.fmMode) fmSetVolume(A.fmVol);      // 正在收音才立刻写芯片
    storeSaveDebounced();
  } else if (ch == "net") {
    A.vol = constrain(v, NVOL_MIN, NVOL_MAX);
    if (!A.fmMode) audioSetVol(A.vol);       // 正在放网络才立刻送解码器
    storeSaveDebounced();
  } else {
    // 没指定 —— 改的就是当前这一路，走原来的分流
    audioSetVol(v);
  }
  actTouch();
  String o = "{\"ok\":1,\"vol\":"; o += A.vol;
  o += ",\"fmvol\":"; o += A.fmVol; o += "}";
  okJson(o.c_str());
}

// ---- 1.22.0：静噪 / 半步进 ----
static void hSquelch() {
  if (!authed()) { denied(); return; }
  cors();
  apiSquelch(argStr("v", "0").toInt());
  okJson("{\"ok\":1}");
}

// ---- 1.38.0：自动搜台（App 里点「重新搜台」）----
// 1.46.33：以前不管搜没搜成都只回 {"ok":1} —— 用户在网络模式里点「重新搜台」，
//   fmScanStart() 一句不吭就返回了（芯片没初始化 / 正在搜都会早退），
//   App 只能显示「正在搜台…」然后一直等，用户看到的就是「没发出去」。
//   现在把三种情况和结果都如实说清楚。
static void hFmScan() {
  if (!authed()) { denied(); return; }
  cors();
  if (fmScanBusy()) { okJson("{\"ok\":1,\"started\":0,\"err\":\"busy\"}"); return; }
  if (!A.fmMode)    { okJson("{\"ok\":0,\"started\":0,\"err\":\"notfm\"}"); return; }
  fmScanStart();
  okJson(fmScanBusy() ? "{\"ok\":1,\"started\":1}"
                      : "{\"ok\":0,\"started\":0,\"err\":\"chip\"}");
}

// 搜到的台表：{n:台数, list:[{khz,rssi,band}]}
// 1.41.0：把收音机直接调到某个频率（App 的「搜到的台」列表点一下就跳过去）。
//   只在收音模式里生效 —— 网络模式下调芯片没有意义，也没有声音。
static void hFmTune() {
  if (!authed()) { denied(); return; }
  cors();
  if (!A.fmMode) { errJson(409, "notfm"); return; }
  int khz = argStr("khz", "0").toInt();
  if (khz <= 0) { errJson(400, "khz"); return; }
  fmTune(radioSnap(khz));
  A.fmFreq = radioSnap(khz);
  A.fmRssi = fmRssi();
  A.dirty  = true;
  actTouch();
  String o = "{\"ok\":1,\"khz\":"; o += A.fmFreq; o += "}";
  okJson(o.c_str());
}

static void hFmStations() {
  if (!authed()) { denied(); return; }
  cors();
  String o = "{\"n\":";
  o += fmScanCount();
  o += ",\"busy\":";
  o += fmScanBusy() ? 1 : 0;
  // 1.46.47：把进度也报出来 —— 「进度一直 0%」这类问题必须能远程看
  o += ",\"pct\":";
  o += fmScanProgress();
  o += ",\"list\":[";
  for (int i = 0; i < fmScanCount(); i++) {
    if (i) o += ',';
    o += "{\"khz\":";
    o += fmScanFreq(i);
    o += ",\"rssi\":";
    o += fmScanRssi(i);
    o += ",\"band\":\"";
    o += radioBandCode(fmScanFreq(i));
    o += "\"}";
  }
  o += "]}";
  okJson(o.c_str());
}

// ---- 1.38.2：电量校准（App 里填万用表量到的真实电压，单位 mV）----
static void hBattCal() {
  if (!authed()) { denied(); return; }
  cors();
  batteryCalibrate(argStr("mv", "0").toInt());
  okJson("{\"ok\":1}");
}

static void hHalfStep() {
  if (!authed()) { denied(); return; }
  cors();
  apiHalfStep(argStr("on", "0").toInt() != 0);
  okJson("{\"ok\":1}");
}

// ---- 1.22.0：已存的 WiFi 列表（最多 3 组）----
// 1.46.33：JSON 拼装抽出来，局域网与跨网共用（别再各写一份口径）
void apiWifiListJson(String& o) {
  WifiCred v[WIFI_MAX_SLOTS];
  const int n = readWifiSlots(v);
  o = "{\"n\":";
  o += n;
  o += ",\"cur\":";
  o += (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  // 1.46.51：把容量一起带上 —— App 要按「总共几格」画空位，
  //   不能靠数 list 里有几个（数不出空槽）。老 App 忽略这个字段，安全。
  o += ",\"max\":";
  o += WIFI_MAX_SLOTS;
  o += ",\"list\":[";
  for (int i = 0; i < n; i++) {
    if (i) o += ',';
    o += "{\"i\":";
    o += i;
    o += ',';
    kv(o, "ssid", v[i].ssid.c_str());
    o += ",\"on\":";
    o += (WiFi.status() == WL_CONNECTED && WiFi.SSID() == v[i].ssid) ? 1 : 0;
    o += '}';
  }
  o += "]}";
}

static void hWifiList() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  apiWifiListJson(o);
  server.send(200, "application/json; charset=utf-8", o);
}

// 忘掉某一组
// 1.46.36：① 返回真实结果（1/0/-1），以前无条件 true；
//          ② 删的是「当前正在用」那组时**不再当场断开** ——
//             断了之后 okJson() 发不出去，App 收到的又是 fetch 失败，
//             用户就会问「到底删没删」。改成排到 netLoop 里 700ms 后执行。
int apiWifiForget(const char* ssidIn) {
  String ssid = ssidIn ? ssidIn : "";
  if (!ssid.length()) return 0;
  const int r = forgetWifiSlot(ssid);
  if (r != 1) return r;                       // 没删成，什么都别动
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    s_wifiDropAt = millis() + 700;            // 回包先出去
    if (s_wifiDropAt == 0) s_wifiDropAt = 1;
    Serial.printf("[wifi] %s 是当前在用的那组，700ms 后断开\n", ssid.c_str());
  }
  return 1;
}

static void hWifiForget() {
  if (!authed()) { denied(); return; }
  cors();
  String ssid = argStr("ssid");
  if (!ssid.length()) { errJson(400, "ssid"); return; }
  const int r = apiWifiForget(ssid.c_str());
  if (r == 1) {
    // 把「删完还剩几组」一起带回去 —— App 直接显示，不用再猜
    WifiCred v[WIFI_MAX_SLOTS];
    String o = "{\"ok\":1,\"left\":";
    o += readWifiSlots(v);
    o += "}";
    okJson(o.c_str());
  } else if (r == 0) {
    errJson(404, "notfound");
  } else {
    errJson(500, "write");
  }
}

// 1.46.51：只存不连
static void hWifiAdd() {
  if (!authed()) { denied(); return; }
  cors();
  String ssid = argStr("ssid"), pass = argStr("pass");
  ssid.trim();
  if (!ssid.length()) { errJson(400, "ssid"); return; }
  const int r = apiWifiAdd(ssid.c_str(), pass.c_str());
  if (r == 1) {
    WifiCred v[WIFI_MAX_SLOTS];
    String o = "{\"ok\":1,\"n\":";
    o += readWifiSlots(v);
    o += ",\"max\":";
    o += WIFI_MAX_SLOTS;
    o += "}";
    okJson(o.c_str());
  } else if (r == -2) {
    errJson(409, "full");          // App 提示「先忘掉一组」
  } else if (r == -1) {
    errJson(500, "write");
  } else {
    errJson(400, "ssid");
  }
}

// 1.46.51：用已存的那组连
static void hWifiUse() {
  if (!authed()) { denied(); return; }
  cors();
  String ssid = argStr("ssid");
  ssid.trim();
  if (!ssid.length()) { errJson(400, "ssid"); return; }
  if (apiWifiUse(ssid.c_str()) != 1) { errJson(404, "notfound"); return; }
  String o = "{\"ok\":1,\"state\":1,";
  kv(o, "ssid", ssid.c_str());
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

static void hMute() {
  // 1.43.0：静音功能整体删了（用户要求「去掉静音功能」）。
  //   接口留着只为不回老 App 的 404 —— 回 ok 但**什么都不做**，
  //   免得界面上按了一下、机器真的没声了，又找不到是谁把它静下来的。
  if (!authed()) { denied(); return; }
  cors();
  okJson("{\"ok\":1,\"mute\":0,\"note\":\"mute removed in 1.43.0\"}");
}

static void hPause() {
  if (!authed()) { denied(); return; }
  cors();
  audioPauseToggle();
  actTouch();
  okJson("{\"ok\":1}");
}

// ------------------------------------------------------------
//  电台增删改
// ------------------------------------------------------------
static bool validUrl(const String& u) {
  // 1.46.33：判断挪到 store.cpp 的 storeUrlOk —— 蜻蜓节目页地址
  //   （https://www.qtfm.cn/channels/<ch>/programs/<pg>）也是合法地址，
  //   起播时由 storeUrl() 现算签名换成真流。
  if (!u.length() || u.length() > 190) return false;
  return storeUrlOk(u.c_str());
}

static void hAdd() {
  if (!authed()) { denied(); return; }
  cors();
  String n = argStr("name"), u = argStr("url");
  n.trim(); u.trim();
  if (!validUrl(u)) { errJson(400, "url"); return; }
  if (!n.length()) n = "未命名";

  // 1.15.0：g 不再是「组号」而是「标签下标」。不传就是第 0 个标签（广播）。
  // 以前要「先加到当前组、再 mvgroup 挪过去」（两次整表重写），现在一次写到位。
  int g = server.hasArg("g") ? argStr("g", "0").toInt() : 0;
  if (g < 0 || g >= storeTagCount()) g = 0;

  int id = storeAdd(n.c_str(), u.c_str(), g);
  if (id < 0) { errJson(400, "full"); return; }
  String o = "{\"id\":";
  o += id;
  o += ",\"count\":";
  o += g_stN;
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

static void hEdit() {
  if (!authed()) { denied(); return; }
  cors();
  int id = argStr("id", "-1").toInt();
  String n = argStr("name"), u = argStr("url");
  n.trim(); u.trim();
  if (u.length() && !validUrl(u)) { errJson(400, "url"); return; }
  bool ok = storeEdit(id, n.length() ? n.c_str() : nullptr,
                          u.length() ? u.c_str() : nullptr);
  okJson(ok ? "{\"ok\":1}" : "{\"err\":\"id\"}");
}

static void hDel() {
  if (!authed()) { denied(); return; }
  cors();
  int id = argStr("id", "-1").toInt();
  if (id < 0 || id >= g_stN) { errJson(400, "id"); return; }
  const bool wasCur = (id == A.cur);
  bool ok = storeDel(id);
  if (ok) {
    if (g_stN == 0) audioStop();
    else if (wasCur) audioPlayIndex(A.cur);      // 删的是在播的台，接着放下一台
  }
  A.dirty = true;
  okJson(ok ? "{\"ok\":1}" : "{\"err\":\"id\"}");
}

// dir=+1/-1 按位挪；to=N 拖到第 N 位（App 里的拖拽排序）
static void hMove() {
  if (!authed()) { denied(); return; }
  cors();
  int id = argStr("id", "-1").toInt();
  bool ok;
  if (server.hasArg("to")) {
    ok = storeMoveTo(id, argStr("to", "0").toInt());
  } else {
    ok = storeMove(id, argStr("dir", "1").toInt() >= 0 ? 1 : -1);
  }
  A.dirty = true;
  okJson(ok ? "{\"ok\":1}" : "{\"err\":\"id\"}");
}

// 换分类：g=目标组号；不传 g 但传 gn=新分类名，则新建
static void hMvGroup() {
  if (!authed()) { denied(); return; }
  cors();
  int id = argStr("id", "-1").toInt();
  String gn = argStr("gn");
  gn.trim();
  int g = server.hasArg("g") ? argStr("g", "-1").toInt() : -1;
  bool ok = storeMoveToGroup(id, g, gn.length() ? gn.c_str() : nullptr);
  A.dirty = true;
  okJson(ok ? "{\"ok\":1}" : "{\"err\":\"mv\"}");
}

static void hGroupOp() {
  if (!authed()) { denied(); return; }
  cors();
  String op = argStr("op");
  String nm = argStr("n");
  nm.trim();
  int g = argStr("g", "-1").toInt();
  bool ok = false;
  if      (op == "add")    ok = storeGroupAdd(nm.c_str());
  else if (op == "rename") ok = storeGroupRename(g, nm.c_str());
  else if (op == "del")    ok = storeGroupDel(g);
  else if (op == "move")   ok = storeGroupMove(g, argStr("dir", "1").toInt() >= 0 ? 1 : -1);
  A.dirty = true;
  okJson(ok ? "{\"ok\":1}" : "{\"err\":\"group\"}");
}

// ------------------------------------------------------------
//  收听记录
// ------------------------------------------------------------
static void hHistory() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  apiHistoryJson(o);          // 1.46.33：与跨网共用同一份
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  体检
// ------------------------------------------------------------
// ------------------------------------------------------------
//  电量（1.19.0）
//    GET /api/batt   {"bok":1,"batt":82,"bmv":3940,"chg":0,"blow":0,"bpin":4}
//    bok=0 表示这台没接电量检测（或读数不可信），App 就别显示电量那一行
// ------------------------------------------------------------
static void hBatt() {
  if (!authed()) { denied(); return; }
  cors();
  String o = "{";
  batteryJson(o);
  o += ",";
  powerJson(o);                // 1.21.0：温度 / 降载档位 / 是否停充
  o += ",";
  voiceJson(o);
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

static void hDiag() {
  if (!authed()) { denied(); return; }
  cors();
  int total = g_stN;              // 1.15.0：整表就是一个列表，不用再按组求和

  struct tm t;
  const bool timeOk = getLocalTime(&t, 0);

  String o = "{";
  kv(o, "fw", FW_VER);
  o += ",\"up\":"; o += (millis() - s_bootAt) / 1000;
  o += ',';
  kv(o, "reset", resetText(esp_reset_reason()));
  o += ",\"heap\":"; o += ESP.getFreeHeap();
  o += ",\"heapMin\":"; o += ESP.getMinFreeHeap();
  // 1.46.10：ESP.getPsramSize() 只看 board json 的声明，这块板声明的是"无 PSRAM"
  //   ⇒ 恒为 0，什么都反映不了。真正该看的是 heap_caps 拿得到的空闲，
  //   以及音频库最终拿了多大缓冲（abuf）。
  o += ",\"psramReal\":"; o += (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  o += ",\"abuf\":"; o += audioBufBytes();
  o += ",\"abufPs\":"; o += audioBufPsram();
  o += ",\"psram\":"; o += (long)ESP.getPsramSize();
  o += ",\"psramFree\":"; o += (long)ESP.getFreePsram();
  o += ",\"fsTotal\":"; o += (long)LittleFS.totalBytes();
  o += ",\"fsUsed\":"; o += (long)LittleFS.usedBytes();

  o += ",\"wifi\":{";
  o += "\"ok\":"; o += (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  o += ',';
  kv(o, "ssid", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
  o += ",\"rssi\":"; o += (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  o += ",\"ch\":"; o += WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0;
  o += ',';
  kv(o, "ip", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : s_ip);
  o += ',';
  kv(o, "gw", WiFi.status() == WL_CONNECTED ? WiFi.gatewayIP().toString().c_str() : "");
  o += ',';
  kv(o, "dns", WiFi.status() == WL_CONNECTED ? WiFi.dnsIP().toString().c_str() : "");
  o += ",\"ap\":"; o += s_apMode ? 1 : 0;
  o += "}";

  o += ",\"audio\":{\"ps\":"; o += (int)A.ps;
  o += ",\"retry\":"; o += audioRetry();
  o += ",\"dead\":"; o += audioDeadLinks();
  o += ",\"kbps\":"; o += audioBitrate();
  o += ",\"sec\":"; o += (long)audioUptimeSec();
  // 1.46.0：排查「某个台没声音」全靠这四个 ——
  //   agc   当前自动增益（1000 = 1.0 倍；>1000 在给轻的台加油）
  //   noise 1 = 连不上，正在放本地沙沙声
  //   dec   解出的 PCM 块数（一直不动 = 这个流根本没解出东西）
  //   sil   连续静音块数（在涨 = 解出来了但样本全是 0）
  o += ",\"agc\":"; o += (long)audioAgcMilli();
  // 1.46.30：AGC 的输入侧两个数 —— arms 小、agc 已经顶到上限 = 抬不动了（这就是"书比台小"）
  o += ",\"arms\":"; o += (long)audioAgcRms();
  o += ",\"apk\":"; o += (long)audioAgcPeak();
  o += ",\"noise\":"; o += audioNoiseActive() ? 1 : 0;
  o += ",\"dec\":"; o += audioDecodeBlocks();
  o += ",\"sil\":"; o += audioSilentBlocks();
  // 1.46.2：有声书看得见的两个数 —— ep 第几集、pos 这一集播到第几秒
  o += ",\"ep\":"; o += audioEpNo();
  o += ",\"pos\":"; o += audioPosSec();
  o += ",\"rs\":"; o += audioResumed();
  o += ',';
  kv(o, "url", audioUrl());
  o += "}";

  o += ",\"time\":{\"ok\":"; o += timeOk ? 1 : 0;
  o += ',';
  char tbuf[28] = "未同步";
  if (timeOk) snprintf(tbuf, sizeof(tbuf), "%02d:%02d %d月%d日",
                       t.tm_hour, t.tm_min, t.tm_mon + 1, t.tm_mday);
  kv(o, "now", tbuf);
  o += "}";

  o += ",\"table\":{\"tags\":"; o += storeTagCount();
  o += ",\"count\":"; o += g_stN;
  o += ",\"total\":"; o += total;
  o += "}";

  o += ",\"token\":{\"set\":"; o += s_tok[0] ? 1 : 0;
  o += ',';
  char fp[5] = {0};
  if (s_tok[0]) { fp[0] = s_tok[0]; fp[1] = s_tok[1]; fp[2] = s_tok[2]; fp[3] = s_tok[3]; }
  kv(o, "fp", fp);
  o += "}";

  o += ",\"pair\":{\"locked\":"; o += s_locked ? 1 : 0;
  o += "}";

  // 1.19.0：电量（含引脚号，接错引脚时靠这个对）与语音播报
  o += ",\"batt\":{";
  batteryJson(o);
  o += "}";
  o += ",\"voice\":{";
  voiceJson(o);
  o += "}";
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  台的健康（1.17.0）
//  GET  /api/health        整表摘要：每台一个状态字符 + 计数 + 正在试的这台
//  GET  /api/health?i=37   单台详情（App 的问题详情弹层用）
// ------------------------------------------------------------
static void hHealth() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  apiHealthJson(o, server.hasArg("i") ? server.arg("i").toInt() : -1);
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  1.46.6：停播台账
//    GET  /api/offair              现在被藏着哪些台（含还剩几分钟）
//    POST /api/offair?clear=1      立即全部放回（"我想再试试那个台"）
//    POST /api/offair?on=0|1       总开关（0 = 一台都不跳）
//
//  为什么留 clear：检测总有误判（路由器抖一下、源临时 502），
//  必须给用户一条**立刻推翻机器判断**的路，不然他会以为台被删了。
// ------------------------------------------------------------
static void hOffair() {
  if (!authed()) { denied(); return; }
  cors();
  if (server.method() == HTTP_POST) {
    bool ch = false;
    if (server.hasArg("clear")) { offairClearAll(); ch = true; }
    if (server.hasArg("on"))    { offairSetEnabled(server.arg("on").toInt() != 0); ch = true; }
    if (ch) { server.send(200, "application/json; charset=utf-8", "{\"ok\":1}"); return; }
  }
  String o;
  offairJson(o);
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  修复
// ------------------------------------------------------------
// ---- 1.22.0：WiFi 存 3 组（原厂是 ssid / ssid2 / ssid3）----
//   WIFI_MAX_SLOTS / struct WifiCred 已在文件前部定义（HTTP handler 要用）

// 读全部已存网络（最多 3 组，第一组 = 上次成功用的那个）
// 只去行尾的 \r（\n 已被 readStringUntil 吃掉）。
// !! 不能用 String::trim() —— 它连尾部的 '\t' 也一起删，
//    "x\t"（空密码 = 开放网络）会被 trim 成 "x"，于是找不到分隔符、整行被丢掉。
//   症状：开放 WiFi 存不住 / 存了 3 组但有一组永远轮不到。这是 1.22.0 之前就有的洞。
static void stripEol(String& s) {
  while (s.length() && (s.endsWith("\r") || s.endsWith("\n"))) {
    s.remove(s.length() - 1);
  }
}

static int readWifiSlots(WifiCred out[WIFI_MAX_SLOTS]) {
  int n = 0;
  if (!LittleFS.exists("/wifi.txt")) return 0;
  File f = LittleFS.open("/wifi.txt", "r");
  if (!f) return 0;
  while (f.available() && n < WIFI_MAX_SLOTS) {
    String ln = f.readStringUntil('\n');
    stripEol(ln);
    if (!ln.length()) continue;
    int tab = ln.indexOf('\t');
    if (tab <= 0) continue;
    out[n].ssid = ln.substring(0, tab);
    out[n].pass = ln.substring(tab + 1);     // 可能为空（开放网络）
    if (out[n].ssid.length()) n++;
  }
  f.close();
  return n;
}

// 写回：把 ssid 放到第一行；其余保持原顺序；超过 3 组的丢掉最老的
// 1.46.36：返回是否写成功 —— 以前是 void，开文件失败就静默丢掉，
//   上层照样回「已删除」，用户问「到底删没删」时谁也答不上来。
static bool writeWifiSlots(const WifiCred* v, int n) {
  File f = LittleFS.open("/wifi.txt", "w");
  if (!f) {
    Serial.println("[wifi] 写 /wifi.txt 失败（打不开）");
    return false;
  }
  size_t wrote = 0;
  for (int i = 0; i < n && i < WIFI_MAX_SLOTS; i++) {
    if (!v[i].ssid.length()) continue;
    wrote += f.print(v[i].ssid);
    wrote += f.print('\t');
    wrote += f.print(v[i].pass);
    wrote += f.print('\n');
  }
  f.close();
  if (n > 0 && wrote == 0) {
    Serial.println("[wifi] 写 /wifi.txt 失败（0 字节）");
    return false;
  }
  return true;
}

// 保存（或更新）一组网络，并置顶
static void saveWifiFile(const String& ssid, const String& pass) {
  WifiCred cur[WIFI_MAX_SLOTS];
  const int had = readWifiSlots(cur);

  WifiCred out[WIFI_MAX_SLOTS];
  int n = 0;
  out[n].ssid = ssid;
  out[n].pass = pass;
  n++;
  for (int i = 0; i < had && n < WIFI_MAX_SLOTS; i++) {
    if (cur[i].ssid == ssid) continue;      // 同一组不重复存（密码取这次的）
    out[n++] = cur[i];
  }
  writeWifiSlots(out, n);

  // 顺手清掉"开机时该试第几组"的记忆，下次上电从第一组重新来
  if (LittleFS.exists("/wifi_idx.txt")) LittleFS.remove("/wifi_idx.txt");
}

// 删掉某一组（App 里「忘记这个网络」）
// 1.46.36：三态 + 回读校验。用户明确问过「不知道收音机里有没有真的删掉」，
//   所以这里删完必须**再从文件读一遍**确认它真的不在了，才敢说成功。
static int forgetWifiSlot(const String& ssid) {
  WifiCred cur[WIFI_MAX_SLOTS];
  const int had = readWifiSlots(cur);
  if (had <= 0) return 0;                      // 一个都没存
  WifiCred out[WIFI_MAX_SLOTS];
  int n = 0;
  bool found = false;
  for (int i = 0; i < had; i++) {
    if (cur[i].ssid == ssid) { found = true; continue; }
    out[n++] = cur[i];
  }
  if (!found) return 0;                        // 本来就没有这一组
  if (!writeWifiSlots(out, n)) return -1;      // 写盘失败
  // 回读校验：真的没了才算删掉
  WifiCred chk[WIFI_MAX_SLOTS];
  const int m = readWifiSlots(chk);
  for (int i = 0; i < m; i++) {
    if (chk[i].ssid == ssid) {
      Serial.println("[wifi] 回读发现还在，判定删除失败");
      return -1;
    }
  }
  Serial.printf("[wifi] 已忘掉 %s，剩 %d 组\n", ssid.c_str(), m);
  return 1;
}

// ------------------------------------------------------------
//  1.45.0：原厂风格的配网页
//
//  为什么改：1.44.x 之前，手机连上热点打开 192.168.4.1 拿到的是整个网页版
//  （180KB，还带配对和轮询）。配网真正要做的只有一件事：填 WiFi 名字和密码。
//  现在从热点进来的请求直接给 /setup 这一页，版式和字段名都照原厂来。
//
//  字段名沿用原厂：ssid/pass、ssid2/pass2、ssid3/pass3。
//  密码留空 = 不改这一组原来的密码（所以页面上不需要把密码回显出来）。
// ------------------------------------------------------------
static bool     s_provSaved = false;      // 配网页保存过，等连上后关热点
static uint32_t s_apOffDeadline = 0;

// ------------------------------------------------------------
//  1.46.51：只存不连 / 用已存的那组连
//
//  为什么加「只存不连」：App 现在是「4 个格子，空着的填上就存」。
//  用户填第 3 组往往是"备用网络"（公司/父母家），不该立刻把当前连接切走 ——
//  一换就可能正在听的台断掉，还得再切回来。所以保存和切换彻底分开：
//    · /api/wifi/add  ：只写 /wifi.txt，连不连下次开机才说
//    · /api/wifi/use  ：拿文件里的密码去连（用户点「连这个」）
//  顺序也不动：add 一律追加在末尾（越靠后越不优先），
//  use 不置顶（开机顺序就是用户排的那个顺序，别偷偷改）。
// ------------------------------------------------------------

// 返回：1 存好 / 0 参数不对 / -1 写盘失败 / -2 四组都满了
int apiWifiAdd(const char* ssidIn, const char* passIn) {
  String ssid = ssidIn ? ssidIn : "";
  String pass = passIn ? passIn : "";
  ssid.trim();
  if (!ssid.length() || ssid.length() > 32) return 0;

  WifiCred cur[WIFI_MAX_SLOTS];
  const int had = readWifiSlots(cur);

  WifiCred out[WIFI_MAX_SLOTS];
  int n = 0;
  bool found = false;
  for (int i = 0; i < had; i++) {
    if (cur[i].ssid == ssid) {
      // 同名：原地更新密码（位置不变 —— 用户排的顺序不要动）
      out[n].ssid = ssid;
      out[n].pass = pass;
      n++;
      found = true;
    } else {
      out[n++] = cur[i];
    }
  }
  if (!found) {
    if (had >= WIFI_MAX_SLOTS) return -2;     // 满了，别静默顶掉别人
    out[n].ssid = ssid;
    out[n].pass = pass;
    n++;
  }
  if (!writeWifiSlots(out, n)) return -1;

  // 回读校验（和 forget 一个口径：写盘说成功不算，读回来才算）
  WifiCred chk[WIFI_MAX_SLOTS];
  const int m = readWifiSlots(chk);
  bool ok = false;
  for (int i = 0; i < m; i++) if (chk[i].ssid == ssid) ok = true;
  Serial.printf("[wifi] 存了一组 %s（现共 %d 组）%s\n", ssid.c_str(), m, ok ? "" : " !! 回读没找到");
  return ok ? 1 : -1;
}

// 用 /wifi.txt 里已有的密码去连。返回 1 找到并发起 / 0 没有这一组
int apiWifiUse(const char* ssidIn) {
  String ssid = ssidIn ? ssidIn : "";
  if (!ssid.length()) return 0;
  WifiCred v[WIFI_MAX_SLOTS];
  const int n = readWifiSlots(v);
  for (int i = 0; i < n; i++) {
    if (v[i].ssid == ssid) {
      // 1.46.53：原来先置 true 再调 apiWifiSet()，而那个函数里有一句
      //   `s_pendKeepOrder = false`（"默认置顶"）—— 把刚设的 true 又清回去了，
      //   于是「连这个」仍然会把那组挪到第一组。改成调完再设。
      apiWifiSet(ssid.c_str(), v[i].pass.c_str());
      s_pendKeepOrder = true;                 // 必须在 apiWifiSet 之后
      return 1;
    }
  }
  return 0;
}

// 已经存过的名字（不返回密码）
static void hProvSlots() {
  cors();
  WifiCred v[WIFI_MAX_SLOTS];
  const int n = readWifiSlots(v);
  String o = "{\"cur\":";
  o += (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  o += ",\"list\":[";
  for (int i = 0; i < n; i++) {
    if (i) o += ',';
    o += '{';
    kv(o, "s", v[i].ssid.c_str());
    o += ",\"on\":";
    o += (WiFi.status() == WL_CONNECTED && WiFi.SSID() == v[i].ssid) ? 1 : 0;
    o += '}';
  }
  o += "]}";
  server.send(200, "application/json; charset=utf-8", o);
}

// 手机的联网探测（Android/iOS/Windows 各有一套地址）撞到我们身上。
// 一律 302 到配网页 —— 系统看到"这个网络要登录"就自己弹浏览器。
static void hCaptive() {
  cors();
  String loc = "http://";
  loc += s_apMode ? WiFi.softAPIP().toString() : String(s_ip);
  loc += "/setup";
  server.sendHeader("Location", loc, true);
  server.send(302, "text/html; charset=utf-8",
              "<!doctype html><meta charset=\"utf-8\"><a href=\"/setup\">点这里配网</a>");
}

static void hProvPage() {
  cors();
  server.send_P(200, "text/html; charset=utf-8", PROV_HTML);
}

static void hProvSave() {
  cors();
  // 1.46.51：3 组 -> 4 组（第 4 组字段名 ssid4/pass4，原厂页面没有，App 里有）
  static const char* K[8] = { "ssid", "pass", "ssid2", "pass2", "ssid3", "pass3", "ssid4", "pass4" };

  WifiCred oldv[WIFI_MAX_SLOTS];
  const int oldN = readWifiSlots(oldv);

  WifiCred out[WIFI_MAX_SLOTS];
  int n = 0;
  for (int i = 0; i < WIFI_MAX_SLOTS; i++) {
    String s = argStr(K[i * 2]);
    s.trim();
    String p = argStr(K[i * 2 + 1]);          // 密码不 trim：前后空格可能是密码的一部分
    if (!s.length()) continue;                // 空着的那组 = 不要
    if (!p.length()) {                        // 留空 = 沿用这一组原来的密码
      for (int j = 0; j < oldN; j++)
        if (oldv[j].ssid == s) { p = oldv[j].pass; break; }
    }
    bool dup = false;
    for (int j = 0; j < n; j++) if (out[j].ssid == s) dup = true;
    if (dup) continue;
    out[n].ssid = s;
    out[n].pass = p;
    n++;
  }

  if (n == 0) {                               // 三组全空：没什么可存的，回页面
    server.sendHeader("Location", "/setup");
    server.send(303, "text/plain", "");
    return;
  }

  writeWifiSlots(out, n);
  s_slotN  = n;
  s_haveCred = true;
  s_slotIdx = 0;

  // 失败要退回原来那个：先把当前连着的 SSID/密码记下来
  if (WiFi.status() == WL_CONNECTED) {
    s_oldSsid = WiFi.SSID();
    for (int j = 0; j < oldN; j++)
      if (oldv[j].ssid == s_oldSsid) { s_oldPass = oldv[j].pass; break; }
  }

  s_pendSsid = out[0].ssid;
  s_pendPass = out[0].pass;
  s_slotTryAt = millis();

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == s_pendSsid) {
    s_pend = false;                           // 现在就是它，不用折腾
    s_wifiState = 2;
    snprintf(s_wifiMsg, sizeof(s_wifiMsg), "已连上 %s", s_pendSsid.c_str());
  } else {
    s_pend = true;
    s_tryAt = millis();
    s_wifiState = 1;
    snprintf(s_wifiMsg, sizeof(s_wifiMsg), "正在连接 %s …", s_pendSsid.c_str());
    WiFi.disconnect(false, false);             // 先断，免得上一组的凭据还占着
    WiFi.begin(s_pendSsid.c_str(), s_pendPass.c_str());
  }

  s_provSaved = true;
  Serial.printf("[prov] 配网页存了 %d 组，试连 %s\n", n, s_pendSsid.c_str());

  server.send_P(200, "text/html; charset=utf-8", PROV_OK_HTML);
}

// 1.46.35：延迟断链重连（先回包，再断）
static uint32_t s_reconnAt = 0;        // 到点就断。0 = 没排
static String   s_reconnSsid, s_reconnPass;

void netReconnectSoon() {
  // 要连哪一组：取 /wifi.txt 里第一组（就是开机先试的那组）
  s_reconnSsid = "";
  s_reconnPass = "";
  if (LittleFS.exists("/wifi.txt")) {
    File f = LittleFS.open("/wifi.txt", "r");
    if (f) {
      String ln = f.readStringUntil('\n');
      f.close();
      // 1.22.0 的教训：别用 trim()，开放网络（空密码）会被吃掉
      while (ln.length() && (ln.endsWith("\r") || ln.endsWith("\n")))
        ln.remove(ln.length() - 1);
      int tab = ln.indexOf('\t');
      if (tab > 0) {
        s_reconnSsid = ln.substring(0, tab);
        s_reconnPass = ln.substring(tab + 1);
      }
    }
  }
  s_reconnAt = millis() + 700;   // 留 700ms 让回包出网卡
  if (s_reconnAt == 0) s_reconnAt = 1;
}

// 1.46.39：收音诊断 —— 主动读一次芯片，看「到底为什么不响」
static void hFmProbe() {
  if (!authed()) { denied(); return; }
  cors();
  int khz = 0, rssi = -1, snr = -1, mute = -1, vol = -1, ready = 0;
  fmProbe(&khz, &rssi, &snr, &mute, &vol, &ready);
  String o = "{\"ok\":1,\"ready\":";
  o += ready;
  o += ",\"mode\":\""; o += A.fmMode ? "fm" : "net"; o += '"';
  o += ",\"khz\":";  o += khz;
  o += ",\"band\":\""; o += radioBandCode(khz ? khz : A.fmFreq); o += '"';
  o += ",\"rssi\":"; o += rssi;
  o += ",\"snr\":";  o += snr;
  o += ",\"mute\":"; o += mute;      // 3 = 双声道硬静音；-1 = 读不到
  o += ",\"vol\":";  o += vol;       // 芯片里的音量寄存器（0..63）；-1 = 读不到
  o += ",\"squelchAuto\":"; o += fmSquelched() ? 1 : 0;
  o += ",\"squelchUser\":"; o += A.squelch;
  o += ",\"scanN\":";  o += fmScanCount();
  o += ",\"scanBusy\":"; o += fmScanBusy() ? 1 : 0;
  o += ",\"fmVol\":";  o += A.fmVol;
  o += "}";
  okJson(o.c_str());
}

// 1.46.43：诊断 —— 临时进 FM 模式（有网也能进），到点自动回来。
//   不传 sec 默认 10 秒，最多 30 秒。返回窗口长度，方便调用方等。
static void hFmTest() {
  if (!authed()) { denied(); return; }
  cors();
  int sec = argStr("sec", "10").toInt();
  if (sec < 1) sec = 1;
  if (sec > 30) sec = 30;
  fmTestBegin((uint32_t)sec * 1000u);
  String o = "{\"ok\":1,\"sec\":";
  o += sec;
  o += ",\"note\":\"已临时进收音，到点自动回网络\"}";
  okJson(o.c_str());
}

// 1.46.39：读事件日志（断网-恢复这种问题，事后全靠它）
static void hLog() {
  if (!authed()) { denied(); return; }
  cors();
  String o;
  evlogJson(o);
  server.send(200, "application/json; charset=utf-8", o);
}

static void hFix() {
  if (!authed()) { denied(); return; }
  cors();
  String act = argStr("action");
  const bool danger = (act == "factory" || act == "reset_stations" || act == "forget_wifi");
  if (danger && argStr("confirm") != "1") { errJson(400, "confirm"); return; }

  String msg = "已处理";
  bool reboot = false;

  if (act == "replay") {
    audioReplay();
    msg = "正在重开这个台";
  } else if (act == "reconnect") {
    msg = "正在重新连 WiFi，页面十几秒后自己回来";
    // 1.46.35：这里原来是「当场 WiFi.disconnect()」—— 链路一断，函数末尾那句
    //   server.send() 根本发不出去，App 收到的是 fetch 失败，于是弹「没成功」。
    //   机器其实连上了，只是回话没送出来。用户报的「点击重连 WiFi 没成功」就是这个。
    //   现在只排个队，回包发完之后由 netLoop 真正断开重连。
    netReconnectSoon();
  } else if (act == "reset_audio") {
    audioStop();
    delay(150);
    audioReplay();
    msg = "音频已重启";
  } else if (act == "fix_table") {
    storeLoad();          // 整表重读（顺手清掉文件里的空行）
    // 一台都读不出来时千万别 storeSave —— 那会把整份台表写成空文件
    if (g_stN > 0) {
      storeSave();
      msg = "台表已整理";
    } else {
      msg = "台表读不出来，得恢复出厂台表";
    }
  } else if (act == "clear_history") {
    historyClear();
    msg = "收听记录已清空";
  } else if (act == "reset_stations") {
    LittleFS.remove("/stations.tsv");
    LittleFS.remove("/tags.txt");          // 1.15.0：标签表也一起删，重启后重灌
    msg = "已恢复出厂台表，正在重启";
    reboot = true;
  } else if (act == "forget_wifi") {
    LittleFS.remove("/wifi.txt");
    msg = "已忘掉 WiFi，正在重启";
    reboot = true;
  } else if (act == "reboot") {
    msg = "正在重启";
    reboot = true;
  } else if (act == "factory") {
    LittleFS.remove("/stations.tsv");
    LittleFS.remove("/history.tsv");
    LittleFS.remove("/cfg.txt");
    LittleFS.remove("/token.txt");
    LittleFS.remove(F_PAIR);
    msg = "已恢复出厂设置，正在重启";
    reboot = true;
  } else {
    errJson(400, "action");
    return;
  }

  // 恢复出厂时 cfg 刚被删掉，别再写回去
  if (act != "factory") storeSaveCfg();
  historySaveNow();

  String o = "{\"ok\":1,\"reboot\":";
  o += reboot ? 1 : 0;
  o += ',';
  kv(o, "msg", msg.c_str());
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);

  if (reboot) {
    // 1.46.33：400ms 太紧 —— App 那边常常拿不到回包，于是点了「重启」却弹「没成功」。
    //   给 TCP 把这几百字节真正推出去的时间。
    delay(1500);
    ESP.restart();
  }
}

// ------------------------------------------------------------
//  配对管理
// ------------------------------------------------------------
static void hPair() {
  if (!authed()) { denied(); return; }
  cors();
  if (server.method() == HTTP_GET) {
    String o = "{\"locked\":";
    o += s_locked ? 1 : 0;
    o += ",\"fp\":\"";
    if (s_tok[0]) { o += s_tok[0]; o += s_tok[1]; o += s_tok[2]; o += s_tok[3]; }
    o += "\",\"ip\":\"";
    o += s_ip;
    o += "\",\"up\":";
    o += (millis() - s_bootAt) / 1000;
    o += ",\"dev\":\"" AP_SSID "\"";
    o += ",\"pin\":\"";
    o += pinCode();                       // 1.46.5：网页设置里要显示它
    o += '"';
    if (s_locked) { o += ",\"code\":\""; o += s_code; o += '"'; }
    o += "}";
    server.send(200, "application/json; charset=utf-8", o);
    return;
  }

  String op = argStr("op");
  if (op == "lock") {
    bool on = argStr("on", "0").toInt() != 0;
    if (on && strlen(s_code) != 6) newCode();
    s_locked = on;
    savePair();
    if (on) uiPairCode(s_code);          // 顺手显示在屏上，用户能抄下来
    String o = "{\"ok\":1,\"locked\":";
    o += s_locked ? 1 : 0;
    if (s_locked) { o += ",\"code\":\""; o += s_code; o += '"'; }
    o += "}";
    server.send(200, "application/json; charset=utf-8", o);
  } else if (op == "newcode") {
    newCode();
    savePair();
    uiPairCode(s_code);
    String o = "{\"ok\":1,\"code\":\"";
    o += s_code;
    o += "\"}";
    server.send(200, "application/json; charset=utf-8", o);
  } else if (op == "unbind") {
    // 换一台手机用：旧令牌作废，新令牌当场发给当前这台
    char t[17];
    for (int i = 0; i < 16; i++) t[i] = "0123456789abcdef"[rnd() & 0x0F];
    t[16] = 0;
    storeSetToken(t);
    strncpy(s_tok, t, sizeof(s_tok) - 1);
    String o = "{\"ok\":1,\"token\":\"";
    o += s_tok;
    o += "\"}";
    server.send(200, "application/json; charset=utf-8", o);
  } else if (op == "pin") {                 // 1.46.5：改 4 位手机连接码
    String c = argStr("code");
    c.trim();
    if (!pinSetCode(c.c_str())) { errJson(400, "code"); return; }
    uiPairCode(pinCode());                   // 改完在屏上显示一次，省得忘
    String o = "{\"ok\":1,\"pin\":\"";
    o += pinCode();
    o += "\"}";
    server.send(200, "application/json; charset=utf-8", o);
  } else if (op == "forget") {
    LittleFS.remove(F_PAIR);
    s_locked = false;
    s_code[0] = 0;
    uiPairCodeClear();
    okJson("{\"ok\":1,\"locked\":0}");
  } else {
    errJson(400, "op");
  }
}

// ------------------------------------------------------------
//  WiFi 配网
// ------------------------------------------------------------
// 1.46.33：配网动作抽出来（跨网「给收音机换 WiFi」要走同一套状态机，
//   连不上才会自动退回原来那组 —— 各写一份迟早对不上）
void apiWifiSet(const char* ssidIn, const char* passIn) {
  String ssid = ssidIn ? ssidIn : "";
  String pass = passIn ? passIn : "";
  ssid.trim();
  if (!ssid.length()) return;
  s_pendSsid = ssid;
  s_pendPass = pass;
  s_pend     = true;
  s_pendKeepOrder = false;      // 1.46.51：默认置顶（apiWifiUse 会先把它设回 true）
  s_tryAt    = millis();
  s_wifiState = 1;
  snprintf(s_wifiMsg, sizeof(s_wifiMsg), "正在连接 %s …", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] 改用 %s\n", ssid.c_str());
}

static void hWifi() {
  cors();
  // 配网：只在「还没 WiFi」或已配对时允许
  if (s_tok[0] && !authed()) { denied(); return; }
  String ssid = argStr("ssid"), pass = argStr("pass");
  ssid.trim();
  if (!ssid.length()) { errJson(400, "ssid"); return; }

  apiWifiSet(ssid.c_str(), pass.c_str());

  String o = "{\"ok\":1,\"state\":1,";
  kv(o, "ssid", ssid.c_str());
  o += "}";
  server.send(200, "application/json; charset=utf-8", o);
}

void apiWifiStatusJson(String& o) {
  o = "{\"state\":";
  o += s_wifiState;
  o += ',';
  kv(o, "msg", s_wifiMsg);
  o += ',';
  kv(o, "ssid", (WiFi.status() == WL_CONNECTED) ? WiFi.SSID().c_str() : "");
  o += ',';
  kv(o, "ip", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : s_ip);
  o += ",\"rssi\":";
  o += (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  o += ",\"ok\":";
  o += (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  o += "}";
}

static void hWifiStatus() {
  cors();
  String o;
  apiWifiStatusJson(o);
  server.send(200, "application/json; charset=utf-8", o);
}

// 1.46.33：用户报「手机开着 WiFi，App 里一直扫不到网络」。
//   原来的写法有两个坑：
//     ① WiFi.scanNetworks() 默认 show_hidden=false —— 名字藏起来的 SSID 直接不出现；
//     ② 返回负值是失败（驱动正忙 / 刚切模式），以前直接当成「0 个网络」，
//        App 就显示「没扫到网络，离路由器近一点」，用户只能干瞪眼。
//   现在：带隐藏网络一起扫；负数重试一次；结果按信号强度从强到弱排。
void apiScanJson(String& o) {
  int n = -1;
  for (int t = 0; t < 2; t++) {
    n = WiFi.scanNetworks(false, true);       // async=false, show_hidden=true
    if (n >= 0) break;
    delay(160);                               // 驱动还没空出来，等一下再来
  }
  if (n < 0) {
    o = "[{\"s\":\"\",\"r\":0,\"e\":0,\"err\":1}]";   // App 会过滤掉空名字
    return;
  }
  // 按 RSSI 排序（简单的选择排序，最多 24 个，够用又不占栈）
  int idx[24], m = 0;
  for (int i = 0; i < n && m < 24; i++) {
    if (WiFi.SSID(i).length()) idx[m++] = i;
  }
  for (int a = 0; a < m; a++)
    for (int b = a + 1; b < m; b++)
      if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }

  o = "[";
  for (int k = 0; k < m; k++) {
    const int i = idx[k];
    String s = WiFi.SSID(i);
    if (k) o += ',';
    o += "{\"s\":\"";
    jsonEsc(o, s.c_str());
    o += "\",\"r\":";
    o += WiFi.RSSI(i);
    o += ",\"e\":";
    o += (int)WiFi.encryptionType(i);
    o += "}";
  }
  o += "]";
  WiFi.scanDelete();
}

static void hScan() {
  cors();
  String o;
  apiScanJson(o);
  server.send(200, "application/json; charset=utf-8", o);
}

// ------------------------------------------------------------
//  静态资源
// ------------------------------------------------------------
static void hRoot() {
  cors();
  // 1.45.0：从热点（192.168.4.x）进来，或者机器根本没存过 WiFi → 直接给配网页。
  //   以前这里一律给 180KB 的网页版：老人连上热点、打开 192.168.4.1，
  //   看到的是「屏幕镜像 / 电台 / 体检」一堆东西，反而找不到配网在哪儿。
  //   已经连上家里 WiFi、从局域网访问的，仍然给完整网页版。
  IPAddress rip = server.client().remoteIP();
  const bool fromAp = (rip[0] == 192 && rip[1] == 168 && rip[2] == 4);
  if (fromAp || !s_haveCred) {
    server.send_P(200, "text/html; charset=utf-8", PROV_HTML);
    return;
  }
  server.send_P(200, "text/html; charset=utf-8", WEBAPP_HTML);
}

static void hManifest() {
  cors();
  server.send_P(200, "application/manifest+json", R"MF({
 "name":"操控收音机","short_name":"操控收音机","start_url":"/","scope":"/",
 "display":"standalone","background_color":"#f7f3ec","theme_color":"#f7f3ec",
 "lang":"zh-CN",
 "icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]
})MF");
}

// 一台老式收音机：圆角机身 + 喇叭网 + 旋钮
static void hIcon() {
  cors();
  server.send_P(200, "image/svg+xml", R"ICON(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 192 192">
<rect width="192" height="192" rx="44" fill="#b06b03"/>
<rect x="34" y="62" width="124" height="76" rx="14" fill="#0b0b0b"/>
<rect x="46" y="74" width="56" height="30" rx="4" fill="#01839d"/>
<rect x="46" y="112" width="72" height="6" rx="3" fill="#cfcece"/>
<rect x="46" y="124" width="52" height="6" rx="3" fill="#6b6257"/>
<circle cx="132" cy="102" r="15" fill="#e9e9e9"/>
<circle cx="132" cy="102" r="6" fill="#b06b03"/>
<path d="M78 62 L112 36" stroke="#0b0b0b" stroke-width="7" stroke-linecap="round"/>
</svg>)ICON");
}

// ------------------------------------------------------------
//  WiFi 底层
// ------------------------------------------------------------
// 1.22.0：读第一组（上次成功用的那个）。
// !! 这里必须放在 readWifiSlots 之后定义，否则编译器看不到它。
static bool loadWifi(String& ssid, String& pass) {
  WifiCred v[WIFI_MAX_SLOTS];
  const int n = readWifiSlots(v);
  if (n <= 0) return false;
  ssid = v[0].ssid;
  pass = v[0].pass;
  return ssid.length() > 0;
}

bool netWifiOk() { return WiFi.status() == WL_CONNECTED; }
int  netRssi()   { return netWifiOk() ? WiFi.RSSI() : -100; }
const char* netIP() { return s_ip; }
const char* netToken() { return s_tok[0] ? s_tok : nullptr; }
bool netNeedSetup() { return !s_haveCred; }
// 1.34.3：本次上电是否连上过 WiFi。用来区分「开机就没网」和「用着用着掉线」。
bool netEverUp()    { return s_everUp; }

void netClockStr(char* buf, int len) {
  struct tm t;
  if (!getLocalTime(&t, 0)) { snprintf(buf, len, "--:--"); return; }
  snprintf(buf, len, "%02d:%02d", t.tm_hour, t.tm_min);
}

// 1.44.1：这里原来整段被 if (AP_ALWAYS_ON) 包着 —— 1.43.0 把 AP_ALWAYS_ON 改成 0
//   之后，它变成**空壳**：不开热点，却先把 s_apMode 置成 true，于是 netApActive()
//   对外报"热点开着"，长按旋钮再按一次还走 netApOff()。"开机自动开热点"那条路
//   就这样静默失败（实机烧完 1.44.0 配网连不上，根因之一）。
//   现在只做转调，热点到底怎么开只在 netApOn() 一处定义。
static void startAp() { netApOn(); }

// ---- 1.43.0：热点的运行时开关 ----
//
//  必须在 AP_STA 模式下开热点：只切 WIFI_AP 会把 STA 连接踢掉，机器就再也连不上
//  路由器了（没网时想连热点，结果连完热点发现机器掉线 —— 最难解释的那种 bug）。
bool netApActive() { return s_apMode; }

// ------------------------------------------------------------
//  1.46.53：连不上 WiFi 超过 60 秒 → 自己开热点，等手机来配网
//
//  为什么必须有这条：原来只有两处会开热点 ——
//    ① 上电时一组凭据都没有（新机器 / 删了最后一组）
//    ② App 里删掉最后一组
//  「凭据存着、但连不上」**没有出口**：机器永远卡在广播页。用户看到的就是
//  「一直广播，连上网也不切回网络广播」；而所谓「连上网」是手机连上了路由器，
//  机器这边 WiFi.status() 一直是断的。
//
//  为什么只在「本次上电没连上过」时开：连上过又掉线，多半是路由器重启，
//  几秒就回来；这时开热点反而把手机从家里 WiFi 拽走（那个热点没网）。
// ------------------------------------------------------------
#define NET_AUTOSETUP_MS  (60UL * 1000)     // 连不上的忍耐时间
#define NET_HELP_SHOW_MS  (15UL * 1000)     // 屏上提示显示多久

static uint32_t s_noNetAt    = 0;           // 从什么时候开始一直连不上
static uint32_t s_helpUntil  = 0;           // 提示页显示到什么时候
static bool     s_autoApDone = false;       // 这次上电已自动开过热点（别反复开）
static uint32_t s_staRetryAt = 0;           // 1.46.61：热点里最后一次回头试 STA 的时间
static int      s_staRetryIdx = 0;          // 1.46.61：轮到第几组凭据

bool netAutoSetupTick() {
  const uint32_t now = millis();

  if (A.wifiOk) {                           // 连上了：计时归零，提示也该收了
    s_noNetAt   = now;
    s_helpUntil = 0;
    return false;
  }

  if (!s_noNetAt) s_noNetAt = now;

  // 已经在热点里（用户长按开的，或上次自动开的）→ 只负责把提示显示完
  if (netApActive()) {
    if (s_helpUntil && (int32_t)(now - s_helpUntil) >= 0) s_helpUntil = 0;
    return s_helpUntil != 0;
  }

  if (!s_autoApDone && !netEverUp() && (now - s_noNetAt) > NET_AUTOSETUP_MS) {
    s_autoApDone = true;
    netApOn();
    s_helpUntil = millis() + NET_HELP_SHOW_MS;
    Serial.println("[net] 60 秒没连上 WiFi → 自动开热点等人配网");
    evlog("net", "60s 没连上 WiFi（存了 %d 组凭据）→ 开热点配网", s_slotN);
    return true;
  }
  return false;
}

uint32_t netAutoSetupLeftMs() {
  if (!s_helpUntil) return 0;
  const int32_t left = (int32_t)(s_helpUntil - millis());
  return left > 0 ? (uint32_t)left : 0;
}

void netApOn() {
  if (s_apMode) return;
  WiFi.mode(WIFI_AP_STA);
  // 1.46.8：密码为空 → 开放热点（点一下就连，不用输 12345678）。
  //   显式传 nullptr：虽然传 "" 也会被库当成 OPEN（它按 strlen < 8 判），
  //   但那是"碰巧"，库里哪天改成"空串报错"就会静默失效 —— 热点开不起来最难查。
  WiFi.softAP(String(AP_SSID).c_str(), AP_PASS[0] ? AP_PASS : nullptr);
  s_apMode = true;
  if (MDNS.begin("myradio")) {
    MDNS.addService("http", "tcp", 80);
  }
  if (!netWifiOk()) {
    strncpy(s_ip, WiFi.softAPIP().toString().c_str(), sizeof(s_ip) - 1);
  }
  // 1.45.0：热点一开就把 DNS 全指回自己。手机连上后系统探测
  //   (connectivitycheck.gstatic.com / captive.apple.com …) 会被导到配网页，
  //   于是"连上就自动弹出配网页面"，不用用户自己去输入 192.168.4.1。
  if (!s_dnsOn) { s_dns.start(53, "*", WiFi.softAPIP()); s_dnsOn = true; }
  Serial.printf("[net] 热点已开「%s」/ %s -> http://%s（连上会自动弹配网页）\n",
                AP_SSID, AP_PASS[0] ? AP_PASS : "开放（无密码）", s_ip);
  A.dirty = true;
}

void netApOff() {
  if (!s_apMode) return;
  if (s_dnsOn) { s_dns.stop(); s_dnsOn = false; }   // 1.45.0
  WiFi.softAPdisconnect(true);       // true = 连在热点里的手机也踢掉
  s_apMode = false;
  WiFi.mode(WIFI_STA);               // 回单 STA，省电
  if (netWifiOk()) strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
  else             snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
  Serial.println("[net] 热点已关");
  A.dirty = true;
}

void netApToggle() {
  if (s_apMode) netApOff(); else netApOn();
}

// ============================================================
//  1.45.1：声音链路诊断接口
//    「网络台能解码但没声音」卡了很久，日志证明 PCM 是有电平的（体检 lv=30），
//    问题只可能在 I2S 三脚 / 功放使能 / 静音脚。这三样都在硬件上，
//    只能靠「改一下、听一下」来定；为此把最小的三个动作暴露成接口，
//    这样一轮排查不用烧一次固件。
//    安全：全部要 token；引脚一律拒绝 26~37（flash/PSRAM）、19/20（USB）、45/46（strapping）。
// ============================================================
static bool dbgPinBad(int p) {
  if (p < 0 || p > 48) return true;
  if (p >= 26 && p <= 37) return true;      // flash / 八线 PSRAM
  if (p == 19 || p == 20) return true;      // USB
  if (p == 45 || p == 46) return true;      // strapping
  // 1.45.2：1.45.1 只挡了 flash/USB/strapping，结果诊断时把 39/40/41
  //   （屏幕 D0~D2）拿去当 I2S，屏幕立刻花掉且一直花到重启。
  //   从现在起，凡是「别人家的线」一律拒绝，只剩 11/12/13/14 可动。
  if (p >= 39) return true;                 // 39~42 / 45~48：屏幕并口 D0~D7
  if (p >= 5 && p <= 9) return true;        // 5=RST 6=CS 7=DC 8=WR 9=RD
  if (p == 10) return true;                 // 功放使能
  if (p == 15) return true;                 // 外设电源自锁（拉错直接断电重启）
  if (p == 16) return true;                 // SI4732 复位
  if (p == 17 || p == 18) return true;      // SI4732 I2C
  if (p == 3) return true;                  // 功放静音
  if (p == 38) return true;                 // 背光
  if (p == 1 || p == 2 || p == 21) return true;  // 编码器 A/B/按键
  if (p == 4) return true;                  // 电池 ADC
  if (p == 0 || p == 43 || p == 44) return true; // BOOT / UART0
  return false;
}

static void hGpio() {
  if (!authed()) { denied(); return; }
  cors();
  const int p = argStr("p", "-1").toInt();
  if (dbgPinBad(p)) { errJson(400, "pin"); return; }
  const bool hasV = server.hasArg("v");
  const int v = argStr("v", "0").toInt();
  if (hasV)      { pinMode(p, OUTPUT); digitalWrite(p, v ? HIGH : LOW); }
  else if (server.hasArg("in")) { pinMode(p, INPUT); }
  const int rd = digitalRead(p);
  String o = "{\"p\":"; o += p;
  o += ",\"read\":"; o += rd;
  o += ",\"drv\":";  o += (hasV ? (v ? 1 : 0) : -1);
  o += "}";
  okJson(o.c_str());
}

// 1.45.5：编码器诊断 —— 转一格到底几个边沿（判断 ENC_DIV 该是 4 还是 2）
static void hEnc() {
  if (!authed()) { denied(); return; }
  cors();
  okJson(inputEncDiag());
}

static void hPins() {
  if (!authed()) { denied(); return; }
  cors();
  const int bck = argStr("bck", "-1").toInt();
  const int ws  = argStr("ws",  "-1").toInt();
  const int din = argStr("din", "-1").toInt();
  if (dbgPinBad(bck) || dbgPinBad(ws) || dbgPinBad(din)) { errJson(400, "pin"); return; }
  audioSetPins(bck, ws, din);
  String o = "{\"ok\":1,\"bck\":"; o += bck;
  o += ",\"ws\":";  o += ws;
  o += ",\"din\":"; o += din;
  o += "}";
  okJson(o.c_str());
}

static void hTone() {
  if (!authed()) { denied(); return; }
  cors();
  const int hz = argStr("hz", "1000").toInt();
  const int ms = argStr("ms", "2500").toInt();
  if (!audioMakeTone("/tone.wav", hz, ms)) { errJson(500, "wav"); return; }
  const bool ok = audioVoice("/tone.wav");
  String o = "{\"ok\":"; o += ok ? 1 : 0;
  o += ",\"hz\":"; o += hz;
  o += ",\"ms\":"; o += ms;
  o += "}";
  okJson(o.c_str());
}

// 1.45.1：开/关 I2S 三脚扫描（诊断）。on=1 顺便把功放使能和静音脚摆到「放声」。
static void hSweep() {
  if (!authed()) { denied(); return; }
  cors();
  const bool on = argStr("on", "1").toInt() != 0;
  if (on) {
#if PIN_AMP_EN >= 0 && AMP_EN_ENABLE
    pinMode(PIN_AMP_EN, OUTPUT); digitalWrite(PIN_AMP_EN, AMP_EN_LEVEL);
#endif
#if PIN_FM_AMP_MUTE >= 0
    pinMode(PIN_FM_AMP_MUTE, OUTPUT); digitalWrite(PIN_FM_AMP_MUTE, LOW);
#endif
  }
  audioSweep(on);
  String o = "{\"ok\":1,\"on\":"; o += on ? 1 : 0;
  o += ",\"n\":"; o += audioSweepCount();
  o += "}";
  okJson(o.c_str());
}

// ------------------------------------------------------------
//  1.46.5：按 4 位手机连接码认机器
//    GET /api/find?code=1234
//      对上 → {"ok":1,"name","fw","ip","devid","locked","count"}
//      不对 → {"ok":0,"bad":1}
//      没带 → {"ok":0,"need":"code"}
//  免鉴权（还没连上时手机根本没有令牌），但**只回**这些公开信息：
//  名字/版本/IP/设备码本来就在 /api/info 里，令牌和遥控码一个字都不给。
//  这是「怎么找到机器」，不是「怎么获得权限」。
// ------------------------------------------------------------
static void hFind() {
  cors();
  String code = argStr("code");
  code.trim();
  if (!code.length()) { okJson("{\"ok\":0,\"need\":\"code\"}"); return; }
  if (!pinMatch(code.c_str())) { okJson("{\"ok\":0,\"bad\":1}"); return; }

  String o = "{\"ok\":1,";
  kv(o, "name", FW_NAME);
  o += ',';
  kv(o, "fw", FW_NAME " " FW_VER);
  o += ',';
  {
    // 热点模式下 s_ip 还是 "-"（netLoop 只在 STA 连上时才填），
    // 这时按当前真正能给手机的地址回，别让 App 拿 "-" 去连。
    IPAddress ipa = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
    char ipb[20];
    snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u", ipa[0], ipa[1], ipa[2], ipa[3]);
    kv(o, "ip", ipb);
  }
  o += ',';
  kv(o, "devid", cloudDevId());
  o += ",\"locked\":";
  o += s_locked ? 1 : 0;
  o += ",\"count\":";
  o += g_stN;
  o += "}";
  okJson(o.c_str());
}

void netBegin() {
  s_bootAt = millis();

  // ---- 1.46.63：时区必须自己设，别指望 configTime 的偏移版 ----
  //  真因（1.46.62 实机实测）：屏上时间比北京时间慢 8 小时 —— 机器报 17:27，
  //  本地是次日 01:27，星期也跟着错成前一天。查下去是 Arduino core 2.0.17 的坑：
  //    configTime(8*3600, 0, ...)  →  setTimeZone(-28800, 0)
  //    setTimeZone() 里 `if (daylight != 3600)` 把 daylight=0 **也算进** DST 分支，
  //    拼出 TZ = "UTC-8DST-8"（没有切换规则，还挂个假的夏令时名）；
  //    newlib 解析失败就直接退回 UTC ⇒ getLocalTime() 一辈子给 UTC。
  //  这里在 WiFi 起来之前就把 TZ 钉成 POSIX 的 CST-8（= UTC+8，无夏令时）：
  //  即使 NTP 还没同步、甚至压根没网，只要系统时间有值，显示就是北京时间。
  setenv("TZ", "CST-8", 1);
  tzset();
  Serial.printf("[net] 时区 TZ=%s（期望 CST-8 = UTC+8）\n",
                getenv("TZ") ? getenv("TZ") : "(null)");
  const char* tk = storeToken();
  if (tk) strncpy(s_tok, tk, sizeof(s_tok) - 1);
  loadPair();

  // 1.43.0：热点默认**不**自动开（AP_ALWAYS_ON=0），要开就长按旋钮 3 秒。
  //   但 WiFi(STA) 照样开机就工作 —— 下面接着读存过的密码去连路由器。
  if (AP_ALWAYS_ON) startAp();
  else {
    WiFi.mode(WIFI_STA);           // 明确置成 STA，避免上一次残留的 AP 状态
    Serial.println("[net] 热点关（长按旋钮 3 秒可开）");
  }

  // 1.22.0：最多 3 组，先试第一组（= 上次成功用的那个）
  {
    WifiCred slots[WIFI_MAX_SLOTS];
    s_slotN = readWifiSlots(slots);
    s_slotIdx = 0;
    s_everUp = false;
    s_haveCred = (s_slotN > 0);
    // 1.44.1：一组都没存过 = STA 永远连不上，而这版热点默认又是关的，
    //   设备就彻底没有入口了（实机烧完就是 wifi=0 / ip=0.0.0.0 / 热点也没开）。
    //   这种状态下自动把热点打开，手机连上「" AP_SSID "」就能配网；
    //   存过密码的机器仍按用户的规矩：热点默认关，长按旋钮 3 秒才开。
    if (!s_haveCred && !AP_ALWAYS_ON) {
      Serial.println("[net] 没存过 WiFi → 自动开热点（SSID 「" AP_SSID "」，连上后自动弹配网页）");
      netApOn();
    }
    if (s_slotN > 0) {
      s_oldSsid = slots[0].ssid;
      s_oldPass = slots[0].pass;
      WiFi.setSleep(false);          // 关省电，音频流更稳
      WiFi.setAutoReconnect(true);
      WiFi.begin(s_oldSsid.c_str(), s_oldPass.c_str());
      s_slotTryAt = millis();
      Serial.printf("[net] 存了 %d 组网络，先连第 1 组 %s\n", s_slotN, s_oldSsid.c_str());
      for (int i = 1; i < s_slotN; i++)
        Serial.printf("[net]   备用 %d: %s\n", i + 1, slots[i].ssid.c_str());
    }
  }

  server.on("/",                HTTP_GET,  hRoot);
  server.on("/manifest.webmanifest", HTTP_GET, hManifest);
  server.on("/icon.svg",        HTTP_GET,  hIcon);
  server.on("/setup",           HTTP_GET,  hProvPage);   // 1.45.0：配网页
  server.on("/setup",           HTTP_POST, hProvSave);
  server.on("/api/prov/slots",  HTTP_GET,  hProvSlots);
  // 1.45.0：各家系统的联网探测地址，统一踢到配网页 → 手机自动弹浏览器
  server.on("/generate_204",         HTTP_GET, hCaptive);   // Android
  server.on("/gen_204",              HTTP_GET, hCaptive);
  server.on("/hotspot-detect.html",  HTTP_GET, hCaptive);   // iOS / macOS
  server.on("/library/test/success.html", HTTP_GET, hCaptive);
  server.on("/ncsi.txt",             HTTP_GET, hCaptive);   // Windows
  server.on("/connecttest.txt",      HTTP_GET, hCaptive);
  server.on("/redirect",             HTTP_GET, hCaptive);
  server.on("/canonical.html",       HTTP_GET, hCaptive);
  server.on("/success.txt",          HTTP_GET, hCaptive);
  server.on("/fwlink",               HTTP_GET, hCaptive);

  server.on("/api/info",        HTTP_GET,  hInfo);
  server.on("/api/auth",        HTTP_GET,  hAuth);
  server.on("/api/state",       HTTP_GET,  hState);
  server.on("/api/screen",      HTTP_GET,  hScreen);
  server.on("/api/stations",    HTTP_GET,  hStations);
  server.on("/api/history",     HTTP_GET,  hHistory);
  server.on("/api/diag",        HTTP_GET,  hDiag);
  server.on("/api/health",      HTTP_GET,  hHealth);     // 1.17.0：台的健康档案
  server.on("/api/health",      HTTP_POST, hHealth);
  server.on("/api/pair",        HTTP_GET,  hPair);
  server.on("/api/pair",        HTTP_POST, hPair);
  server.on("/api/find",        HTTP_GET,  hFind);        // 1.46.5：按 4 位码找机器
  server.on("/api/offair",      HTTP_GET,  hOffair);      // 1.46.6：停播台账
  server.on("/api/offair",      HTTP_POST, hOffair);

  server.on("/api/play",        HTTP_POST, hPlay);
  server.on("/api/step",        HTTP_POST, hStep);
  server.on("/api/epis",        HTTP_POST, hEpis);
  server.on("/api/group",       HTTP_POST, hGroup);
  server.on("/api/vol",         HTTP_POST, hVol);
  // 1.22.0
  server.on("/api/squelch",     HTTP_POST, hSquelch);
  server.on("/api/epnext",      HTTP_POST, hEpNext);   // 1.46.33：下次从第 X 集起
  server.on("/api/halfstep",    HTTP_POST, hHalfStep);
  server.on("/api/battcal",     HTTP_POST, hBattCal);      // 1.38.2：电量校准
  server.on("/api/fmscan",      HTTP_POST, hFmScan);        // 1.38.0：重新搜台
  server.on("/api/fmstations",  HTTP_GET,  hFmStations);    // 1.38.0：搜到的台表
  server.on("/api/fmtune",      HTTP_POST, hFmTune);        // 1.41.0：直接调到某个频率
  server.on("/api/wifi/list",   HTTP_GET,  hWifiList);
  server.on("/api/wifi/forget", HTTP_POST, hWifiForget);
  server.on("/api/wifi/add",    HTTP_POST, hWifiAdd);   // 1.46.51：只存不连
  server.on("/api/wifi/use",    HTTP_POST, hWifiUse);   // 1.46.51：用已存的密码连
  server.on("/api/set",         HTTP_POST, hSet);
  server.on("/api/mute",        HTTP_POST, hMute);
  server.on("/api/pause",       HTTP_POST, hPause);

  server.on("/api/add",         HTTP_POST, hAdd);
  server.on("/api/edit",        HTTP_POST, hEdit);
  server.on("/api/del",         HTTP_POST, hDel);
  server.on("/api/move",        HTTP_POST, hMove);
  server.on("/api/mvgroup",     HTTP_POST, hMvGroup);
  server.on("/api/groupop",     HTTP_POST, hGroupOp);

  server.on("/api/log",           HTTP_GET,  hLog);   // 1.46.39
  server.on("/api/fmprobe",       HTTP_GET,  hFmProbe); // 1.46.39
  server.on("/api/fmtest",        HTTP_GET,  hFmTest);  // 1.46.43
  server.on("/api/fix",         HTTP_POST, hFix);
  server.on("/api/batt",        HTTP_GET,  hBatt);
  server.on("/api/wifi",        HTTP_POST, hWifi);
  server.on("/api/wifi/status", HTTP_GET,  hWifiStatus);
  server.on("/api/scan",        HTTP_GET,  hScan);
  server.on("/api/gpio",        HTTP_GET,  hGpio);       // 1.45.1：读/写单个 GPIO
  server.on("/api/enc",         HTTP_GET,  hEnc);        // 1.45.5：旋钮每格几个边沿
  server.on("/api/pins",        HTTP_GET,  hPins);       // 1.45.1：在线换 I2S 三脚
  server.on("/api/tone",        HTTP_GET,  hTone);       // 1.45.1：放本地测试音
  server.on("/api/sweep",       HTTP_GET,  hSweep);      // 1.45.1：自动扫 I2S 三脚
  server.on("/api/cloud",       HTTP_GET,  hCloud);
  server.on("/api/cloud",       HTTP_POST, hCloud);

  server.onNotFound([]() {
    cors();
    // 1.45.0：热点里的任何未知路径（手机爱乱请求 /favicon.ico 之类）
    //   都送回配网页，别让用户在大网页版本里迷路。
    //
    // 1.46.8：**这里必须 302，不能 200** —— 这是"连上热点就自动弹配网页"的关键。
    //   各家系统判定 captive portal 的方式不一样：Android 请求
    //   connectivitycheck.gstatic.com/generate_204（期待 204 空 body）、
    //   iOS 请求 captive.apple.com/hotspot-detect.html、华为/小米/vivo 各有自己的地址。
    //   我们靠 DNS（s_dns 的 "*"）把这些域名全指回自己，但**路径**各不相同 ——
    //   只会命中这条 onNotFound。回 200 + HTML，有的系统会当成"网是通的、只是没有
    //   那个文件"，于是不弹登录页；回 302 才是它们共同认可的"这网要登录"信号。
    //   （Android 12+ 在开放网络上检测到 302 会直接拉起登录界面，用户不用再点通知。）
    {
      IPAddress rip = server.client().remoteIP();
      const bool fromAp = s_apMode && rip[0] == 192 && rip[1] == 168 && rip[2] == 4;
      if (fromAp) {
        String loc = "http://";
        loc += WiFi.softAPIP().toString();
        loc += "/setup";
        server.sendHeader("Location", loc, true);
        server.send(302, "text/html; charset=utf-8",
                    "<!doctype html><meta charset=\"utf-8\"><a href=\"/setup\">点这里配网（" AP_SSID "）</a>");
        return;
      }
    }
    // 单页应用：任何未知路径都回主页
    server.send_P(200, "text/html; charset=utf-8", WEBAPP_HTML);
  });
  // 1.46.19：ESP32 的 WebServer **只解析 collectHeaders() 里登记过的请求头**。
  //   网页一直用 X-Token 头带配对令牌（core.js 的 getLan/postLan），但这行登记
  //   一直没有 —— server.header("X-Token") 恒为空串，authed() 只剩 ?t= 一条路，
  //   于是 App / 浏览器一进首页，所有 /api/* 全是 401，顶上永远挂「没连上收音机」。
  //   （用户报「输 1234 找到了机器，进去却显示没连上」就是这条。）
  {
    static const char* kHdrKeys[] = {"X-Token"};
    server.collectHeaders(kHdrKeys, 1);
  }
  server.begin();
  Serial.printf("[net] app: http://%s  或  http://myradio.local\n", s_ip);
}

void netLoop() {
  server.handleClient();
  if (s_dnsOn) s_dns.processNextRequest();   // 1.45.0：captive portal 的 DNS

  static uint32_t lastTick = 0;
  const uint32_t now = millis();

  // 1.46.36：当前用的那组被删了 —— 到点断开，改连剩下的第一组；
  //   一组都不剩就开热点，让手机能连上来重新配网（否则机器彻底失联）。
  if (s_wifiDropAt && (int32_t)(now - s_wifiDropAt) >= 0) {
    s_wifiDropAt = 0;
    WiFi.disconnect(false, false);
    delay(80);
    WifiCred v[WIFI_MAX_SLOTS];
    const int left = readWifiSlots(v);
    s_slotN = left;
    s_slotIdx = 0;
    s_everUp = false;
    s_slotTryAt = now;
    s_lastTry = now;
    if (left > 0) {
      Serial.printf("[wifi] 改连剩下的第 1 组：%s\n", v[0].ssid.c_str());
      WiFi.begin(v[0].ssid.c_str(), v[0].pass.c_str());
    } else {
      Serial.println("[wifi] 一组都不剩了，开热点等配网");
      netApOn();
    }
  }

  // 1.46.35：到点了才真正断链重连（回包这时已经发出去）
  if (s_reconnAt && (int32_t)(now - s_reconnAt) >= 0) {
    s_reconnAt = 0;
    Serial.printf("[net] 重连 WiFi：%s\n", s_reconnSsid.length() ? s_reconnSsid.c_str() : "(上次那组)");
    WiFi.disconnect(false, false);
    delay(80);
    if (s_reconnSsid.length()) WiFi.begin(s_reconnSsid.c_str(), s_reconnPass.c_str());
    else                       WiFi.reconnect();
    s_lastTry = now;                  // 别让下面那句 8 秒兜底立刻又插一脚
    s_wifiState = 1;
  }

  A.wifiOk = netWifiOk();
  if (A.wifiOk) s_everUp = true;      // 1.22.0：本次上电连上过，就不再乱换网
  // ---- 1.46.61：自动开的热点不能变成永久状态 ----
  //  1.46.53 加了「60 秒连不上就开热点等人配网」，但热点一开就再也回不去了：
  //  下面那串 else-if 在 s_apMode 为真时只剩「报一下 AP 的 IP」一条路，没有重连分支；
  //  netAutoSetupTick() 更是在 netApActive() 时直接 return。
  //  于是「手机热点关了一会儿 / 路由器重启」→ 机器永久停在「我的收音机」热点里：
  //  在线台、有声书全哑，NTP 也不再同步（屏上的时间越跑越偏），
  //  手机连着家里 WiFi 去打 192.168.4.1 必然是 ERR_CONNECTION_TIMED_OUT。
  //  现在：自动开的热点每 30 秒回头试一组存过的 WiFi，连上就 8 秒后自动关热点。
  //  （长按旋钮手动开的热点不受影响 —— 那是用户自己要用。）
  if (s_autoApDone && s_apMode && !A.wifiOk && now - s_staRetryAt > 30000) {
    s_staRetryAt = now;
    WifiCred v[WIFI_MAX_SLOTS];
    const int n = readWifiSlots(v);
    if (n > 0) {
      if (s_staRetryIdx >= n) s_staRetryIdx = 0;
      Serial.printf("[net] 热点里回头试第 %d/%d 组：%s\n",
                    s_staRetryIdx + 1, n, v[s_staRetryIdx].ssid.c_str());
      WiFi.begin(v[s_staRetryIdx].ssid.c_str(), v[s_staRetryIdx].pass.c_str());
      s_staRetryIdx++;
    }
  }
  if (s_autoApDone && s_apMode && A.wifiOk && !s_apOffDeadline) {
    s_apOffDeadline = now + 8000;
    s_autoApDone    = false;          // 回常态：以后掉线走原来的 8 秒重连
    Serial.println("[net] 热点模式下重连成功，8 秒后关热点");
    evlog("net", "自动热点里重连上 %s，关热点", WiFi.SSID().c_str());
  }
  if (A.wifiOk) {
    strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);

    struct tm tmNow;
    // ---- 1.46.63：两处一起改 ----
    //  ① 用 configTzTime 显式给 POSIX 时区串。configTime 的偏移版在 core 2.0.17
    //    上会生成非法的 TZ（长注释见 netBegin），时间会一直按 UTC 显示。
    //  ② 重试间隔 30 秒 → 10 分钟。lwIP 的 SNTP 请求超时远大于 30 秒，原来那句
    //    会在请求还没回来时就 sntp_stop()/sntp_init() 掐掉重来 ——
    //    路由器慢或者 DNS 慢的时候，反而一辈子同步不上。
    if (!getLocalTime(&tmNow, 0) && (!s_lastNtp || now - s_lastNtp > 600000)) {
      s_lastNtp = now;
      configTzTime("CST-8", "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
      Serial.println("[net] 校时请求已发出（configTzTime / ntp.aliyun.com）");
    }
    // 第一次真正拿到时间时打一行，方便以后一眼看出校时到底成没成。
    static bool s_tmLogged = false;
    if (!s_tmLogged && getLocalTime(&tmNow, 0)) {
      s_tmLogged = true;
      Serial.printf("[net] 时间已同步 %04d-%02d-%02d %02d:%02d:%02d (TZ=%s)\n",
                    tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                    tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
                    getenv("TZ") ? getenv("TZ") : "(null)");
    }
    static bool mdnsDone = false;
    if (!mdnsDone) {
      mdnsDone = true;
      if (MDNS.begin("myradio")) MDNS.addService("http", "tcp", 80);
      Serial.printf("[net] online http://%s  http://myradio.local\n", s_ip);
    }
    if (s_wifiState == 0) s_wifiState = 2;
  } else if (!s_apMode && s_slotN > 1 && !s_everUp && now - s_slotTryAt > NET_CRED_TRY_MS) {
    // ---- 1.22.0：这一组迟迟连不上 → 换下一组 ----
    // 只在「本次上电还没连上过」时轮换。一旦连上过，掉线走下面原来的 reconnect，
    // 否则路由器重启一次，机器就会在 3 组之间瞎跳。
    WifiCred slots[WIFI_MAX_SLOTS];
    const int n = readWifiSlots(slots);
    if (n > 1) {
      s_slotN = n;
      s_slotIdx = (s_slotIdx + 1) % s_slotN;
      s_oldSsid = slots[s_slotIdx].ssid;
      s_oldPass = slots[s_slotIdx].pass;
      s_everUp = false;
      s_slotTryAt = now;
      Serial.printf("[net] 上一组没连上，换第 %d/%d 组：%s\n",
                    s_slotIdx + 1, s_slotN, s_oldSsid.c_str());
      WiFi.disconnect(false, false);
      WiFi.begin(s_oldSsid.c_str(), s_oldPass.c_str());
    }
  } else if (!s_apMode && now - s_lastTry > 8000) {
    s_lastTry = now;
    WiFi.reconnect();
  } else if (s_apMode && !A.wifiOk) {
    strncpy(s_ip, WiFi.softAPIP().toString().c_str(), sizeof(s_ip) - 1);
  } else if (!s_apMode && !A.wifiOk) {
    // 1.43.0：热点关着、路由器也没连上 —— IP 是假的，别显示成上次那个
    snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
  }

  // 1.45.0：从配网页存完、真的连上之后 8 秒关热点。
  //   手机这时已经能通过家里的 WiFi 找到收音机，热点留着反而会被手机误连
  //   （连上没网的热点，微信就收不到了）。
  if (s_provSaved && A.wifiOk) {
    s_provSaved = false;
    s_apOffDeadline = now + 8000;
    Serial.println("[prov] 配网成功，热点 8 秒后自动关闭");
  }
  if (s_apOffDeadline && (int32_t)(now - s_apOffDeadline) >= 0) {
    s_apOffDeadline = 0;
    if (s_apMode) netApOff();
  }

  // 换网试连：成功才写盘，超时就退回原来那个，别把机器留在断网状态
  if (s_pend) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == s_pendSsid) {
      // 1.46.51：从「已存的那组」连过来的不置顶，顺序就是用户排的顺序
      if (!s_pendKeepOrder) saveWifiFile(s_pendSsid, s_pendPass);
      s_oldSsid = s_pendSsid;
      s_oldPass = s_pendPass;
      s_haveCred = true;
      s_pend = false;
      s_wifiState = 2;
      snprintf(s_wifiMsg, sizeof(s_wifiMsg), "已连上 %s", s_pendSsid.c_str());
      Serial.printf("[net] switched to %s\n", s_pendSsid.c_str());
    } else if (now - s_tryAt > 25000) {
      s_pend = false;
      s_wifiState = 3;
      snprintf(s_wifiMsg, sizeof(s_wifiMsg), "连不上 %s，密码或信号有问题", s_pendSsid.c_str());
      if (s_oldSsid.length()) WiFi.begin(s_oldSsid.c_str(), s_oldPass.c_str());
      Serial.println("[net] wifi switch failed, rollback");
    }
  }

  // RSSI 每 5 秒刷一次，别让 UI 一直重绘
  if (A.wifiOk && now - lastTick > 5000) {
    lastTick = now;
    A.rssi = WiFi.RSSI();
  }
}
