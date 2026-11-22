// m4aseek.cpp —— 1.46.52：取 m4a 文件头，算出「第 N 秒」的字节位置
//
// 一次额外 HTTP 请求（Range: bytes=0-CAP），把 moov 读进来，顺着
//   moov -> trak -> mdia -> mdhd(timescale) / minf -> stbl -> stsz
// 找到时间基准和每帧字节数，然后
//   帧号   = 秒 × timescale ÷ 1024
//   字节位 = mdat 负载起点 + Σ stsz[0 .. 帧号-1]
// 这两步是**精确**的（不是按码率估），所以续播能落到同一秒。
//
// 实测（tools/probe_m4a.py，2026-11-04）：
//   三国演义   moov 90267 B  timescale 44100  stsz 20340 帧  帧长 371~480
//   水浒传     moov 63443 B  timescale 44100  stsz 14200 帧
//   官场现形记 moov 33161 B  timescale 44100
//   原来是这样 moov 22137 B  timescale 22050（蜻蜓）
// 都在 120KB 以内 —— 取一次 256KB 足够，且 mdat 一定在 moov 之后。
#include "m4aseek.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "evlog.h"

// 读多少字节：分两档试。
//   实测 moov 大小（tools/verify_m4aseek.py）：
//     三国演义 90267 / 水浒传 63443 / 官场现形记 33161 / 原来是这样 22137 / 如临大敌 120015
//   160KB 能覆盖全部这些（含 mdat 头），绝大多数一次就够；
//   万一遇到超大 moov（每小时级的长音频），再补一次到 384KB。
//   为什么不一上来就 384KB：多读 224KB 在 2.4G 上要多花 0.3~0.6 秒，而这只影响
//   「换台回来」那一下的等待，能省就省。
#define M4A_SEEK_CAP1  (160 * 1024)
#define M4A_SEEK_CAP2  (384 * 1024)
// 单次连接最多跟几次重定向（喜马/蜻蜓都可能 302）
#define M4A_SEEK_HOPS  3

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------
static inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// URL 拆解：支持 http/https，默认端口 80/443
static bool urlSplit(const char* url, bool* ssl, String* host, uint16_t* port, String* path) {
  const char* p = url;
  if (strncmp(p, "https://", 8) == 0)      { *ssl = true;  *port = 443; p += 8; }
  else if (strncmp(p, "http://", 7) == 0)  { *ssl = false; *port = 80;  p += 7; }
  else return false;

  const char* slash = strchr(p, '/');
  if (!slash) return false;

  String hp(p, (size_t)(slash - p));
  int colon = hp.indexOf(':');
  if (colon > 0) {
    *port = (uint16_t)hp.substring(colon + 1).toInt();
    hp = hp.substring(0, colon);
  }
  *host = hp;
  *path = String(slash);
  return host->length() > 0 && path->length() > 0;
}

// ------------------------------------------------------------
//  box 遍历
//
//  [0..3] size  [4..7] type
//  size == 1 表示 64 位长度（紧跟 8 字节）—— 有声书里不会出现，直接放弃
//  size == 0 表示「一直到容器末尾」
// ------------------------------------------------------------
struct BoxRef {
  uint32_t off;    // box 起点
  uint32_t sz;     // box 总长
  uint32_t body;   // 负载起点（off + 8）
};

static bool findChild(const uint8_t* b, uint32_t begin, uint32_t end,
                      const char* name, BoxRef* out) {
  uint32_t p = begin;
  while (p + 8 <= end) {
    const uint32_t sz = be32(b + p);
    if (sz == 1) return false;                 // 64 位长度：不支持
    const uint32_t real = (sz == 0) ? (end - p) : sz;
    if (real < 8 || p + real > end) return false;   // 越界/坏数据，别硬来
    if (memcmp(b + p + 4, name, 4) == 0) {
      out->off  = p;
      out->sz   = real;
      out->body = p + 8;
      return true;
    }
    p += real;
  }
  return false;
}

