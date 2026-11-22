#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_pairpin.py —— 1.46.5「4 位手机连接码」的离线回归

这条最该测的不是「代码写没写」，而是**固件和 App 两边说的是不是同一句话**：
  · UDP 端口两边一样吗？
  · 探针前缀都是 "MR1?" 吗？
  · 回包的分隔符、字段顺序对得上吗？
  · 码不对时两边都认得那个「0」吗？
这类不一致编译期一声不响，只有用户在那儿干看着「连不上」。

跑法：python tools/verify_pairpin.py
"""
import io
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OK = 0
BAD = 0


def rd(p, mode='r'):
    with io.open(os.path.join(ROOT, p), mode, encoding=None if 'b' in mode else 'utf-8') as f:
        return f.read()


def ck(name, cond, extra=''):
    global OK, BAD
    if cond:
        OK += 1
        print('OK   ' + name)
    else:
        BAD += 1
        print('FAIL ' + name + ('   ' + str(extra) if extra else ''))


cfg = rd('src/config.h')
pin_h = rd('src/pairpin.h')
pin_c = rd('src/pairpin.cpp')
net = rd('src/net.cpp')
main_c = rd('src/main.cpp')
ui = rd('src/ui.cpp')
pf = rd('android/app/src/main/java/com/myradio/app/PairFind.java')
ma = rd('android/app/src/main/java/com/myradio/app/MainActivity.java')
home = rd('android/app/src/main/assets/home.html')

# ---------------------------------------------------------------- 1. 出厂码
m = re.search(r'#define\s+PIN_DEFAULT\s+"([^"]*)"', cfg)
ck('config.h 定义了 PIN_DEFAULT', bool(m))
ck('出厂码就是 1234（用户要的那个数）', bool(m) and m.group(1) == '1234',
   m.group(1) if m else '')

m2 = re.search(r'#define\s+PIN_UDP_PORT\s+(\d+)', cfg)
ck('config.h 定义了 PIN_UDP_PORT', bool(m2))
fw_port = int(m2.group(1)) if m2 else -1
ck('UDP 端口不是 80/5353/67（不撞系统服务）', fw_port not in (80, 67, 5353, -1), fw_port)

# ---------------------------------------------------------------- 2. 固件行为
ck('pairpin.cpp 没有文件时写默认码', 'File w = LittleFS.open(F_PIN, "w")' in pin_c)
ck('pairpin.cpp 读盘后校验「4 位数字」', 'isFour' in pin_c and "c[i] < '0'" in pin_c)
ck('pairpin.cpp 只认 MR1? 开头的探针', '"MR1?"' in pin_c)
ck('pairpin.cpp 对上了回 MR1|1|名字|版本|IP|设备码', '"MR1|1|%s|%s|%u.%u.%u.%u|%s"' in pin_c)
ck('pairpin.cpp 码不对回 MR1|0', '"MR1|0"' in pin_c)
ck('pairpin.cpp 只回给发问的那个人（unicast）',
   'beginPacket(s_udp.remoteIP(), s_udp.remotePort())' in pin_c)
ck('pairpin.cpp 有防乱猜的限流窗口', 's_winN' in pin_c and '60000' in pin_c)
ck('pairpin.cpp 通网后重开 socket（换网本地 IP 会变）', 's_udpOn' in pin_c and 's_udp.stop()' in pin_c)
ck('pairpin.cpp 回包里不含令牌字段', 's_tok' not in pin_c and 'token' not in pin_c.lower())
ck('热点模式（AP）下也答话', 'WiFi.getMode() & WIFI_AP' in pin_c and 'softAPIP()' in pin_c)

ck('main.cpp 开机读码', 'pinBegin();' in main_c)
ck('main.cpp 主循环里应答', 'pinLoop();' in main_c)
ck('main.cpp include 了 pairpin.h', '#include "pairpin.h"' in main_c)

# ---------------------------------------------------------------- 3. /api/find
ck('注册了 /api/find', 'server.on("/api/find"' in net)
i = net.find('static void hFind()')
ck('hFind 存在', i > 0)
# 只截 hFind 自己这一段 —— 多切一个字都会把后面 netBegin 的 s_tok 算进来
_j = net.find('void netBegin()', i) if i > 0 else -1
seg = net[i:_j] if (i > 0 and _j > i) else ''
ck('/api/find 免鉴权（不能有 authed 检查）', 'authed()' not in seg.split('}')[0])
ck('/api/find 码不对回 bad=1', '"bad\\":1' in seg.replace('\\"', '\\"'))
ck('/api/find 回 ok=1', '"ok\\":1' in seg)
for k in ('name', 'fw', 'ip', 'devid'):
    ck('/api/find 回 %s' % k, 'kv(o, "%s"' % k in seg)
ck('/api/find 不吐令牌', 's_tok' not in seg)
ck('/api/find 热点下回热点地址（别回 "-"）', 'WiFi.softAPIP()' in seg and 'ipb' in seg)

# /api/pair 里要能读到/改到 4 位码
ck('/api/pair GET 带 pin 字段', 'pinCode()' in net and 'o += pinCode();' in net)
ck('/api/pair POST 支持 op=pin', 'op == "pin"' in net and 'pinSetCode' in net)
ck('改码后屏上显示一次', 'uiPairCode(pinCode())' in net)

# ---------------------------------------------------------------- 4. 屏上文案
ck('屏上那页文案跟 4 位走', 'strlen(s_pc) == 4' in ui)

# ---------------------------------------------------------------- 5. App 侧
m3 = re.search(r'static final int UDP_PORT = (\d+);', pf)
ck('App 里也定义了 UDP_PORT', bool(m3))
ck('两边的 UDP 端口一致（%d）' % fw_port, m3 is not None and int(m3.group(1)) == fw_port,
   m3.group(1) if m3 else '')

ck('App 发的探针和固件同一个前缀', '"MR1?" + code' in pf)
ck('App 认 MR1| 开头的回包', 'startsWith("MR1|")' in pf)
ck('App 认「1」= 对上了', '"1".equals(f[1])' in pf)
ck('App 认「0」= 码不对', '"0".equals(f[1])' in pf)
ck('App 优先广播（255.255.255.255）', '255.255.255.255' in pf)
ck('App 也发子网广播（同网段更可靠）', 'getBroadcast()' in pf)
ck('App 有兜底：逐地址问 /api/find', '/api/find?code=' in pf)
ck('App 兜底也认 bad=1', '"bad":1' in pf.replace('\\"', '"'))
ck('App 扫地址时走 NO_PROXY（别被系统代理劫走）', 'Proxy.NO_PROXY' in pf)

ck('MainActivity 暴露了 pinFind 给页面', 'public void pinFind(final String code)' in ma)
ck('MainActivity 声明了 pairCb', 'private final PairFind.Cb pairCb' in ma)
ck('pairCb 会把结果回给页面', 'window.__mrPair' in ma)
ck('找到后进机器页', 'go("http://" + ip + "/")' in ma)

ck('首页有 4 位输入框', 'id="pin"' in home)
ck('首页有连接按钮', 'id="btnPin"' in home)
ck('输满 4 位自动连（不用再点）', 'v.length === 4' in home and 'pinGo()' in home)
ck('只收数字、最多 4 位', "replace(/\\D/g, '')" in home and 'slice(0, 4)' in home)
ck('页面接了 __mrPair 回调', 'window.__mrPair = function' in home)
ck('默认值提示写着 1234', '1234' in home)

# ---------------------------------------------------------------- 6. 版本
v = re.search(r'#define\s+FW_VER\s+"([^"]+)"', cfg)
ck('固件版本 >= 1.46.5', bool(v) and v.group(1) >= '1.46.5', v.group(1) if v else '')

print('\n通过 %d / 失败 %d' % (OK, BAD))
sys.exit(1 if BAD else 0)
