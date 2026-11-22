// ============================================================
//  cloud.cpp —— 跨网远程控制（1.8.0）
//
//  机器是「客户端」：主动往公网 MQTT 服务器上连一条线。
//  手机在同一个 WiFi 里就走局域网 HTTP（快），不在就走这条线（能到）。
//
//  命令格式（手机 → 机器，主题 myradio/<设备码>/c）：
//    {"i":12,"k":"482913","m":"POST","p":"/api/vol","b":"v=50"}
//      i  自增序号，用来把响应配回去
//      k  6 位遥控码，对不上直接丢
//      m  GET / POST（只影响语义，机器内部都按动作走）
//      p  与局域网 API 完全相同的路径
//      b  form 参数串（与 HTTP 的 body 一样）
//
//  响应（机器 → 手机，主题 myradio/<设备码>/r）：
//    {"i":12,"s":200,"d":{"ok":1}}     d 就是局域网那套 JSON 原文
//
//  1.19.0 起多了两件事（"免配对"）：
//    · 在线广播：机器每隔 30 秒往 myradio/dev/<6位短码> 发一条 retain 的小 JSON
//      （名字/固件/电量/是否在线）。App 一打开就订阅这个通配主题，于是任何一台
//      手机 —— 不管是不是当初配网的那台 —— 都能看见"家里那台收音机"，点一下就用。
//    · 遥控码可推导：不用屏幕上那串随机码，而是 cloudDerivedCode(设备码) 算出来的。
//      算法是 FNV-1a 32，加盐 "MyRadio-1"，取模 900000 再加 100000。
//      !! tools/app/js/cloud.js 的 deriveCode() 是同一份算法，改这里必须改那里，
//         两边的测试向量在 tools/verify_cloud.js 的 [10] 段。
//
//  为什么不直接把 HTTP 转给 127.0.0.1：
//    本机的 WebServer 是同步的，handleClient() 在主循环里跑。
//    主循环里再发一个 HTTP 请求给自己 = 自己等自己 = 必然超时。
//    所以这里直接调 api.h 那层动作，不绕 HTTP。
// ============================================================
#include "cloud.h"
#include "config.h"
#include "state.h"
#include "store.h"
#include "api.h"
#include "net.h"
#include "battery.h"
#include "pairpin.h"        // 1.46.7：4 位连接码，跟着在线广播一起发出去
#include "fm.h"              // 1.46.33：跨网「重新搜台」要调 fmScanStart()

#include <WiFi.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <ctype.h>
#include "mqtt_client.h"

static const char* F_CLOUD = "/cloud.txt";

static esp_mqtt_client_handle_t s_cli = nullptr;
// 1.42.0：远程控制**恒开**。用户原话「远程控制应该本来就有，而不是用按键开关打开」。
// 之前 s_on 会被 /cloud.txt 的 on=0 改掉，一次误关之后 App 再也连不上（而且没网时改不回来）。
// 现在这个变量只为兼容 saveCfg/日志保留，恒 true。
static bool s_on      = true;
static bool s_open    = CLOUD_OPEN ? true : false;   // "免配对"：广播 + 认派生码
static uint32_t s_presAt = 0;                        // 上次广播时刻
static uint32_t s_presN  = 0;                        // 广播了几次（诊断用）
static bool s_conn    = false;
static bool s_started = false;

static char s_dev[13]   = {0};      // 12 位十六进制（WiFi MAC）
static char s_short[7]  = {0};      // 后 6 位，给人看
static char s_code[8]   = {0};      // 6 位遥控码
static char s_host[40]  = CLOUD_HOST;
static int  s_port      = CLOUD_PORT;

static String s_tC, s_tR;           // 命令主题 / 响应主题

// ---- 接收缓冲（MQTT 任务写，主循环读）----
// 台表列表最大约 9KB，所以给 9.5KB。一次只排一条命令：人手点的频率远低于此。
static char         s_rx[9500];
static int          s_rxGot   = 0;
static int          s_rxTotal = 0;
static volatile bool s_qFull  = false;
static char         s_q[CLOUD_MAX_CMD];

static uint32_t s_cmdCount = 0;
static uint32_t s_badCount = 0;

// ------------------------------------------------------------
//  设备码 / 遥控码
// ------------------------------------------------------------
static void makeDevId() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_dev, sizeof(s_dev), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  memcpy(s_short, s_dev + 6, 6);
  s_short[6] = 0;
}

