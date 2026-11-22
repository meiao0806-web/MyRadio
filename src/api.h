// ============================================================
//  api.h —— 「动作 + 屏幕 JSON」这一层，给两条入口共用
//
//  1.7.0 及以前，这些逻辑长在 net.cpp 的各个 hXxx() 里 ——
//  那些函数直接 server.send()，只有 HTTP 能调。
//  1.8.0 起手机可以在外地控制（走 MQTT 公网中转），同样这些动作
//  也得能调一次，所以把它们抽出来放这儿：
//
//     局域网   net.cpp 的 handler  →  server.send(...)
//     跨网     cloud.cpp 的命令队列 →  这里 → 拼 JSON 回手机
//
//  !! 改动作语义时两边一起看：net.cpp 的 hStep/hPlay/hGroup/hVol/hSet
//     与这里的 apiStep/apiPlay/... 必须一模一样。
//     （屏幕 JSON 与台表 JSON 以本文件为准，net.cpp 已改为调用它）
// ============================================================
#pragma once
#include <Arduino.h>

// ---- 只读：与 /api/screen、/api/stations 相同的 JSON ----
void apiScreenJson(String& o);
void apiStationsJson(String& o);
void apiDiagJson(String& o);              // 体检（跨网时字段比局域网少几项，够看）
// 1.17.0：台的健康档案。one < 0 = 整表摘要（每台一个状态字符），
// one >= 0 = 单台详情。局域网与跨网都用这一份，避免两边口径不一致。
void apiHealthJson(String& o, int one = -1);
// 1.46.33：收听记录（局域网与跨网返回同一份，以前跨网恒为 0）
void apiHistoryJson(String& o);

// ---- 动作：成功返回 true ----
bool apiStep(int dir);                    // 下一台 / 上一台（有声书按专辑跳）
bool apiPlay(int id);                     // 播本组第 id 台
bool apiPauseToggle();                    // 播放 / 暂停
bool apiMuteToggle();                     // 静音开关
bool apiVol(int v);
bool apiSquelch(int v);      // 1.22.0：静噪门限 0..100（0=关）
bool apiHalfStep(bool on);   // 1.22.0：半步进                       // 0..100
bool apiGroupSet(int g);                  // 切到第 g 组
bool apiGroupShift(int dir);              // 上一组 / 下一组
bool apiSetKey(const char* k, const char* v);

// ---- 1.9.0 有声书集（一部书 = 一个台）----
// 上一集 / 下一集（dir>0 下一集），作用在「正在播的那个台」上
bool apiEpisStep(int dir);
// 直接跳到第 e 集（0 基，越界自动夹紧）
bool apiEpisJump(int e);
// 1.46.33：跳到「第 st 台的第 e 集」。App 的选集弹层不会再为了打开它先把机器切过去，
//   所以点具体某一集时才带台号过来；st == A.cur 时只换集，不换台。
bool apiEpisJumpAt(int st, int e);
// 1.46.33：「下次调到这个台时，从第 e 集起」（e<0 = 取消）。
//   !! 只写标记：不换台、不起播、不动「听到第几集」。
bool apiEpNext(int st, int e);
// 某台的集表（**按页**，1.18.0）：
//   {"prog":"","n":1167,"cur":2,"from":240,"list":[{"i":240,"t":""},...]}
// n = 这本书共几集，from = 这一页从第几集起（0 基），list 只装这一页。
// 为什么分页：官场现形记 1167 集，整份 JSON 30KB+，而跨网那条 MQTT 通道的
// 收发缓冲只有 16KB（CLOUD_BUF），一次发不完就会被截断/丢掉。
// budget = 这一页的字节上限（0 = 用默认值），按实际拼出来的长度卡，不按条数卡 ——
// 集标题长短差三倍，按条数卡要么撑爆要么太浪费。
// 保证至少返回 1 集（哪怕一条就超预算），否则 App 会以为这本书是空的。
bool apiEpisJson(int stIdx, String& o, int from = 0, int budget = 0);   // bright / offmin / screen
// replay / reconnect / reset_audio / fix_table / clear_history /
// reset_stations / forget_wifi / reboot / factory
// 1.46.33：reboot 类动作只置 *reboot，由调用方（cloud.cpp）把回包发完再重启。
bool apiFix(const char* action, bool* reboot = nullptr);

// ---- 1.46.33：台表增删改（跨网那条通道以前完全没有，App 只能弹"失败"）----
bool apiAdd(const char* name, const char* url, int tag, int* outId = nullptr);
bool apiDel(int id);
bool apiEdit(int id, const char* name, const char* url);
// hasTo=true 用 to 拖到第 to 位；否则按 dir 挪一位
bool apiMove(int id, int to, int dir, bool hasTo);
bool apiMvGroup(int id, int g, const char* gn);       // 换标签；gn 非空 = 新建
bool apiGroupOp(const char* op, int g, const char* name);   // add/rename/del/move

// ---- 1.46.33：WiFi 相关（实现放在 net.cpp，那边才有配网的状态机）----
void apiScanJson(String& o);                     // /api/scan：附近网络数组
void apiWifiListJson(String& o);                 // /api/wifi/list
// 1.46.36：返回值改成三态，谁也别再「无条件成功」
//   1 = 删掉了   0 = 本来就没这一组   -1 = 写盘失败（存储满 / 文件打不开）
int  apiWifiForget(const char* ssid);            // /api/wifi/forget
// 1.46.51：只存不连（App 里空槽填名字密码保存）。1 存好 / 0 参数 / -1 写盘 / -2 满
int  apiWifiAdd(const char* ssid, const char* pass);
void apiWifiStatusJson(String& o);               // /api/wifi/status
// /api/wifi：交给 net.cpp 的配网状态机（连不上会自动退回原来那组）
void apiWifiSet(const char* ssid, const char* pass);
// 1.46.51：用 /wifi.txt 里已存的密码去连（不重新输密码）
int  apiWifiUse(const char* ssid);