// mdhd 里的 timescale：v0 在 body+16，v1 在 body+24
//   body = size(4) + type(4) + version/flags(4) 之后
//   v0: creation(4) modification(4) timescale(4) ...
//   v1: creation(8) modification(8) timescale(4) ...
static uint32_t mdhdTimescale(const uint8_t* b, const BoxRef& mdhd) {
  if (mdhd.body + 4 > mdhd.off + mdhd.sz) return 0;
  const uint8_t version = b[mdhd.body];
  const uint32_t t = (version == 1) ? mdhd.body + 4 + 16 : mdhd.body + 4 + 8;
  if (t + 4 > mdhd.off + mdhd.sz) return 0;
  return be32(b + t);
}

// 累加 stsz 前 frames 帧的字节数
//   stsz 负载：version/flags(4) sample_size(4) count(4) entries[](4 each)
//   sample_size != 0 时所有帧一样大，不用读数组
static bool stszSum(const uint8_t* b, const uint32_t n, const BoxRef& stsz,
                    uint32_t frames, uint32_t* sum, uint32_t* sampleSize, uint32_t* totalFrames) {
  if (stsz.body + 12 > stsz.off + stsz.sz) return false;
  const uint32_t ss    = be32(b + stsz.body + 4);
  const uint32_t count = be32(b + stsz.body + 8);
  if (sampleSize)  *sampleSize  = ss;
  if (totalFrames) *totalFrames = count;
  if (count == 0) return false;

  if (ss != 0) {                              // 定长帧
    if (frames > count) frames = count;
    *sum = ss * frames;
    return true;
  }

  const uint32_t entries = stsz.body + 12;
  if (frames > count) frames = count;
  if (entries + (uint64_t)frames * 4 > n) return false;   // 表超出这次读到的范围

  uint32_t s = 0;
  for (uint32_t i = 0; i < frames; i++) s += be32(b + entries + i * 4);
  *sum = s;
  return true;
}

// ------------------------------------------------------------
//  一次 GET（只取头部），返回读到的字节数
//
//  用 WiFiClientSecure + setInsecure()，和库里的 clientsecure 口径一致
//  （库在构造函数里就是 setInsecure，喜马/蜻蜓的证书链在 ESP32 上没法现验）
// ------------------------------------------------------------
static int httpGetHead(const char* url, uint8_t* buf, uint32_t cap,
                       uint32_t* totalOut, String* err) {
  bool ssl = false;
  String host, path;
  uint16_t port = 0;
  if (!urlSplit(url, &ssl, &host, &port, &path)) { *err = "url"; return -1; }

  String cur = url;
  for (int hop = 0; hop < M4A_SEEK_HOPS; hop++) {
    bool s2; String h2, p2; uint16_t pt2;
    if (!urlSplit(cur.c_str(), &s2, &h2, &pt2, &p2)) { *err = "url2"; return -1; }

    WiFiClient plain;
    WiFiClientSecure sec;
    WiFiClient* c = nullptr;
    if (s2) { sec.setInsecure(); c = &sec; } else { c = &plain; }

    if (!c->connect(h2.c_str(), pt2, s2 ? 12000 : 6000)) {
      *err = String("connect ") + h2;
      return -1;
    }
    c->setTimeout(8);

    String req = String("GET ") + p2 + " HTTP/1.1\r\n"
               + "Host: " + h2 + "\r\n"
               + "User-Agent: MyRadio\r\n"
               + "Accept-Encoding: identity\r\n"
               + "Range: bytes=0-" + String(cap - 1) + "\r\n"
               + "Connection: close\r\n\r\n";
    c->print(req);

    // ---- 响应头 ----
    uint32_t t0 = millis();
    String hdr;
    int status = 0;
    uint32_t clen = 0, crTotal = 0;
    while (millis() - t0 < 12000) {
      if (!c->available()) {
        if (!c->connected()) break;
        delay(2);
        continue;
      }
      int ch = c->read();
      if (ch < 0) break;
      hdr += (char)ch;
      if (hdr.length() >= 4 && hdr.endsWith("\r\n\r\n")) break;
      if (hdr.length() > 4096) { *err = "hdr"; return -1; }
    }
    if (!hdr.startsWith("HTTP/")) { *err = "nohdr"; return -1; }
    int sp = hdr.indexOf(' ');
    if (sp > 0) status = hdr.substring(sp + 1, sp + 4).toInt();

    // Content-Length / Content-Range: bytes 0-N/TOTAL
    int p = hdr.indexOf("Content-Length:");
    if (p < 0) p = hdr.indexOf("content-length:");
    if (p >= 0) clen = (uint32_t)hdr.substring(p + 15, p + 34).toInt();

    p = hdr.indexOf("Content-Range:");
    if (p < 0) p = hdr.indexOf("content-range:");
    if (p >= 0) {
      int slash = hdr.indexOf('/', p);
      if (slash > 0) crTotal = (uint32_t)hdr.substring(slash + 1, slash + 20).toInt();
    }

    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      p = hdr.indexOf("Location:");
      if (p < 0) p = hdr.indexOf("location:");
      if (p < 0) { *err = "noloc"; return -1; }
      int e = hdr.indexOf("\r\n", p);
      String loc = hdr.substring(p + 9, e);
      loc.trim();
      if (!loc.startsWith("http")) { *err = "relurl"; return -1; }
      cur = loc;
      c->stop();
      continue;                                // 跟一次
    }

    if (status != 200 && status != 206) { *err = String("http") + status; return -1; }
    *totalOut = crTotal ? crTotal : clen;

    // ---- 读正文 ----
    uint32_t got = 0;
    uint32_t lastData = millis();
    while (got < cap) {
      int av = c->available();
      if (av > 0) {
        int want = (int)(cap - got);
        if (av > want) av = want;
        int r = c->read(buf + got, av);
        if (r > 0) { got += r; lastData = millis(); }
        continue;
      }
      if (!c->connected()) break;
      if (millis() - lastData > 6000) break;   // 读不动了，拿到的就算
      delay(2);
    }
    c->stop();
    return (int)got;
  }
  *err = "hop";
  return -1;
}