static void makeCode() {
  uint32_t r = esp_random() % 1000000u;
  snprintf(s_code, sizeof(s_code), "%06u", (unsigned)r);
}

// 从设备码算出遥控码（1.19.0）：FNV-1a 32 + 盐 "MyRadio-1"，取模 900000 再加 100000。
// !! 这是**公开算法**：算得出来就意味着谁都能控（见 config.h 里 CLOUD_OPEN 的说明）。
//    想收紧就在 App 里关掉「免配对」，机器立刻只认屏幕上那串随机码。
// !! tools/app/js/cloud.js 的 deriveCode() 必须与这里逐字节一致。
static void deriveFrom(const char* dev, char* out, size_t n) {
  uint32_t h = 2166136261u;
  for (const char* p = "MyRadio-1"; *p; p++) { h ^= (uint32_t)(uint8_t)*p; h *= 16777619u; }
  for (const char* p = dev; p && *p; p++) {
    h ^= (uint32_t)(uint8_t)toupper((unsigned char)*p);
    h *= 16777619u;
  }
  snprintf(out, n, "%06u", (unsigned)(100000u + h % 900000u));
}

// ------------------------------------------------------------
//  存盘：一行一个 key=value
// ------------------------------------------------------------
static void saveCfg() {
  File f = LittleFS.open(F_CLOUD, "w");
  if (!f) return;
  f.printf("on=%d\ncode=%s\nhost=%s:%d\nopen=%d\n",
           s_on ? 1 : 0, s_code, s_host, s_port, s_open ? 1 : 0);
  f.close();
}

static void loadCfg() {
  if (!LittleFS.exists(F_CLOUD)) return;
  File f = LittleFS.open(F_CLOUD, "r");
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int eq = ln.indexOf('=');
    if (eq <= 0) continue;
    String k = ln.substring(0, eq), v = ln.substring(eq + 1);
    if (k == "on") {
      // 1.42.0：**故意忽略**。老版本可能存过 on=0，读它会让机器永久失联远程。
      // 留这个分支是为了让解析逻辑对旧文件不报错。
    } else if (k == "open") {
      s_open = (v.toInt() != 0);
    } else if (k == "code") {
      if (v.length() == 6) { strncpy(s_code, v.c_str(), sizeof(s_code) - 1); s_code[6] = 0; }
    } else if (k == "host") {
      int c = v.indexOf(':');
      String h = (c > 0) ? v.substring(0, c) : v;
      int    p = (c > 0) ? v.substring(c + 1).toInt() : CLOUD_PORT;
      if (h.length() && h.length() < (int)sizeof(s_host)) {
        strncpy(s_host, h.c_str(), sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = 0;
        if (p > 0 && p < 65536) s_port = p;
      }
    }
  }
  f.close();
}

// ------------------------------------------------------------
//  极小 JSON / form 取值（不引第三方库；命令是我们自己发的，格式固定）
// ------------------------------------------------------------
static bool jStr(const char* s, const char* key, char* out, size_t n) {
  if (out && n) out[0] = 0;
  char pat[16];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(s, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) { p++; }
    if (i + 1 < n) out[i++] = *p;
    p++;
  }
  if (out && n) out[i] = 0;
  return true;
}

static long jInt(const char* s, const char* key, long def) {
  char pat[16];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(s, pat);
  if (!p) return def;
  p += strlen(pat);
  while (*p == ' ') p++;
  if (*p != '-' && (*p < '0' || *p > '9')) return def;
  return atol(p);
}

// 1.46.33：跨网那条通道的 body 是 App 用 new URLSearchParams(body) 编出来的，
//   中文台名会变成 %E4%B8%AD…、流地址里的 :// 会变成 %3A%2F%2F。
//   以前这里直接取值不解码 —— 结果「远程添加台」存进去的是一串 %XX，
//   台名是乱码、地址打不开，用户看到的就是「添加流地址失败」。
//   HTTP 那条路由 WebServer 自己解码，所以只有这条路需要补。
static void urlDecodeInPlace(char* s) {
  if (!s) return;
  char* w = s;
  for (const char* r = s; *r; r++) {
    if (*r == '+') { *w++ = ' '; continue; }
    if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      *w++ = (char)((hex(r[1]) << 4) | hex(r[2]));
      r += 2;
      continue;
    }
    *w++ = *r;
  }
  *w = 0;
}

