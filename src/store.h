// ============================================================
//  store.h —— 台表与设置的持久化（LittleFS，纯文本，不用 JSON 库）
//
//  1.15.0 起**不再分组**：
//    /stations.tsv 每行「标签 <TAB> 地址 <TAB> 台名」，文件顺序 = 播放顺序。
//    开机把整份表读进 g_st[]（出厂 307 台，上限 MAX_STATIONS=500），
//    旋钮从第 0 台一路转到最后一台。
//
//    「标签」只是给手机 App 筛选 / 管理用的标记，不参与播放顺序，
//    也不要求同标签的行连续（18 部有声书就均匀穿插在电台之间）。
// ============================================================
#pragma once
#include "state.h"

bool storeBegin();
void storeSeedIfEmpty();          // 首次开机把 kPreset 灌进 LittleFS
bool storeLoad();                 // 整表读进 g_st
void storeSave();                 // 整份重写台表（增删/移动/改标签后）
void storeSaveDebounced();        // 频繁改（音量）时用，1.5s 合并一次
void storeTick();                 // 在 loop 里调，处理 debounce
// ---- 1.46.29：音量和「其余设置」分开落盘 ----
//   音量：每次调完写 NVS（往已擦好的页里追加一条记录，1~2ms，不擦块）→ 不打断声音。
//   其余：仍旧写 LittleFS，但只在「本来就没声」的时刻写（见 storeTick）。
void storeSaveVolFast();          // 音量立刻进 NVS
void storeLoadVolNvs();           // 开机：NVS 那份音量盖到 A.vol / A.fmVol
void storeFlush();                // 断电前：不管有没有声音，立刻把 cfg 落盘

// ---- 分组（1.15.0 起已退化：恒为 1 组、切组无效）----
// 保留这些签名是为了 App / 云端 / 屏上那些调用点不用全改；
// 它们现在只会得到「只有一组」，不会再真的换组。
int         storeGroupCount();
const char* storeGroupName(int g);
int         storeGroup();
bool        storeSetGroup(int g);              // 恒 false
bool        storeShiftGroup(int dir);          // 恒 false
int         storeGroupStations(int g);

// ---- 标签（App 的筛选 / 管理用）----
int         storeTagCount();
const char* storeTagName(int t);
int         storeTagStations(int t);
int         storeTagOf(int i);                 // 第 i 台的标签下标
const char* storeTag(int i);                   // 第 i 台的标签名（没标签返回 ""）

// ---- 台（整表，索引 0..g_stN-1）----
int  storeAdd(const char* name, const char* url, int tag);   // tag<0 = 广播
bool storeDel(int i);
bool storeMove(int i, int dir);   // 上移/下移一位
int  storeFind(const char* name);

// ---- 1.4.0 手机 App 用的编辑能力 ----
bool storeEdit(int i, const char* name, const char* url);       // 改名 / 改地址
bool storeMoveTo(int i, int to);                               // 拖到第 to 位（0 基）
bool storeMoveToGroup(int i, int gTo, const char* newGroup);    // 换标签（只改标签，不动位置）
bool storeGroupAdd(const char* name);                          // 新建标签
bool storeGroupRename(int g, const char* name);
bool storeGroupDel(int g);                                     // 删标签，台退回第 0 个标签
bool storeGroupMove(int g, int dir);                           // 标签排序（不影响播放顺序）
int  storeGroupEnsure(const char* name);                        // 有就返回，没有就建

// 台表里存的是「蜻蜓台号」或「完整 URL」两种，播放前统一还原
void storeUrl(int i, char* out, size_t n);
bool storeIsQtfm(int i);
// 收不收这个地址（蜻蜓台号 / http(s) / 蜻蜓节目页）
bool storeUrlOk(const char* u);

bool storeLoadCfg();
void storeSaveCfg();

// 配对令牌（App 首次绑定后固定）
const char* storeToken();
void        storeSetToken(const char* t);
