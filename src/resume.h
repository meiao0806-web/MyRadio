// ============================================================
//  resume.h —— 有声书「听到第几分钟」的断点记忆（1.46.2）
//  与 epis 的分工：
//    epis.cpp  ：「这部书听到第几集」        —— 换集/关机都记得
//    resume.cpp：「这一集听到第几秒、什么码率」—— 换台/关机后从那儿接着放
//  为什么要单独存码率：续播是靠 HTTP Range 按**字节**定位的，
//    秒 → 字节需要码率（mp3 是 CBR，秒 × 码率 ÷ 8 就是起播字节）。
// ============================================================
#pragma once

void resumeBegin();                                        // 开机从 /resume.txt 载入
// 1.46.38：多带三个 ASC 字段（aot/声道/采样率）—— m4a 从中间起播必须靠它解，
//   没记住就只能从头放（mp3 用不到，传 0 就行）。
void resumeRemember(int book, int ep, int sec, int kbps,
                    int aot = 0, int ch = 0, int rate = 0);
bool resumeGet(int book, int* ep, int* sec, int* kbps,
               int* aot = nullptr, int* ch = nullptr, int* rate = nullptr);
// 这一集「上次听到第几秒」（只查，不改；给屏/App 显示用）
int  resumeSecOf(int book, int ep);
void resumeClear(int book);                                // 这一集放完了：清掉，别下次又续到尾巴
void resumeTick();                                         // 由主循环调