static long formLong(const char* b, const char* k, long def) {
  if (!b || !*b) return def;
  char pat[20];
  snprintf(pat, sizeof(pat), "%s=", k);
  size_t pl = strlen(pat);
  const char* p = b;
  while (p && *p) {
    if (!strncmp(p, pat, pl)) return atol(p + pl);
    const char* amp = strchr(p, '&');
    if (!amp) break;
    p = amp + 1;
  }
  return def;
}

static bool formStr(const char* b, const char* k, char* out, size_t n) {
  if (out && n) out[0] = 0;
  if (!b || !*b) return false;
  char pat[20];
  snprintf(pat, sizeof(pat), "%s=", k);
  size_t pl = strlen(pat);
  const char* p = b;
  while (p && *p) {
    if (!strncmp(p, pat, pl)) {
      const char* v = p + pl;
      size_t i = 0;
      while (*v && *v != '&' && i + 1 < n) out[i++] = *v++;
      if (out && n) out[i] = 0;
      urlDecodeInPlace(out);        // 1.46.33：%XX / + 还原
      return true;
    }
    const char* amp = strchr(p, '&');
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

// ------------------------------------------------------------
//  在线广播（1.19.0）
// ------------------------------------------------------------
// 保留（retain）一条小 JSON 在 myradio/dev/<短码> 上：
//   {"d":"A1B2C3","id":"A1B2C3D4E5F6","n":"MyRadio-A1B2C3","fw":"1.46.7",
//    "open":1,"up":1234,"batt":82,"pin":"1234"}
// App 订阅 myradio/dev/+ 就能列出"看得见的机器"，不用手抄遥控码。
// 关掉「免配对」时把这条**清成空保留消息**，App 那边就看不见它了。
//
// 1.46.7：末尾的 "pin" 就是机器那 4 位连接码（出厂 1234）。手机拿到后在本地
//   按码筛一遍 —— 只留一台就自动连上，等于把局域网的"输码就连"搬到跨网。
//   ⚠️ 别把它挪到单独主题（如 myradio/pin/1234）：retain 一个主题只存一条，
//      两台机器都叫 1234 时会互相覆盖，输码就可能连到别人家。
static void pubPresence(bool clear) {
  if (!s_cli || !s_conn) return;
  String t = String(CLOUD_DEV_PREFIX) + s_short;
  if (clear) {
    esp_mqtt_client_publish(s_cli, t.c_str(), "", 0, 0, 1);
    return;
  }
  // 1.46.7：buf 从 176 提到 208 —— 多了 "pin":"1234" 这一段（含键名 14 字节）
  char b[208];
  snprintf(b, sizeof(b),
           "{\"d\":\"%s\",\"id\":\"%s\",\"n\":\"MyRadio-%s\",\"fw\":\"%s\","
           "\"open\":%d,\"up\":%lu,\"batt\":%d,\"pin\":\"%s\"}",
           s_short, s_dev, s_short, FW_VER, s_open ? 1 : 0,
           (unsigned long)(millis() / 1000UL), batteryPct(), pinCode());
  esp_mqtt_client_publish(s_cli, t.c_str(), b, (int)strlen(b), 0, 1);
  s_presN++;
}

static void pub(const String& s) {
  if (!s_cli || !s_conn) return;
  esp_mqtt_client_publish(s_cli, s_tR.c_str(), s.c_str(), s.length(), 0, 0);
}

// ------------------------------------------------------------
//  执行一条命令
// ------------------------------------------------------------
static void runCmd(const char* js) {
  long id = jInt(js, "i", 0);

  char code[12] = {0};
  char path[96] = {0};
  char form[512] = {0};
  jStr(js, "k", code, sizeof(code));
  jStr(js, "p", path, sizeof(path));
  jStr(js, "b", form, sizeof(form));

  String o = "{\"i\":";
  o += id;

  // 遥控码不对：不执行、不回内容，只报错（防着公共服务器上有人乱试）
  // 1.19.0：开了「免配对」时，从设备码算出来的那串也认（App 就是这么用的）
  bool codeOk = (strcmp(code, s_code) == 0);
  if (!codeOk && s_open) {
    char dc[8] = {0};
    deriveFrom(s_dev, dc, sizeof(dc));
    codeOk = (strcmp(code, dc) == 0);
  }
  if (!codeOk) {
    s_badCount++;
    o += ",\"s\":403,\"d\":{\"err\":\"code\"}}";
    pub(o);
    Serial.printf("[cloud] 遥控码不对，丢弃（%lu）\n", (unsigned long)s_badCount);
    return;
  }

  s_cmdCount++;
  String d;
  bool ok = true;
  bool needReboot = false;        // 1.46.33：重启类动作要等回包发出去

  if (!strcmp(path, "/api/screen")) {
    apiScreenJson(d);
  } else if (!strcmp(path, "/api/stations")) {
    apiStationsJson(d);
  } else if (!strcmp(path, "/api/info")) {
    d = "{\"fw\":\"";
    d += FW_NAME;
    d += " ";
    d += FW_VER;
    d += "\",\"devid\":\"";
    d += s_dev;
    d += "\",\"short\":\"";
    d += s_short;
    d += "\",\"code\":\"";
    d += s_code;
    d += "\",\"count\":";
    d += g_stN;
    d += ",\"cloud\":1,\"conn\":";
    d += s_conn ? 1 : 0;
    d += ",\"open\":";
    d += s_open ? 1 : 0;
    d += ",";
    batteryJson(d);              // 1.19.0：外地也能看电量
    d += "}";
  } else if (!strcmp(path, "/api/step")) {
    apiStep((int)formLong(form, "dir", 1));
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/epis")) {
    // 1.9.0：有声书。dir=上一集/下一集，e=跳集，id=要哪一台的集表
    char tmp[16] = {0};
    if (formStr(form, "dir", tmp, sizeof(tmp))) {
      ok = apiEpisStep(atoi(tmp) >= 0 ? 1 : -1);
      d = ok ? "{\"ok\":1}" : "{\"ok\":0}";
    } else if (formStr(form, "e", tmp, sizeof(tmp))) {
      // 1.46.33：可选 id —— App 选集弹层不再先切台，点具体一集时才说台号
      char ti[16] = {0};
      const int st = formStr(form, "id", ti, sizeof(ti)) ? atoi(ti) : A.cur;
      ok = apiEpisJumpAt(st, atoi(tmp));
      d = ok ? "{\"ok\":1}" : "{\"ok\":0}";
    } else {
      int id = formStr(form, "id", tmp, sizeof(tmp)) ? atoi(tmp) : A.cur;
      // 1.18.0：集表按页取。跨网这条通道 CLOUD_BUF=16KB，1167 集整份发不出去，
      // 所以 apiEpisJson 按 EPIS_PAGE_BYTE 切页，from 由 App 带过来。
      char tf[16] = {0};
      int from = formStr(form, "from", tf, sizeof(tf)) ? atoi(tf) : 0;
      if (!apiEpisJson(id, d, from)) d = "{\"prog\":\"\",\"n\":0,\"cur\":-1,\"from\":0,\"list\":[]}";
    }
  } else if (!strcmp(path, "/api/play")) {
    ok = apiPlay((int)formLong(form, "id", -1));
    d = ok ? "{\"ok\":1}" : "{\"ok\":0}";
  } else if (!strcmp(path, "/api/pause")) {
    apiPauseToggle();
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/mute")) {
    // 1.43.0：静音功能已删 —— 回 ok 但不做事（老 App 不会因为 404 卡住）
    d = "{\"ok\":1,\"mute\":0}";
  } else if (!strcmp(path, "/api/vol")) {
    apiVol((int)formLong(form, "v", 50));
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/group")) {
    bool r;
    if (strstr(form, "dir=")) r = apiGroupShift((int)formLong(form, "dir", 1));
    else                      r = apiGroupSet((int)formLong(form, "g", 0));
    d = "{\"ok\":";
    d += r ? 1 : 0;
    d += ",\"group\":";
    d += A.group;
    d += "}";
  } else if (!strcmp(path, "/api/set")) {
    char k[16] = {0}, v[16] = {0};
    formStr(form, "k", k, sizeof(k));
    formStr(form, "v", v, sizeof(v));
    ok = apiSetKey(k, v);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"k\"}";
  } else if (!strcmp(path, "/api/fix")) {
    char a[24] = {0};
    formStr(form, "action", a, sizeof(a));
    ok = apiFix(a, &needReboot);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"action\"}";
    if (ok) d = String("{\"ok\":1,\"reboot\":") + (needReboot ? 1 : 0) + "}";
  } else if (!strcmp(path, "/api/history")) {
    // 1.46.33：以前恒回 0 —— 外地看收听记录永远是「今天 0 分钟」
    apiHistoryJson(d);
  } else if (!strcmp(path, "/api/diag")) {
    apiDiagJson(d);
  } else if (!strcmp(path, "/api/health")) {
    // 1.17.0：台的健康档案。b 里带 i= 就是单台详情
    apiHealthJson(d, (int)formLong(form, "i", -1));
  } else if (!strcmp(path, "/api/state")) {
    // 与 net.cpp 的 hState 同样的口径（设置页靠它回填音量/亮度/熄屏）
    d = "{\"cur\":";
    d += A.cur;
    d += ",\"count\":";
    d += g_stN;
    d += ",\"group\":";
    d += A.group;
    d += ",\"gcount\":";
    d += storeGroupCount();
    d += ",\"ps\":";
    d += (int)A.ps;
    d += ",\"vol\":";
    d += A.vol;
    d += ",\"mute\":";
    d += A.muted ? 1 : 0;
    d += ",\"bmax\":";
    d += A.brightMax;
    d += ",\"offmin\":";
    d += A.scrOffMin;
    d += ",\"on\":";
    d += A.screenOn ? 1 : 0;
    d += ",\"wifi\":";
    d += A.wifiOk ? 1 : 0;
    d += "}";
  } else if (!strcmp(path, "/api/batt")) {
    d = "{";
    batteryJson(d);
    d += "}";
  } else if (!strcmp(path, "/api/pair")) {
    d = "{\"locked\":0,\"code\":\"\"}";
  } else if (!strcmp(path, "/api/cloud")) {
    // 云端只允许「换遥控码」：关远程得在同一个 WiFi 里关，
    // 否则这边一关，响应就发不回去了
    if (strstr(form, "newcode=")) cloudNewCode();
    // 1.19.0：跨网改「免配对」开关（关掉立刻撤掉广播）
    if (strstr(form, "open=")) {
      const bool want = formLong(form, "open", 1) != 0;
      if (want != s_open) {
        s_open = want;
        saveCfg();
        pubPresence(!s_open);
        Serial.printf("[cloud] 免配对 %s\n", s_open ? "开" : "关");
      }
    }
    d = "{";
    cloudJson(d);
    d += "}";
  } else if (!strcmp(path, "/api/add")) {
    // 1.46.33：跨网也能加台（以前不认识这条路径 → App 弹「没加上」）
    char nm[64] = {0}, uu[200] = {0}, tg[8] = {0};
    formStr(form, "name", nm, sizeof(nm));
    formStr(form, "url", uu, sizeof(uu));
    formStr(form, "g", tg, sizeof(tg));
    if (!uu[0]) { ok = false; d = "{\"err\":\"url\"}"; }
    else {
      int nid = -1;
      ok = apiAdd(nm, uu, tg[0] ? atoi(tg) : 0, &nid);
      d = ok ? (String("{\"id\":") + nid + ",\"count\":" + g_stN + "}")
             : String("{\"err\":\"full\"}");
    }
  } else if (!strcmp(path, "/api/del")) {
    ok = apiDel((int)formLong(form, "id", -1));
    d = ok ? "{\"ok\":1}" : "{\"err\":\"id\"}";
  } else if (!strcmp(path, "/api/edit")) {
    char nm[64] = {0}, uu[200] = {0};
    formStr(form, "name", nm, sizeof(nm));
    formStr(form, "url", uu, sizeof(uu));
    ok = apiEdit((int)formLong(form, "id", -1), nm, uu);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"id\"}";
  } else if (!strcmp(path, "/api/move")) {
    ok = apiMove((int)formLong(form, "id", -1), (int)formLong(form, "to", 0),
                 (int)formLong(form, "dir", 1), strstr(form, "to=") != nullptr);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"id\"}";
  } else if (!strcmp(path, "/api/mvgroup")) {
    char gn[24] = {0};
    formStr(form, "gn", gn, sizeof(gn));
    ok = apiMvGroup((int)formLong(form, "id", -1), (int)formLong(form, "g", -1), gn);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"mv\"}";
  } else if (!strcmp(path, "/api/groupop")) {
    char op[12] = {0}, nm[24] = {0};
    formStr(form, "op", op, sizeof(op));
    formStr(form, "n", nm, sizeof(nm));
    ok = apiGroupOp(op, (int)formLong(form, "g", -1), nm);
    d = ok ? "{\"ok\":1}" : "{\"err\":\"group\"}";
  } else if (!strcmp(path, "/api/squelch")) {
    apiSquelch((int)formLong(form, "v", 0));
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/halfstep")) {
    apiHalfStep(formLong(form, "on", 0) != 0);
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/fmscan")) {
    // 收音芯片那头是异步扫的，这里只负责"开始"
    fmScanStart();
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/fmstations")) {
    d = "{\"n\":";
    d += fmScanCount();
    d += ",\"busy\":";
    d += fmScanBusy() ? 1 : 0;
    d += ",\"list\":[";
    for (int i = 0; i < fmScanCount(); i++) {
      if (i) d += ',';
      d += "{\"khz\":";
      d += fmScanFreq(i);
      d += ",\"rssi\":";
      d += fmScanRssi(i);
      d += "}";
    }
    d += "]}";
  } else if (!strcmp(path, "/api/scan")) {
    // 扫描是从**机器**那头发起的，跨网也能用（以前 App 一直显示「没扫到网络」）
    apiScanJson(d);
  } else if (!strcmp(path, "/api/wifi")) {
    // 1.46.33：远程给收音机换 WiFi。换完机器会短暂失联，这是预期。
    char ss[40] = {0}, pw[72] = {0};
    formStr(form, "ssid", ss, sizeof(ss));
    formStr(form, "pass", pw, sizeof(pw));
    if (!ss[0]) { ok = false; d = "{\"err\":\"ssid\"}"; }
    else {
      apiWifiSet(ss, pw);
      d = "{\"ok\":1,\"state\":1}";
    }
  } else if (!strcmp(path, "/api/wifi/list")) {
    apiWifiListJson(d);
  } else if (!strcmp(path, "/api/wifi/forget")) {
    char ss[40] = {0};
    formStr(form, "ssid", ss, sizeof(ss));
    // 1.46.36：按真实结果回话（1 删掉 / 0 本来没有 / -1 写盘失败）
    const int wr = apiWifiForget(ss);
    ok = (wr == 1);
    if (wr == 1)       d = "{\"ok\":1}";
    else if (wr == 0)  d = "{\"err\":\"notfound\"}";
    else               d = "{\"err\":\"write\"}";
  } else if (!strcmp(path, "/api/wifi/add")) {
    // 1.46.51：只存不连（跨网同样是这个语义）
    char ss[40] = {0}, pw[72] = {0};
    formStr(form, "ssid", ss, sizeof(ss));
    formStr(form, "pass", pw, sizeof(pw));
    const int ar = apiWifiAdd(ss, pw);
    ok = (ar == 1);
    if (ar == 1)       d = "{\"ok\":1}";
    else if (ar == -2) d = "{\"err\":\"full\"}";
    else if (ar == -1) d = "{\"err\":\"write\"}";
    else               d = "{\"err\":\"ssid\"}";
  } else if (!strcmp(path, "/api/wifi/use")) {
    // 1.46.51：用已存的那组连
    char ss[40] = {0};
    formStr(form, "ssid", ss, sizeof(ss));
    if (apiWifiUse(ss) == 1) d = "{\"ok\":1,\"state\":1}";
    else { ok = false; d = "{\"err\":\"notfound\"}"; }
  } else if (!strcmp(path, "/api/wifi/status")) {
    apiWifiStatusJson(d);
  } else if (!strcmp(path, "/api/epnext")) {
    // 1.46.33：下次调到这个台从第 X 集起（只写标记，不跳台不起播）
    const int st = (int)formLong(form, "id", -1);
    const int e  = (int)formLong(form, "e", -1);
    ok = apiEpNext(st, e);
    d = ok ? (String("{\"ok\":1,\"next\":") + e + "}") : String("{\"err\":\"notbook\"}");
  } else if (!strcmp(path, "/api/battcal")) {
    batteryCalibrate((int)formLong(form, "mv", 0));
    d = "{\"ok\":1}";
  } else if (!strcmp(path, "/api/auth")) {
    // 云端不靠令牌（靠遥控码），但 App 的通用流程会先要一次，给它一个占位
    d = "{\"token\":\"cloud\",\"locked\":0}";
  } else {
    ok = false;
    d = "{\"err\":\"path\"}";
  }

  o += ",\"s\":";
  o += ok ? 200 : 400;
  o += ",\"d\":";
  o += (d.length() ? d : String("{}"));
  o += "}";
  pub(o);

  // 1.46.33：重启/恢复出厂这类动作，回包先发出去再重启 ——
  //   以前 apiFix 只认 replay，跨网点「重启」直接回 err，App 就弹「没成功」。
  if (needReboot) {
    Serial.println("[cloud] 按远程指令重启");
    delay(1200);                  // 给 MQTT 把这条回包推出去
    ESP.restart();
  }
}

// ------------------------------------------------------------
//  MQTT 事件
// ------------------------------------------------------------
static void onMqtt(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg; (void)base;
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_conn = true;
      if (s_cli) esp_mqtt_client_subscribe(s_cli, s_tC.c_str(), 0);
      if (s_open) pubPresence(false);          // 1.19.0：上线就报一声


      Serial.printf("[cloud] 已连上 %s:%d，等命令\n", s_host, s_port);
      break;

    case MQTT_EVENT_DISCONNECTED:
      s_conn = false;
      Serial.println("[cloud] 断开了，自动重连");
      break;

    case MQTT_EVENT_DATA: {
      // 大消息（台表 9KB）会被拆成几片，这里按 offset 拼回去
      if (e->current_data_offset == 0) {
        s_rxGot = 0;
        s_rxTotal = e->total_data_len;
      }
      int n = e->data_len;
      if (s_rxGot + n > (int)sizeof(s_rx) - 1) n = (int)sizeof(s_rx) - 1 - s_rxGot;
      if (n > 0) {
        memcpy(s_rx + s_rxGot, e->data, n);
        s_rxGot += n;
      }
      if (s_rxTotal > 0 && s_rxGot >= s_rxTotal) {
        s_rx[s_rxGot] = 0;
        int c = s_rxGot;
        if (c > (int)sizeof(s_q) - 1) c = (int)sizeof(s_q) - 1;
        memcpy(s_q, s_rx, c);
        s_q[c] = 0;
        s_qFull = true;          // 交给主循环执行（不在这里碰音频/台表）
      }
      break;
    }

    default:
      break;
  }
}

// ------------------------------------------------------------
//  启动 / 停
// ------------------------------------------------------------
static void startClient() {
  if (s_cli) {
    esp_mqtt_client_stop(s_cli);
    esp_mqtt_client_destroy(s_cli);
    s_cli = nullptr;
    s_conn = false;
  }
  // 主题用 6 位短码：手机上要手抄，12 位太长。
  // 短码 16.7M 种组合 + 每条命令再校验 6 位遥控码，公共服务器上撞车/乱试都进不来。
  s_tC = String(CLOUD_PREFIX) + "/" + s_short + "/c";
  s_tR = String(CLOUD_PREFIX) + "/" + s_short + "/r";

  char uri[80];
  snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_host, s_port);

  esp_mqtt_client_config_t cfg = {};
  cfg.uri                = uri;
  cfg.keepalive          = CLOUD_KEEPALIVE;
  cfg.buffer_size        = CLOUD_BUF;
  cfg.out_buffer_size    = CLOUD_BUF;
  cfg.disable_auto_reconnect = false;
  cfg.reconnect_timeout_ms   = 5000;
  cfg.network_timeout_ms     = 8000;
  // 遗嘱：机器掉线时手机立刻能看到「离线」，不用等超时
  cfg.lwt_topic   = s_tR.c_str();
  cfg.lwt_msg     = "{\"off\":1}";
  cfg.lwt_msg_len = 9;
  cfg.lwt_qos     = 0;
  cfg.lwt_retain  = 0;

  s_cli = esp_mqtt_client_init(&cfg);
  if (!s_cli) {
    Serial.println("[cloud] mqtt init 失败");
    return;
  }
  esp_mqtt_client_register_event(s_cli, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onMqtt, nullptr);
  esp_mqtt_client_start(s_cli);
  s_started = true;
  Serial.printf("[cloud] 连 %s  主题 %s\n", uri, s_tC.c_str());
}

