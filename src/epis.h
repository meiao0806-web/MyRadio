// ============================================================
//  epis.h —— 「一部书 = 一个台」的集表（1.9.0）
//
//  1.7.0 的做法：有声书的每一集都是台表里的一个台，靠台名尾部的
//  「 001」认出它们属于同一部书，旋钮跳过整段。列表里还是 15 行，
//  老人翻起来全是「上下五千年 001..015」，认不出哪个是「那个台」。
//
//  1.9.0 改成：台表里一部书只占一个台（台名就是书名），
//  各集的地址与单集短名编在 preset_episodes.h 里（kEpis / kEpisBook）。
//   - 播完一集 → 自动接下一集；最后一集播完 → 回到第一集（循环）
//   - 再转到这部书 → 从上次听的那一集接着放（进度存 /epis.txt）
//   - App 可以「上一集 / 下一集 / 选集」，并看到「第 3 集 / 共 15 集」
// ============================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

// 开机时调（storeLoadCfg 之后、audioBegin 之前）
void episBegin();
// 主循环里调：2 秒合并写一次盘
void episTick();

// 台名 → 书号；不是有声书返回 -1
int  episBookOfName(const char* name);
// 当前组里第 stIdx 个台 → 书号；普通台返回 -1
int  episBookOfStation(int stIdx);

int  episBookCount();
const char* episBookName(int b);
int  episCount(int b);              // 这部书共几集
int  episCur(int b);                // 上次听到第几集（0 基；没听过 = 0）
void episSetCur(int b, int e);      // 记进度（合并写盘）
int  episStep(int b, int dir);      // 循环取下一集 / 上一集，返回新集号

// ---- 1.46.33：「下次从第 X 集播放」----
// 和「上次听到第几集」是两件事：
//   s_cur  用户听到哪儿了（一直跟着走）
//   s_next 用户在 App 里点名「下次调到这个台，从第 X 集起」
// 设的时候只写标记，**不换台、不起播**；等这个台真的被调到时才用一次，用完作废。
int  episNext(int b);               // 设了返回 0 基集号，没设返回 -1
void episSetNext(int b, int e);     // 设（越界自动夹紧）；e<0 = 清掉
void episClearNext(int b);
int  episTakeNext(int b);           // 取一次并作废（起播时用）；没设返回 -1
bool episUrl(int b, int e, char* out, size_t n);
const char* episTitle(int b, int e);   // 单集短名，可能为空串
