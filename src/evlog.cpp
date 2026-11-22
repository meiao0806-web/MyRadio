// ============================================================
//  evlog.cpp —— 见 evlog.h
// ============================================================
#include "evlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EV_N    64        // 条数
#define EV_LEN  96        // 每条长度（含结尾 0）

static char     s_buf[EV_N][EV_LEN];
static uint16_t s_head = 0;      // 下一条写这里
static uint16_t s_cnt  = 0;      // 一共写过多少条（用来算总数，饱和到 EV_N）
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// 一开始就记版本号：事后看到日志第一行就知道是哪一版跑的
static bool s_booted = false;

void evlog(const char* tag, const char* fmt, ...) {
  char body[72];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  // 用「上电后多少秒」当时间戳：板子没网时拿不到真实时间，
  // 而排查「断了多久才切」「几秒后回来」恰恰只需要相对时间。
  const uint32_t ms = millis();
  char line[EV_LEN];
  snprintf(line, sizeof(line), "%lu.%01lu %s: %s",
           (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100),
           tag ? tag : "-", body);

  portENTER_CRITICAL(&s_mux);
  memcpy(s_buf[s_head], line, EV_LEN);
  s_buf[s_head][EV_LEN - 1] = 0;
  s_head = (uint16_t)((s_head + 1) % EV_N);
  if (s_cnt < EV_N) s_cnt++;
  portEXIT_CRITICAL(&s_mux);

  // 串口也打一份：插着 USB 的时候能直接看
  Serial.printf("[ev] %s\n", line);
}

void evlogJson(String& o) {
  char tmp[EV_N][EV_LEN];
  uint16_t n = 0, start = 0;
  portENTER_CRITICAL(&s_mux);
  n = s_cnt;
  // 老的在前：写指针往前推 n 条就是最老的那条
  start = (uint16_t)((s_head + EV_N - n) % EV_N);
  for (uint16_t i = 0; i < n; i++)
    memcpy(tmp[i], s_buf[(start + i) % EV_N], EV_LEN);
  portEXIT_CRITICAL(&s_mux);

  o = "{\"n\":";
  o += n;
  o += ",\"list\":[";
  for (uint16_t i = 0; i < n; i++) {
    if (i) o += ',';
    o += '"';
    for (const char* p = tmp[i]; *p; p++) {
      const char c = *p;
      if (c == '"' || c == '\\') { o += '\\'; o += c; }
      else if ((uint8_t)c < 0x20) o += ' ';
      else o += c;
    }
    o += '"';
  }
  o += "]}";
}

void evlogClear() {
  portENTER_CRITICAL(&s_mux);
  s_head = 0;
  s_cnt = 0;
  memset(s_buf, 0, sizeof(s_buf));
  portEXIT_CRITICAL(&s_mux);
  s_booted = true;
}

// 开机那条（在 setup 末尾调一次）
void evlogBoot(const char* ver) {
  if (s_booted) return;
  s_booted = true;
  evlogClear();
  evlog("boot", "%s 起来了", ver ? ver : "?");
}
