// ============================================================
//  api.cpp —— 动作层（局域网与跨网共用）
//  语义一律对齐 1.7.0 的 net.cpp，别在这里发明新行为。
// ============================================================
#include "api.h"
#include "config.h"
#include "battery.h"
#include "state.h"
#include "store.h"
#include "audio.h"
#include "ui.h"
#include "input.h"
#include "album.h"
#include "epis.h"
#include "fm.h"
#include "net.h"
#include "health.h"
#include "offair.h"      // 1.46.6：台表现在不播
#include "history.h"      // 1.46.33：apiFix 要能清空收听记录

#include <WiFi.h>
#include <LittleFS.h>
#include <ESP.h>
#include <time.h>
#include <string.h>      // 1.46.1：apiStationsJson 算 reserve 用

// ------------------------------------------------------------
//  小工具（与 net.cpp 里的 kv / jsonEsc 行为一致）
// ------------------------------------------------------------
static void kv(String& o, const char* k, const char* v) {
  o += '"'; o += k; o += "\":\"";
  const char* p = v ? v : "";
  while (*p) {
    char c = *p++;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n')        { o += "\\n"; }
    else if ((unsigned char)c < 0x20) { /* 丢掉控制字符 */ }
    else                       { o += c; }
  }
  o += '"';
}

// 与 net.cpp 的 tipText() 一致：App 状态下那行小字
static const char* tipText() {
  if (!A.wifiOk) return "等下我重连";
  if (A.ps == PS_ERR) return "换个台试试";
  if (A.ps == PS_BUFF) return "稍等缓冲";
  return "旋钮换台";            // 1.15.0：不再有「左右换组」这句
}

// ------------------------------------------------------------
//  屏幕快照 —— /api/screen 的同一份 JSON
// ------------------------------------------------------------
void apiScreenJson(String& o) {
  int idx = (A.preview >= 0) ? A.preview : A.cur;
  const char* nm = (idx >= 0 && idx < g_stN) ? g_st[idx].name : "没有电台";
  char clk[8] = "--:--";
  netClockStr(clk, sizeof(clk));
  struct tm t;
  int wd = getLocalTime(&t, 0) ? t.tm_wday : -1;

  o = "{\"on\":";
  o += A.screenOn ? 1 : 0;
  o += ",\"br\":";
  o += A.bright;
  o += ',';
  kv(o, "clock", clk);
  o += ",\"wd\":";
  o += wd;
  o += ",\"grp\":";
  o += A.group;
  o += ',';
  // 1.15.0：gname 不再是「分组名」，而是**当前这台所属的标签**（屏上顶栏那一小块）
  kv(o, "gname", storeTag(idx));
  o += ",\"idx\":";
  o += idx;
  o += ",\"cnt\":";
  o += g_stN;
  o += ',';
  kv(o, "name", nm);
  o += ",\"ps\":";
  o += (int)A.ps;
  o += ",\"vol\":";
  // 1.22.0：这里的 vol 给的是**当前这一路**的值（收音/网络各一套）。
  //   老 App 只认 vol，行为不变：在收音模式下它显示的就是收音音量，
  //   这比原来「收音时给网络音量」更符合直觉。具体是哪一路看 mode 字段。
  o += A.fmMode ? A.fmVol : A.vol;
  o += ",\"nvol\":";          // 网络那一路（无论当前模式，都给真值）
  o += A.vol;
  o += ",\"fvol\":";          // 收音那一路
  o += A.fmVol;
  o += ",\"sq\":";            // 静噪门限（0=关）
  o += A.squelch;
  o += ",\"hs\":";            // 半步进
  o += A.halfstep ? 1 : 0;
  o += ",\"fsq\":";           // 当前是否正被静噪压着
  o += fmSquelched() ? 1 : 0;
  o += ",\"mute\":";
  o += A.muted ? 1 : 0;
  o += ",\"wifi\":";
  o += A.wifiOk ? 1 : 0;
  o += ",\"rssi\":";
  o += A.rssi;
  o += ",\"mode\":\"";
  o += A.fmMode ? "fm" : "net";
  o += "\"";
  o += ",\"fmfreq\":";
  // 老字段（1.5.0~1.9.0）：只有调频时有意义，单位 100kHz，老 App 照旧能用
  o += (A.fmMode && radioBand(A.fmFreq) == RADIO_FM) ? (A.fmFreq / 10) : 0;
  o += ",\"fmrssi\":";
  o += A.fmRssi;
  // 1.10.0：一个 kHz 频率说清全部波段（长波/中波/调频/短波）
  o += ",\"khz\":";
  o += A.fmFreq;
  o += ",\"band\":\"";
  o += radioBandCode(A.fmFreq);
  o += "\"";
  o += ",\"pv\":";
  o += A.preview;
  o += ",\"learn\":";
  o += inputLearnActive() ? 1 : 0;
  o += ",\"pc\":";
  o += uiPairCodeActive() ? 1 : 0;
  o += ',';
  kv(o, "tip", uiPairCodeActive() ? "屏幕正在显示配对码" : tipText());
  o += ",\"mu\":";
  o += A.muted ? 1 : 0;

  // 1.9.0：有声书的集（App 靠这三项画「第 3 集 / 共 15 集」和上一集/下一集）
  const int bk = episBookOfStation(idx);
  o += ",\"epn\":";
  o += (bk >= 0) ? episCount(bk) : 0;
  o += ",\"epi\":";
  o += (bk >= 0) ? episCur(bk) : -1;
  // 1.46.33：App 设的「下次从第 X 集起」（-1 = 没设），设置页拿它回填
  o += ",\"epnext\":";
  o += (bk >= 0) ? episNext(bk) : -1;
  // 1.46.38：这一集**听到第几秒**（换台回来会从这里接着放，见 resume.cpp）。
  //   有它 App 就能显示「第 5 集 3分20秒」，不用自己拿开关机的毫秒数去猜。
  o += ",\"sec\":";
  o += (bk >= 0) ? (long)audioUptimeSec() : -1;
  o += ',';
  kv(o, "ept", (bk >= 0) ? episTitle(bk, episCur(bk)) : "");
  o += ',';
  kv(o, "prog", (bk >= 0) ? episBookName(bk) : "");
  o += ',';
  // 1.38.2：把电量也带上 —— App 主页那块模拟屏要画左上角的电量条，
  //   原来只有 /api/state 有，主页每 0.9 秒还得多请求一次，没必要。
  batteryJson(o);
  o += "}";
}