static void stopClient() {
  if (s_cli) {
    esp_mqtt_client_stop(s_cli);
    esp_mqtt_client_destroy(s_cli);
    s_cli = nullptr;
  }
  s_started = false;
  s_conn    = false;
}

// ------------------------------------------------------------
//  对外
// ------------------------------------------------------------
void cloudBegin() {
  makeDevId();
  loadCfg();
  if (!s_code[0]) {
    makeCode();
    saveCfg();
  }
  s_tC = String(CLOUD_PREFIX) + "/" + s_short + "/c";
  s_tR = String(CLOUD_PREFIX) + "/" + s_short + "/r";
  Serial.printf("[cloud] 设备码 %s / 短码 %s / 遥控码 %s / 服务器 %s:%d / %s\n",
                s_dev, s_short, s_code, s_host, s_port, s_on ? "开" : "关");
}

void cloudLoop() {
  if (!s_on) {
    if (s_started) stopClient();
    s_qFull = false;
    return;
  }
  // WiFi 连上才启动（配网阶段不会白连）
  if (!s_started && WiFi.status() == WL_CONNECTED) startClient();
  if (s_started && !s_cli) s_started = false;

  // 1.46.7：4 位连接码改了（App/网页里改的）→ 立刻重播一次广播。
  //   不重播的话，手机上还会拿旧码找人：明明改成了 5678，输 5678 却"找不到"，
  //   得等 30 秒那一轮才生效 —— 改完码当场试一次是必然动作，不能等。
  {
    static char s_pinSeen[8] = {0};
    const char* pc = pinCode();
    if (pc && strcmp(s_pinSeen, pc) != 0) {
      const bool first = (s_pinSeen[0] == 0);
      snprintf(s_pinSeen, sizeof(s_pinSeen), "%s", pc);
      if (!first && s_conn && s_open) pubPresence(false);
    }
  }

  // 在线广播：连上就每 30 秒报一次（retain 保证新连上来的 App 也能立刻看到）
  const uint32_t now = millis();
  if (s_conn && s_open && now - s_presAt > CLOUD_PRESENCE_MS) {
    s_presAt = now;
    pubPresence(false);
  }

  if (s_qFull) {
    static char local[CLOUD_MAX_CMD];
    memcpy(local, s_q, sizeof(s_q));
    s_qFull = false;
    runCmd(local);
  }
}

