# -*- coding: utf-8 -*-
"""verify_apopen.py —— 1.46.8「热点去密码 / 改名 / 连上自动跳配网页」的回归

四段：
  A. 热点本身：SSID 是哪几个字、有没有密码、字节数够不够装
  B. 自动跳转：captive portal 的**状态码**对不对（这是能不能自动弹的关键）
  C. App 改名：桌面名 / 网页标题 / PWA 名 / 页面大标题 全都要一致
  D. 全仓不残留旧密码：12345678 不应该再出现在任何"要用户照着输"的地方

用法：python tools/verify_apopen.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def P(*a): return os.path.join(ROOT, *a)
def rd(*a): return io.open(P(*a), encoding='utf-8', errors='replace').read()

PASS = FAIL = 0
def ok(c, m):
    global PASS, FAIL
    if c: PASS += 1; print('  OK   ' + m)
    else: FAIL += 1; print('  FAIL ' + m)

def code_only(t):
    """去掉 C/JS 注释：注释里允许出现「以前是 12345678」这种说明"""
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    t = re.sub(r'//[^\n]*', '', t)
    return t

cfg = rd('src', 'config.h')
net = rd('src', 'net.cpp')

# ============================================================ A. 热点
print('[A] 热点：名字与密码')
m_ssid = re.search(r'#define\s+AP_SSID\s+"([^"]*)"', cfg)
m_pass = re.search(r'#define\s+AP_PASS\s+"([^"]*)"', cfg)
ok(bool(m_ssid), 'config.h 里有 AP_SSID')
SSID = m_ssid.group(1) if m_ssid else ''
PASS_ = m_pass.group(1) if m_pass else 'x'
ok(SSID == '我的收音机', '热点名 = 「%s」' % SSID)
ok(PASS_ == '', 'AP_PASS 为空串 = 开放热点（点一下就连）')

# ESP-IDF 的 ssid 是 uint8_t[32]，按 UTF-8 字节算
nbyte = len(SSID.encode('utf-8'))
ok(0 < nbyte <= 32, '中文 SSID 占 %d 字节（上限 32，UTF-8，够装）' % nbyte)
ok(nbyte != len(SSID), '确实是多字节（%d 字 / %d 字节）—— 手机看到的是完整的中文' % (len(SSID), nbyte))

# softAP 必须显式传 nullptr，不能靠"密码短于 8 位"这种隐式行为
ok('WiFi.softAP(String(AP_SSID).c_str(), AP_PASS[0] ? AP_PASS : nullptr);' in net,
   'softAP 空密码时显式传 nullptr（不依赖库的 strlen<8 隐式判定）')
ok('开放（无密码）' in net, '串口日志会说清「开放（无密码）」，不用猜')
ok('AP_PASS[0] ? AP_PASS : "开放（无密码）"' in net, '日志按有没有密码分两种说法')

# ============================================================ B. 自动跳转
print('\n[B] 连上就自动弹配网页（captive portal）')
ok('s_dns.start(53, "*", WiFi.softAPIP())' in net,
   'DNS 把所有域名指回本机（手机一联网就会被拽过来）')
# 各家探测地址
for path, who in [('/generate_204', 'Android'), ('/hotspot-detect.html', 'iOS'),
                  ('/ncsi.txt', 'Windows'), ('/connecttest.txt', 'Windows 10+')]:
    ok('server.on("%s"' % path in net, '%s 的探测地址 %s 已注册' % (who, path))

# 关键：未注册路径必须 302，不能 200
i = net.find('server.onNotFound(')
ok(i > 0, '有 onNotFound（各种国产 ROM 的探测路径都靠它兜）')
blk = net[i:i + 2600] if i > 0 else ''
# 只取「热点分支」那一段：从 fromAp 到配网页那行 send
j = blk.find('const bool fromAp')   # 从这行开始，'s_apMode && rip[0]==192' 才在切片里
if j < 0: j = blk.find('if (fromAp)')
apblk = blk[j:blk.find('// 单页应用', j)] if j > 0 else ''
ok('server.send(302' in apblk, 'onNotFound 里热点请求回 **302**（不是 200 + HTML）')
ok('sendHeader("Location"' in apblk and '/setup' in apblk, '302 指向配网页 /setup')
ok('send_P(200' not in apblk and 'send(200' not in apblk,
   '热点分支里不再有 200（回 200 有的系统会当成"网是通的、只是没这文件"，就不弹了）')
ok('s_apMode && rip[0] == 192' in apblk, '只对热点来的请求跳（局域网访问不被打扰）')

# ============================================================ C. App 改名
print('\n[C] App 名 = 「操控收音机」')
base = rd('tools', 'app/base.html')
body = rd('tools', 'app/body.html')
st = rd('android/app/src/main/res/values/strings.xml')
home = rd('android/app/src/main/assets/home.html')
wa = rd('src', 'webapp.h')

ok('<title>操控收音机</title>' in base, '网页版 <title> = 操控收音机')
ok('content="操控收音机"' in base, 'iOS 加到桌面时的名字也改了')
ok('<h1>操控收音机</h1>' in body, '首页大标题 = 操控收音机')
ok('"name":"操控收音机"' in net and '"short_name":"操控收音机"' in net,
   'PWA manifest 的 name / short_name 都改了')
ok('"short_name":"收音机"' not in net, '旧 short_name「收音机」已清掉')
ok('>操控收音机<' in st, 'APK 桌面名（strings.xml app_name）= 操控收音机')
ok('<title>操控收音机</title>' in home and '<h1>操控收音机</h1>' in home,
   'App 外壳首页标题 / 大标题 = 操控收音机')
ok('操控收音机' in wa, 'webapp.h 里已经是新名字（生成物跟上了源文件）')

# 远程版：标题后缀不能丢（以前写死原文，改名后会静默失效）
rm = rd('MyRadio-远程版.html')
mt = re.search(r'<title>([^<]*)</title>', rm)
ok(mt and mt.group(1) == '操控收音机 · 远程', '远程版标题 = %s' % (mt.group(1) if mt else '?'))
gr = rd('tools', 'gen_remote.py')
ok('re.search(r\'<title>([^<]*)</title>\'' in gr,
   'gen_remote 用正则取标题（不再写死「我的收音机」，改名不会静默失效）')

# ============================================================ D. 不残留旧密码
print('\n[D] 不给用户留「要输 12345678」的错觉')
targets = [
    ('tools/app/body.html', body),
    ('tools/app/js/pages.js', rd('tools/app/js', 'pages.js')),
    ('android/app/src/main/assets/home.html', home),
    ('src/webapp.h', wa),
    ('src/net.cpp', net),
    ('src/config.h', cfg),
]
for name, txt in targets:
    code = code_only(txt)
    # ⚠️ 别用裸子串：net.cpp 里有 "0123456789abcdef"（令牌随机字典），
    #   裸匹配会把它当成密码，报假 FAIL（这次就踩了）。
    n = len(re.findall(r'(?<![0-9a-fA-F])12345678(?![0-9a-fA-F])', code))
    ok(n == 0, '%s 里没有把 12345678 当密码（%d 处）' % (name, n))

# 但「没有密码」这个说法必须在用户看得到的地方出现
ok('没有密码' in body, 'App 帮助里写了「没有密码」')
ok('没有密码' in rd('tools/app/js', 'pages.js'), '配网步骤里写了「没有密码」')
ok('没有密码' in home, 'App 外壳的提示里写了「没有密码」')
# 注释里正好写着「别再拼出（密码 ）」—— 必须去注释后判断，否则自己撞自己
ok('（密码 ）' not in code_only(home), '不会拼出「（密码 ）」这种空括号')
ok("(AP.pass ? '（密码 <b>'" in home, '有密码才显示「（密码 xxx）」，没密码显示「没有密码」')

# Android 侧：两个热点名都要认（App 可能比固件新）
pv = rd('android/app/src/main/java/com/myradio/app/Provision.java')
ok('static final String AP_SSID = "我的收音机";' in pv, 'Provision.AP_SSID = 我的收音机')
ok('AP_SSID_OLD = "MyRadio"' in pv, '同时保留老固件的热点名 MyRadio')
ok('s.equals(AP_SSID) || s.equals(AP_SSID_OLD)' in pv,
   '判断"连的是不是机器热点"时两个名字都认')
ok('static final String AP_PASS = "";' in pv, 'Provision.AP_PASS 也是空（开放热点）')

# APK 版本必须升（改名不升版本，覆盖安装后桌面名字不会变）
gr2 = rd('android/app/build.gradle')
vcode = int(re.search(r'versionCode\s+(\d+)', gr2).group(1))
vname = re.search(r'versionName\s+"([^"]+)"', gr2).group(1)
mac = rd('android/app/src/main/java/com/myradio/app/MainActivity.java')
mav = re.search(r'APP_VER = "([^"]+)"', mac).group(1)
ok(vcode == 1260 and vname == '1.26.0', 'APK 版本 %d / %s（改了名就必须升，否则覆盖安装不生效）' % (vcode, vname))
ok(mav == vname, 'MainActivity 的 APP_VER(%s) 与 gradle 一致' % mav)

print('\n通过 %d / 失败 %d' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