// ------------------------------------------------------------
//  整张台表 + 标签清单 —— /api/stations 的同一份 JSON
//
//  1.15.0：不再是「当前组的台 + 分组清单」，而是**整表**（286 台）
//  加上「标签清单」。App 点标签只是**本地筛选**，不再叫机器切组。
// ------------------------------------------------------------
void apiStationsJson(String& o) {
  // 1.46.1：先算长度、一次性 reserve，再往里拼。
  //   为什么非加不可（台表上限 380 -> 500 之后）：ESP32 的 String 是 std::string，
  //   容量不够时按几何增长扩容，拼接过程中峰值会到最终长度的 ~2 倍。
  //   500 台按出厂表均值约 40 KB、若大量是长地址（有声书那种 233 字节直链）能到 150 KB+，
  //   而播放 https 期间内部空闲只有 165 KB 上下 —— 2 倍峰值一抖就 malloc 失败，
  //   于是 /api/stations 返回空串：App 电台页整页空白，还不报错，最难查。
  //   reserve 一次到位后峰值 = 最终长度，不再翻倍。
  {
    size_t need = 64;                                  // 外层 {"count":..,"tags":[..]}
    for (int t = 0; t < storeTagCount(); t++)
      need += strlen(storeTagName(t)) + 48;
    for (int i = 0; i < g_stN; i++)
      need += strlen(g_st[i].name) + strlen(g_st[i].url) + 64;   // 每台固定键名约 42 B，留余量
    o.reserve(o.length() + need);
  }
  o = "{\"count\":";
  o += g_stN;
  o += ",\"tags\":[";
  for (int t = 0; t < storeTagCount(); t++) {
    if (t) o += ',';
    o += "{\"i\":";
    o += t;
    o += ',';
    kv(o, "n", storeTagName(t));
    o += ",\"c\":";
    o += storeTagStations(t);
    o += "}";
  }
  o += "],\"list\":[";
  for (int i = 0; i < g_stN; i++) {
    if (i) o += ',';
    o += "{\"i\":";
    o += i;
    o += ',';
    kv(o, "n", g_st[i].name);
    o += ',';
    kv(o, "u", g_st[i].url);
    o += ",\"q\":";
    o += storeIsQtfm(i) ? 1 : 0;
    // 1.9.0：这一台是有声书就报「共几集」，App 列表里显示「共 15 集」，0 = 普通电台
    o += ",\"ep\":";
    o += episCount(episBookOfName(g_st[i].name));
    // 1.15.0：标签下标（App 的 chips 按它筛选）
    o += ",\"t\":";
    o += storeTagOf(i);
    // 1.46.6：这台正被「停播台账」藏着（App 想灰显就用它；不读也没影响）
    if (offairSkip(i)) o += ",\"off\":1";
    o += "}";
  }
  o += "]}";
}