bool cloudOn()        { return s_on; }
bool cloudOpen()      { return s_open; }

// 给 App / 测试用的派生码（和 deriveFrom 同一个算法）
const char* cloudDerivedCode() {
  static char s_dc[8] = {0};
  deriveFrom(s_dev, s_dc, sizeof(s_dc));
  return s_dc;
}

bool cloudSetOpen(bool on) {
  if (s_open == on) return true;
  s_open = on;
  saveCfg();
  if (s_conn) pubPresence(!s_open);
  return true;
}
bool cloudConnected() { return s_conn; }
const char* cloudDevId()    { return s_dev; }
const char* cloudShortId()  { return s_short; }
const char* cloudCode()     { return s_code; }
const char* cloudHost()     { return s_host; }
int         cloudPort()     { return s_port; }

bool cloudSetOn(bool on) {
  // 1.42.0：**不允许关**。远程控制是「本来就有」的能力，不是可选项：
  //   关掉之后手机在外地就再也没有别的路能把它打开（只有在同一个网里才行），
  //   等于一个误触就把机器锁死。所以这里只接受「开」，关的请求当作重申开启。
  (void)on;
  if (!s_on) s_on = true;
  saveCfg();
  return true;
}

bool cloudSetCode(const char* c) {
  if (!c) return false;
  String s = c;
  s.trim();
  if (s.length() != 6) return false;
  for (unsigned i = 0; i < 6; i++) if (!isdigit((unsigned char)s[i])) return false;
  strncpy(s_code, s.c_str(), sizeof(s_code) - 1);
  s_code[6] = 0;
  saveCfg();
  return true;
}