// ------------------------------------------------------------
//  解析：从已经读到的 buf[0..n) 里挖 moov / mdhd / stsz / mdat，
//  算出第 sec 秒的绝对字节位置。读不够（moov 或被 stsz 超出）就返回 false。
// ------------------------------------------------------------
static bool parseHead(const uint8_t* buf, uint32_t n, int sec, uint32_t total,
                      M4aSeek* out, const char** why) {
  // ---- 顶层：找 moov 和 mdat ----
  BoxRef moov{}, mdat{};
  bool hasMoov = false, hasMdat = false;
  {
    uint32_t p = 0;
    while (p + 8 <= n) {
      const uint32_t sz = be32(buf + p);
      if (sz == 1) break;
      const uint32_t real = (sz == 0) ? (n - p) : sz;
      if (real < 8) break;
      if (memcmp(buf + p + 4, "moov", 4) == 0) { moov.off = p; moov.sz = real; moov.body = p + 8; hasMoov = true; }
      if (memcmp(buf + p + 4, "mdat", 4) == 0) { mdat.off = p; mdat.sz = real; mdat.body = p + 8; hasMdat = true; break; }
      if (p + real > n) break;
      p += real;
    }
  }

  if (!hasMoov) { *why = "no-moov"; return false; }

  // ---- moov -> trak -> mdia -> (mdhd, minf -> stbl -> stsz) ----
  uint32_t timescale = 0, sampleSize = 0, frames = 0;
  BoxRef stsz{};
  bool okStsz = false;

  uint32_t mEnd = moov.off + moov.sz;
  if (mEnd > n) mEnd = n;                      // moov 本体没读全

  uint32_t tp = moov.body;
  while (tp + 8 <= mEnd) {                     // 逐个 trak 试，取第一个有 stsz 的
    BoxRef trak{};
    if (!findChild(buf, tp, mEnd, "trak", &trak)) break;
    const uint32_t tEnd = trak.off + trak.sz;

    BoxRef mdia{};
    if (findChild(buf, trak.body, tEnd, "mdia", &mdia)) {
      const uint32_t dEnd = mdia.off + mdia.sz;

      BoxRef mdhd{};
      if (findChild(buf, mdia.body, dEnd, "mdhd", &mdhd)) timescale = mdhdTimescale(buf, mdhd);

      BoxRef minf{};
      if (findChild(buf, mdia.body, dEnd, "minf", &minf)) {
        BoxRef stbl{};
        if (findChild(buf, minf.body, minf.off + minf.sz, "stbl", &stbl)) {
          BoxRef tmp{};
          if (findChild(buf, stbl.body, stbl.off + stbl.sz, "stsz", &tmp)) {
            // frames 传 0 只读表头（count/sample_size），不累加
            if (stszSum(buf, n, tmp, 0, nullptr, &sampleSize, &frames)) {
              stsz = tmp;
              okStsz = true;
            }
          }
        }
      }
    }
    if (okStsz) break;
    tp = trak.off + trak.sz;
  }

  if (!okStsz || frames == 0) { *why = "no-stsz"; return false; }
  if (timescale == 0)         { *why = "no-timescale"; return false; }

  // ---- mdat 起点：一定要在 moov 之后，且这次读到了 ----
  if (!hasMdat) {
    // moov 结束位置往后按 box 链走到 mdat（通常紧跟，或中间夹一个 free）
    uint32_t p = mEnd;
    while (p + 8 <= n) {
      const uint32_t sz = be32(buf + p);
      if (sz < 8) break;
      if (memcmp(buf + p + 4, "mdat", 4) == 0) { mdat.off = p; mdat.sz = sz; mdat.body = p + 8; hasMdat = true; break; }
      if (p + sz > n) break;
      p += sz;
    }
  }
  if (!hasMdat) { *why = "no-mdat"; return false; }

  // ---- 目标帧 + 累加 ----
  // AAC 每帧 1024 个采样（ASC 里 frameLengthFlag=1 时是 960，有声书里没见过）
  uint32_t target = (uint32_t)((uint64_t)sec * timescale / 1024ull);
  if (target >= frames) target = frames - 1;

  uint32_t off = 0;
  if (!stszSum(buf, n, stsz, target, &off, nullptr, nullptr)) { *why = "stsz-short"; return false; }

  out->dataStart   = mdat.body;
  out->timescale   = timescale;
  out->frames      = frames;
  out->absPos      = mdat.body + off;
  out->frame       = target;
  out->moovBytes   = n;

  if (total && out->absPos >= total) { *why = "beyond-total"; return false; }
  return true;
}