// ------------------------------------------------------------
//  动作
// ------------------------------------------------------------
bool apiStep(int dir) {
  // ---- 1.41.0 修 P0：收音模式（SI4732）下这三个动作原来全打空 ----
  //
  //  App 的「上一台 / 暂停 / 下一台」一直只调 audioNext/audioPauseToggle，
  //  而那两套只动网络流（ESP32-audioI2S）。机器在收音模式时 FM 的声音是从
  //  SI4732 的耳机口直连功放的，ESP32 根本没参与 ⇒ 无论按哪个都没反应。
  //  用户原话：「需要 App 真实可操作收音机」—— 这一条不修，收音模式下 App 就是个摆设。
  //
  //  收音模式下：换台 = 在搜到的台表里走一格（fmScanStep），
  //  和旋钮的判据完全一致（1.38.0 起旋钮也是一格一个搜到的台）。
  if (A.fmMode) {
    // 1.46.59：旋钮改成手动调频之后，App 的「上一台/下一台」保留"跳到下一个
    //   搜到的台"（手机上没法一格一格挪）；台表是空/只有一个的时候退回
    //   ±0.5MHz 的频率步进，不然这个按钮就完全没反应。
    const int f0 = fmFreq();
    int f;
    if (fmScanCount() > 1) f = fmScanStep(f0, dir >= 0 ? 1 : -1);
    else                   f = radioWalk(f0, dir >= 0 ? 1 : -1, 5);
    if (f != fmFreq()) fmTune(f);
    A.fmFreq = f;
    A.fmRssi = fmRssi();
    A.dirty  = true;
    Serial.printf("[api] 收音换台 %+d -> %d kHz\n", dir, f);
    actTouch();
    return true;
  }
  audioNext(dir >= 0 ? 1 : -1);
  actTouch();
  return true;
}

bool apiPlay(int id) {
  if (id < 0 || id >= g_stN) return false;
  audioPlayIndex(id);
  actTouch();
  return true;
}

bool apiPauseToggle() {
  // 1.43.0：收音模式没有「暂停」，而静音功能也已经删了 ⇒ 这里什么都不做。
  //   返回 true 让 App 不报错（它只是按下去了，机器确实没东西可做）。
  if (A.fmMode) { actTouch(); return true; }
  audioPauseToggle();
  actTouch();
  return true;
}

bool apiMuteToggle() {
  // 1.43.0：静音功能整体删掉（用户要求）。
  //   为什么连"收音模式的芯片静音"也一起删：那正是「按了没声音、还查不出谁干的」
  //   的最大嫌疑 —— 一个开关管两条声道（芯片寄存器 + 音频库），
  //   任何一边没对上，用户就只觉得"机器哑了"。
  //   现在宁可没有静音，也不留一个能把声音弄没的入口。
  actTouch();
  return true;
}

bool apiVol(int v) {
  // 1.22.0：audioSetVol 内部按当前模式分流，这里不用判断
  // 1.44.0：上下限也交给它 —— 两路量程不同，这里夹一次会把 FM 的值夹坏
  audioSetVol(v);
  actTouch();
  return true;
}

