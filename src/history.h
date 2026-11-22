// ============================================================
//  history.h —— 收听记录与时长统计
//
//  为什么要有：
//    老人常问「我昨天听的那个台叫啥」「这个月听了多久」。
//    手机上要能看到，就得在机器里攒。但 flash 有写寿命
//    （约 10 万次），所以内存里按秒累计、60 秒才落盘一次。
//
//  归属判定：标签 + 台名。全国 31 个地区里有重名台，
//  只用台名会把两个地区的「新闻广播」合并成一条。
// ============================================================
#pragma once
#include "state.h"

#define HIST_MAX       48
#define HIST_NAME_LEN  34
#define HIST_GRP_LEN   14
#define HIST_DAYS      7      // 只留最近 7 天，够画个小柱状图

struct HistItem {
  char     name[HIST_NAME_LEN];
  char     grp[HIST_GRP_LEN];
  uint32_t sec;        // 累计收听秒数
  uint32_t last;       // 最后一次收听（unix 秒，0=还没同步时间）
  uint16_t plays;      // 打开次数
};

// 某一天的合计。key = YYYYMMDD
struct DayStat {
  uint32_t key;
  uint32_t sec;
  uint16_t plays;
};

void historyBegin();            // 读 /history.tsv
void historyTick();             // 主循环每秒调一次（不做 IO）
void historySaveNow();          // 立即落盘（清空/恢复出厂前调）
void historyClear();

int             historyCount();
const HistItem* historyAt(int i);      // 0 = 最近听的那个
uint32_t        historyTotalSec();
uint32_t        historyTodaySec();
int             historyTodayPlays();

int             historyDayCount();     // 0 = 今天
const DayStat*  historyDayAt(int i);