// ------------------------------------------------------------
//  主函数
// ------------------------------------------------------------
bool m4aSeekOffset(const char* url, int sec, M4aSeek* out) {
  if (!url || !out) return false;
  if (sec < 0) sec = 0;
  memset(out, 0, sizeof(*out));

  // 两档：先 160KB（够 99%），失败再 384KB（超长 moov）
  const uint32_t caps[2] = { M4A_SEEK_CAP1, M4A_SEEK_CAP2 };
  const char* why = "";
  uint32_t total = 0;

  for (int attempt = 0; attempt < 2; attempt++) {
    const uint32_t cap = caps[attempt];
    uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
      evlog("m4a", "取文件头失败：PSRAM 分配不了 %uKB", (unsigned)(cap / 1024));
      return false;
    }

    String err;
    const uint32_t t0 = millis();
    const int n = httpGetHead(url, buf, cap, &total, &err);
    const uint32_t dt = millis() - t0;

    if (n <= 0) {
      evlog("m4a", "取文件头失败（%s，%ums）", err.c_str(), (unsigned)dt);
      free(buf);
      return false;                             // 网络问题，重试也没用
    }

    const bool ok = parseHead(buf, (uint32_t)n, sec, total, out, &why);
    const uint32_t got = (uint32_t)n;
    free(buf);

    if (ok) {
      evlog("m4a", "第%d秒 → 帧%u/%u ts=%u → 字节%u（读 %uKB，%ums）",
            sec, (unsigned)out->frame, (unsigned)out->frames, (unsigned)out->timescale,
            (unsigned)out->absPos, (unsigned)(got / 1024), (unsigned)dt);
      return true;
    }
    // 「没读够」才值得读更多；其它原因（没 stsz、timescale 缺失）再读也一样
    if (strcmp(why, "no-mdat") != 0 && strcmp(why, "stsz-short") != 0 &&
        strcmp(why, "no-moov") != 0) {
      evlog("m4a", "解析失败（%s），第%d集从头播", why, sec);
      return false;
    }
    if (attempt == 1) {
      evlog("m4a", "读到 %uKB 仍不够（%s），第%d秒算不出位置",
            (unsigned)(got / 1024), why, sec);
    }
  }
  return false;
}