// 1.22.0：静噪门限（0 = 关；1..100 = 越容易静音）
bool apiSquelch(int v) {
  A.squelch = constrain(v, 0, 100);
  A.dirty = true;
  storeSaveDebounced();
  Serial.printf("[api] 静噪门限 = %d\n", A.squelch);
  actTouch();
  return true;
}

// 1.22.0：半步进
bool apiHalfStep(bool on) {
  A.halfstep = on;
  A.dirty = true;
  storeSaveDebounced();
  Serial.printf("[api] 半步进 %s\n", on ? "开" : "关");
  actTouch();
  return true;
}

// 1.15.0：不再分组 —— 这两个动作已经不存在，恒返回 false。
// 老 App / 老远程页还会发 /api/group，收到 false 只会静默不响应，不会出错。
bool apiGroupSet(int) {
  return false;
}

bool apiGroupShift(int) {
  return false;
}

// ------------------------------------------------------------
//  有声书集（1.9.0）
// ------------------------------------------------------------
bool apiEpisStep(int dir) {
  const int bk = episBookOfStation(A.cur);
  if (bk < 0 || episCount(bk) < 2) return false;
  episSetCur(bk, episStep(bk, dir >= 0 ? 1 : -1));
  audioPlayIndex(A.cur);          // 同一个台，换成那一集
  actTouch();
  return true;
}

bool apiEpisJump(int e) {
  return apiEpisJumpAt(A.cur, e);
}

// 1.46.33：允许带台号 —— App 的「选集」弹层现在不会为了打开它先把机器切过去，
//   所以点某一集的时候才第一次告诉机器「是哪一台的哪一集」。
bool apiEpisJumpAt(int st, int e) {
  const int bk = episBookOfStation(st);
  if (bk < 0) return false;
  episSetCur(bk, e);
  episClearNext(bk);              // 手动点了具体集数，「下次从第 X 集」就没意义了
  // 同一台只换集：audioPlayIndex 会重新连这一集（会有一两秒空档，这是换流的物理代价）；
  // 不是当前台才真的换台。
  audioPlayIndex(st);
  actTouch();
  return true;
}

// 「下次调到这个台时，从第 e 集起」（e<0 = 取消这个设置）
// !! 这里**绝不能**调 audioPlayIndex / episSetCur —— 用户明确要求：
//    设在哪儿就设在哪儿，机器不跳台、不起播、也不动他听到的进度。
bool apiEpNext(int st, int e) {
  const int bk = episBookOfStation(st);
  if (bk < 0) return false;
  if (e < 0) episClearNext(bk);
  else       episSetNext(bk, e);
  Serial.printf("[epis] 台 %d 下次从第 %d 集起（%s）\n", st, e + 1, e < 0 ? "已取消" : "已设");
  return true;
}

bool apiEpisJson(int stIdx, String& o, int from, int budget) {
  const int bk = episBookOfStation(stIdx);
  if (bk < 0) return false;
  const int n = episCount(bk);
  if (from < 0) from = 0;
  if (from > n) from = n;                       // 越界就回空页（App 那边按 n 纠回来）
  const int bud = budget > 0 ? budget : EPIS_PAGE_BYTE;

  o = "{";
  kv(o, "prog", episBookName(bk));
  o += ",\"n\":";
  o += n;
  o += ",\"cur\":";
  o += episCur(bk);
  o += ",\"from\":";
  o += from;
  o += ",\"list\":[";

  int e = from;
  for (; e < n; e++) {
    const int mark = o.length();
    if (e > from) o += ',';
    o += "{\"i\":";
    o += e;
    o += ',';
    kv(o, "t", episTitle(bk, e));
    o += "}";
    // 至少给 1 集：第一集无论多长都留下，之后按字节预算收手
    if (o.length() > bud && e > from) { o.remove(mark); break; }
  }
  o += "]}";
  return true;
}

// ---- 1.46.33：台表的增删改，跨网那条通道以前根本没有 ----
//   用户报的「删除失败 / 添加流地址失败」多半就是远程控制时走到这儿：路径不认识，
//   云端回 400，App 只能弹「删除失败」。局域网和跨网必须一模一样。
bool apiAdd(const char* name, const char* url, int tag, int* outId) {
  if (!storeUrlOk(url)) return false;
  const int id = storeAdd(name, url, tag);
  if (id < 0) return false;
  if (outId) *outId = id;
  return true;
}

