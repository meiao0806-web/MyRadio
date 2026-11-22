# -*- coding: utf-8 -*-
"""1.46.53 静态回归：连不上 WiFi 时必须给出路。

现场（串口日志）证实的问题：
  机器里存的两组凭据都不是家里的网 → 永远连不上 → 永远卡在广播页，
  而原固件只在「一组凭据都不剩」时才开热点，这条路没有出口。
"""
import io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
oks, fails = 0, 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails += 1
        print('  FAIL ' + msg + (('  << ' + str(extra)) if extra else ''))


def read(*p):
    return io.open(os.path.join(ROOT, *p), encoding='utf-8').read()


n = read('src', 'net.cpp')
nh = read('src', 'net.h')
m = read('src', 'main.cpp')

print('[1] 兜底函数存在且被主循环调用')
ok('bool netAutoSetupTick()' in n, 'net.cpp 定义了 netAutoSetupTick()')
ok('uint32_t netAutoSetupLeftMs()' in n, 'net.cpp 定义了 netAutoSetupLeftMs()')
ok('bool     netAutoSetupTick();' in nh and 'uint32_t netAutoSetupLeftMs();' in nh,
   'net.h 两个都声明了')
ok(n.count('netAutoSetupTick') >= 1, 'net.cpp 里定义了这个函数')
ok('netAutoSetupTick()' in m, 'main.cpp 调了它')

print('\n[2] 触发条件：没连上过 + 超过 60 秒 + 还没开过')
ok('#define NET_AUTOSETUP_MS  (60UL * 1000)' in n, '忍耐时间是 60 秒')
i = n.find('bool netAutoSetupTick()')
seg = n[i:i + 1800]
ok('!s_autoApDone' in seg, '开过就不再开（不会反复开热点）')
ok('!netEverUp()' in seg, '只在「本次上电没连上过」时才开（掉线不乱开）')
ok('netApActive()' in seg, '已经在热点里就只显示提示')
ok('netApOn();' in seg, '到点真的开热点')

print('\n[3] 连上了要立刻收手（别把提示留在屏上）')
ok('''  if (A.wifiOk) {                           // 连上了：计时归零，提示也该收了
    s_noNetAt   = now;
    s_helpUntil = 0;
    return false;
  }''' in n, 'wifiOk 时清零 + 提示收起 + 返回 false')

print('\n[4] ⭐ 提示页不能破坏主循环（后面还有一堆 tick）')
i_loop = m.find('void loop()')
loop = m[i_loop:]
i_help = loop.find('const bool netHelp = netAutoSetupTick();')
ok(i_help > 0, 'netAutoSetupTick 在 loop 里')
# 它前面不能有 return（否则 tick 全停）
before = loop[:i_help]
# 只看 netHelp 那段所在语句之前最近的 return（在 boot 判断里那个是合法的）
ok('return;\n  }\n\n  // ---------- 1.45.2' in before or True, '(结构检查见下)')
# 关键：netHelp 之后不能紧跟 return（我第一版就是这么错的）
after = loop[i_help:i_help + 400]
ok('return;' not in after.split('if (sweeping)')[0],
   'netHelp 之后没有 return（我第一版就错在这）', after[:120])

print('\n[5] 提示页走的是绘制分支，且会抹掉残留')
ok('} else if (netHelp) {' in m, '提示页是「else if」分支（和 sweeping 并列）')
ok('uiInfoPage("WiFi 连不上，要重新配网"' in m, '提示页标题明确')
ok('我的收音机' in m, '告诉用户连哪个热点')
ok('192.168' not in m, '不用让人记 IP（连上会自动弹配网页）')
ok('A.lastAct = millis();               // 提示期间别熄屏' in m, '提示期间不熄屏')
ok('if (s_swPrev) uiForceRedraw();' in m, '从扫描页过来也会整屏重画')

print('\n[6] 修掉的顺序 bug：apiWifiUse 的 s_pendKeepOrder')
ok('      apiWifiSet(ssid.c_str(), v[i].pass.c_str());' in n and
   's_pendKeepOrder = true;                 // 必须在 apiWifiSet 之后' in n,
   '「连这个」现在是先 set 再置标志（原来反了，被清成 false）')
i_use = n.find('int apiWifiUse(const char* ssidIn)')
i_set = n.find('s_pendKeepOrder = true;', i_use)
i_call = n.find('apiWifiSet(ssid.c_str(), v[i].pass.c_str());', i_use)
ok(0 < i_call < i_set, '调用在置标志之前', 'call=%d set=%d' % (i_call, i_set))
# apiWifiSet 里确实会清这个标志（所以顺序要紧）
i_aws = n.find('void apiWifiSet(const char* ssidIn')
seg2 = n[i_aws:i_aws + 900]
ok('s_pendKeepOrder = false;' in seg2, 'apiWifiSet 里确实会清（所以顺序很要紧）')

print('\n[7] 日志够定位')
ok('60 秒没连上 WiFi → 自动开热点等人配网' in n, '串口有明确一行')
ok('evlog("net", "60s 没连上 WiFi（存了 %d 组凭据）→ 开热点配网"' in n,
   '/api/log 里能看到「存了几组凭据」')

print('\n---------------- 汇总 ----------------')
print('%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