void cloudNewCode() {
  makeCode();
  saveCfg();
}

bool cloudSetHost(const char* hostPort) {
  if (!hostPort) return false;
  String v = hostPort;
  v.trim();
  if (!v.length()) return false;
  int c = v.indexOf(':');
  String h = (c > 0) ? v.substring(0, c) : v;
  int    p = (c > 0) ? v.substring(c + 1).toInt() : CLOUD_PORT;
  if (!h.length() || h.length() >= (int)sizeof(s_host)) return false;
  if (p <= 0 || p > 65535) p = CLOUD_PORT;
  strncpy(s_host, h.c_str(), sizeof(s_host) - 1);
  s_host[sizeof(s_host) - 1] = 0;
  s_port = p;
  saveCfg();
  if (s_started) startClient();       // 换了服务器立刻重连
  return true;
}

void cloudJson(String& o) {
  o += "\"on\":";      o += s_on ? 1 : 0;
  o += ",\"conn\":";   o += s_conn ? 1 : 0;
  o += ",\"devid\":\""; o += s_dev; o += '"';
  o += ",\"short\":\""; o += s_short; o += '"';
  o += ",\"code\":\"";  o += s_code;  o += '"';
  o += ",\"host\":\"";  o += s_host;  o += '"';
  o += ",\"port\":";    o += s_port;
  o += ",\"open\":";    o += s_open ? 1 : 0;
  o += ",\"pin\":\"";   o += pinCode(); o += '"';    // 1.46.7：跨网输码用的那 4 位
  o += ",\"pres\":";    o += (unsigned long)s_presN;
  o += ",\"cmds\":";    o += s_cmdCount;
  o += ",\"bad\":";     o += s_badCount;
}