bool apiDel(int id) {
  if (id < 0 || id >= g_stN) return false;
  const bool wasCur = (id == A.cur);
  if (!storeDel(id)) return false;
  if (g_stN == 0) audioStop();
  else if (wasCur) audioPlayIndex(A.cur);      // 删的是在播的台，接着放下一台
  A.dirty = true;
  actTouch();
  return true;
}

bool apiEdit(int id, const char* name, const char* url) {
  if (url && *url && !storeUrlOk(url)) return false;
  const bool ok = storeEdit(id, (name && *name) ? name : nullptr,
                                (url && *url) ? url : nullptr);
  if (ok) A.dirty = true;
  return ok;
}

bool apiMove(int id, int to, int dir, bool hasTo) {
  const bool ok = hasTo ? storeMoveTo(id, to) : storeMove(id, dir >= 0 ? 1 : -1);
  A.dirty = true;
  return ok;
}

bool apiMvGroup(int id, int g, const char* gn) {
  const bool ok = storeMoveToGroup(id, g, (gn && *gn) ? gn : nullptr);
  A.dirty = true;
  return ok;
}

bool apiGroupOp(const char* op, int g, const char* name) {
  String o = op ? op : "";
  String n = name ? name : "";
  bool ok = false;
  if      (o == "add")    ok = storeGroupAdd(n.c_str());
  else if (o == "rename") ok = storeGroupRename(g, n.c_str());
  else if (o == "del")    ok = storeGroupDel(g);
  else if (o == "move")   ok = storeGroupMove(g, 1);
  A.dirty = true;
  return ok;
}

// 1.46.33：收听记录 JSON 挪到这一层。
//   以前只有局域网 hHistory 会拼，跨网直接回 {"today":0,...} —— App 从外地看
//   永远是「今天 0 分钟」，用户说的「收听记录不准」有一半是这个。
void apiHistoryJson(String& o) {
  o = "{\"total\":";
  o += historyTotalSec();
  o += ",\"today\":";
  o += historyTodaySec();
  o += ",\"plays\":";
  o += historyTodayPlays();
  o += ",\"days\":[";
  for (int i = 0; i < historyDayCount(); i++) {
    const DayStat* d = historyDayAt(i);
    if (!d) continue;
    if (i) o += ',';
    o += "{\"d\":\"";
    o += (int)(d->key / 10000);
    o += '-';
    int m = (d->key / 100) % 100;
    if (m < 10) o += '0';
    o += m;
    o += '-';
    int dd = d->key % 100;
    if (dd < 10) o += '0';
    o += dd;
    o += "\",\"s\":";
    o += d->sec;
    o += ",\"p\":";
    o += d->plays;
    o += "}";
  }
  o += "],\"list\":[";
  for (int i = 0; i < historyCount(); i++) {
    const HistItem* h = historyAt(i);
    if (!h) continue;
    if (i) o += ',';
    o += "{\"n\":\"";
    {
      const char* p = h->name;
      while (*p) {
        char c = *p++;
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c >= 0x20) o += c;
      }
    }
    o += "\",\"g\":\"";
    {
      const char* p = h->grp;
      while (*p) {
        char c = *p++;
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c >= 0x20) o += c;
      }
    }
    o += "\",\"s\":";
    o += h->sec;
    o += ",\"last\":";
    o += h->last;
    o += ",\"p\":";
    o += h->plays;
    o += "}";
  }
  o += "]}";
}

bool apiSetKey(const char* k, const char* v) {
  String key = k ? k : "";
  String val = v ? v : "";
  if (key == "bright") {
    A.brightMax = constrain(val.toInt(), 5, 100);
    A.bright = A.brightMax;
    A.lastAct = millis();
  } else if (key == "offmin") {
    A.scrOffMin = constrain(val.toInt(), 0, 60);
  } else if (key == "screen") {
    if (val == "0") { A.screenOn = false; A.dirty = true; }
    else            { A.lastAct = millis(); if (!A.screenOn) { A.screenOn = true; A.dirty = true; } }
  } else {
    return false;
  }
  storeSaveCfg();
  return true;
}

