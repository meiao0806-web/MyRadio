// ============================================================
//  pairpin.cpp —— 4 位「手机连接码」（1.46.5）
//  协议见 pairpin.h 顶部注释。这里只做三件事：
//    ① 存/读 /pin.txt
//    ② UDP 8137 上「问 → 答」
//    ③ 屏上显示一次（复用 uiPairCode 那页）
// ============================================================
#include "pairpin.h"
#include "config.h"
#include "ui.h"
#include "cloud.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>

static const char* F_PIN = "/pin.txt";

static char     s_pin[8]   = PIN_DEFAULT;
static WiFiUDP  s_udp;
static bool     s_udpOn    = false;
static uint32_t s_winAt    = 0;      // 限流窗口
static int      s_winN     = 0;

static bool isFour(const char* c) {
  if (!c) return false;
  if (strlen(c) != 4) return false;
  for (int i = 0; i < 4; i++) if (c[i] < '0' || c[i] > '9') return false;
  return true;
}

void pinBegin() {
  s_pin[0] = 0;
  File f = LittleFS.open(F_PIN, "r");
  if (f) {
    String s = f.readString();
    f.close();
    s.trim();
    if (isFour(s.c_str())) strncpy(s_pin, s.c_str(), sizeof(s_pin) - 1);
  }
  if (!s_pin[0]) {
    strncpy(s_pin, PIN_DEFAULT, sizeof(s_pin) - 1);
    s_pin[sizeof(s_pin) - 1] = 0;
    File w = LittleFS.open(F_PIN, "w");
    if (w) { w.print(s_pin); w.close(); }
  }
  Serial.printf("[pin] 手机连接码 %s（UDP %d）\n", s_pin, PIN_UDP_PORT);
}

const char* pinCode() { return s_pin; }

bool pinSetCode(const char* c) {
  if (!isFour(c)) return false;
  strncpy(s_pin, c, sizeof(s_pin) - 1);
  s_pin[sizeof(s_pin) - 1] = 0;
  File w = LittleFS.open(F_PIN, "w");
  if (w) { w.print(s_pin); w.close(); }
  Serial.printf("[pin] 改成 %s\n", s_pin);
  return true;
}

bool pinMatch(const char* c) { return c && strcmp(c, s_pin) == 0; }

void pinShow() { uiPairCode(pinCode()); }

// ------------------------------------------------------------ UDP 应答
//
//  只在 WiFi 连着的时候开 socket：换网 / 重连后本地 IP 变了，
//  socket 要跟着重开，否则「能上网但手机问不到」这种问题很难查。
void pinLoop() {
  // AP 模式下也要答话（1.46.5 补丁）：手机刚连上收音机热点、还没配网时，
  // 正是「输 1234 就连」最该好使的场合 —— 那时 WiFi.status() 不是 CONNECTED。
  const bool sta = (WiFi.status() == WL_CONNECTED);
  const bool ap  = (WiFi.getMode() & WIFI_AP) != 0;
  if (!sta && !ap) {
    if (s_udpOn) { s_udp.stop(); s_udpOn = false; }
    return;
  }
  if (!s_udpOn) {
    if (!s_udp.begin(PIN_UDP_PORT)) return;
    s_udpOn = true;
  }
  const int n = s_udp.parsePacket();
  if (n <= 0) return;

  char buf[80];
  int len = s_udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = 0;

  // 只认自己的探针，别把别人家的广播当成命令
  if (strncmp(buf, "MR1?", 4) != 0) return;

  const uint32_t now = millis();
  if (now - s_winAt > 60000) { s_winAt = now; s_winN = 0; }
  if (++s_winN > 60) return;              // 一分钟最多答 60 次，挡住乱猜

  const bool ok = pinMatch(buf + 4);
  char out[112];
  if (ok) {
    IPAddress ip = sta ? WiFi.localIP() : WiFi.softAPIP();
    snprintf(out, sizeof(out), "MR1|1|%s|%s|%u.%u.%u.%u|%s",
             FW_NAME, FW_VER, ip[0], ip[1], ip[2], ip[3], cloudDevId());
  } else {
    snprintf(out, sizeof(out), "MR1|0");
  }
  s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
  s_udp.write((const uint8_t*)out, strlen(out));
  s_udp.endPacket();
}
