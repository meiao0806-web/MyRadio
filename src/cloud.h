// ============================================================
//  cloud.h —— 跨网远程控制（1.8.0）
//
//  为什么要有这一层：
//    1.7.0 及以前，手机 App 只能在家里的 WiFi 里控制机器（HTTP 直连）。
//    出门在外就没辙了。1.8.0 让机器自己往公网 MQTT 服务器上挂一条线，
//    手机在哪儿都能顺着这条线发命令 —— 不要求同一个网、同一个城市，
//    家里也不用开端口 / 公网 IP / DDNS。
//
//  通道：
//    机器订阅  myradio/<设备码>/c     手机往这儿发命令
//    机器发布  myradio/<设备码>/r     机器回结果
//    设备码 = 12 位十六进制（WiFi MAC），主题猜不到；
//    命令里还要带 6 位遥控码，对不上一律丢弃。
//
//  1.19.0 又加了一条单向通道（免配对发现）：
//    机器发布  myradio/dev/<6位短码>   每 30 秒一条 retain 的"我在线"
//    手机订阅  myradio/dev/+           App 打开就能列出看得见的机器
//    遥控码在免配对模式下由设备码算出（cloudDerivedCode），所以换手机不用再抄。
//
//  1.46.7：这条广播里多带一个 "pin":"1234"（机器那 4 位手机连接码）。
//    于是**跨网也能「输码就连」**：手机把 4 位码拿到本地一筛，只剩一台就直接选它，
//    和局域网输码是同一种手感。
//    为什么不另开一个 myradio/pin/<码> 主题：retain 一个主题只存一条消息，
//    家里两台都叫 1234 时会互相覆盖，用户「输 1234」就可能连到别人家去。
//    塞进每台自己的 dev 主题里，多台同码天然可以枚举，界面自己决定要不要让人挑。
// ============================================================
#pragma once
#include <Arduino.h>

void cloudBegin();                 // 读 /cloud.txt 并启动
void cloudLoop();                  // 在 loop 里调（执行排队的命令、处理重连）

bool cloudOn();                    // App 里的开关
bool cloudConnected();             // MQTT 已连上
const char* cloudDevId();          // 12 位设备码（全码，主题用）
const char* cloudShortId();        // 后 6 位（屏幕 / App 上给人看）
const char* cloudCode();           // 6 位遥控码
const char* cloudHost();           // 服务器主机名
int         cloudPort();

bool cloudSetOn(bool on);                       // 开 / 关（存 flash）
bool cloudSetCode(const char* c);               // 改遥控码（6 位数字）
bool cloudSetHost(const char* hostPort);         // "host" 或 "host:端口"
void cloudNewCode();                            // 换一个随机码

// ---- 1.19.0 免配对 ----
// open = 1（默认）：机器往 myradio/dev/<短码> 广播"我在线"（retain），
//   并且认「从设备码算出来的」遥控码 —— 于是任何手机装上 App 就能直接控制它。
// open = 0：撤掉广播（清空保留消息），只认屏幕上/App 里那串随机遥控码。
bool        cloudOpen();                        // 当前是不是免配对
bool        cloudSetOpen(bool on);              // 开 / 关（存 flash）
const char* cloudDerivedCode();                 // 派生码（与 App 的 deriveCode() 同算法）

// 给 /api/cloud 用的 JSON 字段片段（不含最外层花括号）
void cloudJson(String& o);