// 1.46.33：以前这里只有 replay —— 跨网时「重启收音机」直接回 err，
//   App 弹「没成功」。现在和局域网 hFix 的动作表对齐（见 net.cpp hFix）。
//   reboot=true 的动作不能在本函数里立刻重启：调用方（cloud.cpp）还要先把
//   回包发出去。所以只置 *reboot，由调用方发完再重启。
bool apiFix(const char* action, bool* reboot) {
  String a = action ? action : "";
  if (reboot) *reboot = false;

  if (a == "replay") {
    audioReplay();
    return true;
  }
  if (a == "reconnect") {
    // 1.46.35：原来当场断链 —— MQTT 的回包同样发不出去，App 只能显示「没成功」。
    //   现在排进同一个队列（netLoop 700ms 后执行），回包先走。
    netReconnectSoon();
    return true;
  }
  if (a == "reset_audio") {
    audioStop();
    delay(120);
    audioReplay();
    return true;
  }
  if (a == "fix_table") {
    storeLoad();
    if (g_stN > 0) { storeSave(); return true; }
    return false;                       // 一台都读不出来时别 storeSave（会写成空表）
  }
  if (a == "clear_history") {
    historyClear();
    historySaveNow();
    return true;
  }
  if (a == "reset_stations") {
    LittleFS.remove("/stations.tsv");
    LittleFS.remove("/tags.txt");
    if (reboot) *reboot = true;
    return true;
  }
  if (a == "forget_wifi") {
    LittleFS.remove("/wifi.txt");
    if (reboot) *reboot = true;
    return true;
  }
  if (a == "reboot") {
    if (reboot) *reboot = true;
    return true;
  }
  if (a == "factory") {
    LittleFS.remove("/stations.tsv");
    LittleFS.remove("/history.tsv");
    LittleFS.remove("/cfg.txt");
    LittleFS.remove("/token.txt");
    LittleFS.remove("/pair.txt");
    if (reboot) *reboot = true;
    return true;
  }
  return false;
}

// ------------------------------------------------------------
//  体检（跨网版）
//  局域网那边 /api/diag 的字段更多（复位原因、音频地址等），
//  这边只给 App 体检页真正会读的那几项，够用又不占流量。
// ------------------------------------------------------------
// 1.17.0：台的健康档案（/api/health）。逻辑全在 health.cpp，
// 这里只做转发，保证局域网和跨网回的是同一份 JSON。
void apiHealthJson(String& o, int one) { healthJson(o, one); }

void apiDiagJson(String& o) {
  o = "{\"fw\":\"" FW_VER "\",\"up\":";
  o += millis() / 1000;
  o += ",\"heap\":";
  o += ESP.getFreeHeap();
  o += ",\"fsUsed\":";
  o += (unsigned long)LittleFS.usedBytes();
  o += ",\"fsTotal\":";
  o += (unsigned long)LittleFS.totalBytes();
  o += ",\"wifi\":{\"ok\":";
  o += A.wifiOk ? 1 : 0;
  o += ",\"ssid\":\"";
  o += WiFi.SSID();
  o += "\",\"ip\":\"";
  o += WiFi.localIP().toString();
  o += "\",\"rssi\":";
  o += A.rssi;
  o += ",\"ch\":";
  o += WiFi.channel();
  o += "}";
  o += ",\"audio\":{\"ps\":";
  o += (int)A.ps;
  o += ",\"retry\":";
  o += audioRetry();
  o += ",\"dead\":";
  o += audioDeadLinks();
  o += ",\"kbps\":";
  o += audioBitrate();
  o += ",\"sec\":";
  o += (long)audioUptimeSec();
  o += ",\"url\":\"";
  o += audioUrl();
  o += "\"}";
  o += ",\"table\":{\"tags\":";
  o += storeTagCount();
  o += ",\"count\":";
  o += g_stN;
  o += "}";
  o += ",\"time\":{\"ok\":";
  o += 0;
  o += ",\"now\":\"远程\"}";
  o += ",\"token\":{\"set\":1,\"fp\":\"----\"}";
  o += ",\"pair\":{\"locked\":0}";
  o += "}";
}
