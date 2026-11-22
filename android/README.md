# android/ —— MyRadio 手机 App 的外壳

把「手机 App」从「用浏览器打开一个网址」变成「桌面上一个图标」。

**这里不含界面本身。** App 的本体是固件里那份 `src/webapp.h`，由**收音机自己发出来**。
这个工程只做四件事：**配网（1.42.0：填表 + 发送）**、找机器、把 WebView 指过去、人在外地时走内置的远程页（远程控制恒开，不需要任何入口）。

```
android/
├── settings.gradle / build.gradle / gradle.properties
├── local.properties            本机 SDK 路径（build_apk.py 会重写）
├── myradio.keystore            自签（alias myradio，密码见 app/build.gradle）
└── app/
    ├── build.gradle            com.myradio.app / minSdk 23 / targetSdk 34 / 1.20.0
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/com/myradio/app/
        │   ├── MainActivity.java    WebView + 跳转 + 返回键 + JS 桥（MRApp）
        │   ├── Discovery.java       mDNS + /24 端口扫描
        │   └── Provision.java       配网（探测 → 发 WiFi → 等新 IP；1.42.0 重写）
        ├── assets/
        │   ├── home.html            首页：设置 WiFi（填表）+ 找机器 + 手动地址
        │   └── remote.html          ← tools/build_apk.py 从 MyRadio-远程版.html 复制，别手改
        └── res/                     图标 / 主题 / network_security_config（放行明文 HTTP）
```

## 出包

```bash
python tools/build_apk.py            # → 项目根 + 交付目录，MyRadio-<versionName>.apk
python tools/build_apk.py --icon     # 顺带重画桌面图标（tools/gen_apk_icon.py）
python tools/build_apk.py --no-copy  # 只编译
```

工具链（本机现成，脚本里是绝对路径）：

| 件 | 路径 |
|---|---|
| JDK 17 | `…\.build-android\jdk17` |
| SDK 34 | `…\.build-android\sdk`（build-tools 34.0.0 有 aapt / apksigner） |
| Gradle 8.6 | `…\.build-android\gradle-8.6\bin\gradle.bat` |
| AGP 8.4.0 | `~/.gradle/caches`，不用联网 |

全量 `clean assembleRelease` 约 20~30 秒。

## 设计取舍（想改之前先读）

**为什么不把网页塞进 APK？** 界面里 64 处请求都是 `/api/...` 的相对路径，只有「页面也是
这台机器发的」时才同源。塞进来就要重写全部请求前缀、处理 CORS、还要给 `/api/screen`
那条 MJPEG 流做代理 —— 风险远大于收益。外壳方案对 App 零改动，且固件升级后界面自动跟着新。

**代价**：APK 必须先能摸到机器才能出界面。人在外网时只能走内置的 `remote.html`（MQTT）。

### 进机器页的规矩（1.26.6 起，别绕开）

所有「打开 `http://<机器IP>/`」都必须走
`MainActivity.openDevice(ip, name, doRemember, stayOnFail)`：

- 先在子线程 `Socket.connect(ip, 80, 900ms)`；连不上就**不进**，直接 `fallback()`
- 进去了挂 6 秒看门狗：`onPageFinished` 之前没加载完，一律当够不着
- `onReceivedError` **一律接管，不要加任何时间窗** —— `ERR_CONNECTION_TIMED_OUT`
  要十几秒才报，任何「几秒内才处理」的判据都会漏掉它，漏掉的结果就是把人扔在
  WebView 自带的报错页上
- `fallback()`：有 `lastIp` → `remote.html`（MQTT）；没有 → `home.html`（去配网）

**`192.168.4.1` 永远不算「这台机器的 IP」** —— 它只在机器热点开着时通，而机器一连上
家里的 WiFi 就会关热点。开机读档、`remember()`、`lastIp()` 三处都要过滤。

2026-11-05 就是栽在这两条上：`lastIp` 存成了 192.168.4.1，一开 App 就扑向一个不通的
地址，超时的退路又被 8 秒窗口卡着，用户只看到一张「网页无法打开」。

## 配网（1.42.0：填表 + 发送，1.20.0 的「一键配网」删了）

1.20.0 那套用 `WifiNetworkSpecifier` 把手机自动切到机器热点 —— 实测问题一堆：
手机断网、系统弹窗被划掉就卡死、配完还要等手机切回来。1.42.0 按用户要求改成
**App 里直接选 WiFi / 填手机热点 + 密码，机器那边去连**（机器不发热点当首选）：

```
点「设置 WiFi」
  → provProbe()   手机够不够得着机器？（同网探 80 端口 / mDNS；够不着 → need-net 指路连热点）
  → 出表单：SSID（预填手机当前连的）+ 密码
      · 「从手机附近选一个」→ provRescan() → getScanResults()（App 侧扫，机器不扫）
      · 「用我手机的热点」→ 只讲清楚去哪开（Android 没有公开接口读本机热点 SSID）
  → provSend(ssid, pass)   → Provision 里走和 1.20.0 一样的 /api/wifi + /api/wifi/status
  → __mrProv({stage:'ok', ip}) → 1.6s 后 provFinish(ip) 打开 http://<新IP>/
```

失败回滚语义不变：`/api/wifi` 提交的是「新网络 + 旧网络」，机器连不上会自己退回原网络，
不会失联；`stage:'fail'` 把原因写给用户，面板上留「再检查一次」。

**离线遥控也删了**（`provDirect` 没了）：1.42.0 起远程控制恒开（固件 `cloudSetOn(false)`
被拒、读盘忽略 `on=0`），人在外地打开 App 自动走 `remote.html`，不需要任何按钮。

**网络列表在 App 侧，不找收音机要。** 优先级：

1. `probe` 时读手机当前 SSID → 直接预填，只要补密码；
2. 用户点「从手机附近选一个」才 `getScanResults()` 拉手机扫到的（按信号排序、带 dBm）；
3. 都拿不到就让用户**手打名字** —— 不阻断。

固件侧 `/api/scan` 与这条链路无关，不要去改它。

`Provision.java` 顶部的 `AP_SSID / AP_PASS / AP_BASE` 必须和 `src/config.h` 保持一致；
改热点名/密码时两边一起改。

权限：`ACCESS_FINE_LOCATION`（API ≤ 32）+ `NEARBY_WIFI_DEVICES`（API 33+，`neverForLocation`）。
没给权限只是列不出网络 —— `stage:'noperm'` 会说清「手打 WiFi 名字一样能配」。
机器开着配对码保护时 `stage:'locked'`，面板指路去机器上看 6 位数字。

## 找机器的两条腿

1. **mDNS**（秒级）：固件 `MDNS.begin("myradio")` + `addService("http","tcp",80)`，
   `NsdManager` 订 `_http._tcp.`，实例名含 `myradio` 就认。
   需要 `CHANGE_WIFI_MULTICAST_STATE` + `MulticastLock`，否则部分手机收不到组播。
2. **/24 扫描兜底**（约 3 秒）：本机 IP 前三段 × 1~254，并发探 80 端口（32 线程 / 350ms），
   通了再 `GET /api/info` 认人 —— 防止把路由器管理页、打印机当机器。

两条都空的话，首页「手动输地址」兜底。最近 8 台存 SharedPreferences。

> `Discovery.start()` 有 1.5 秒冷却：`onResume` 和首页脚本都会触发，不冷却的话两边互相
> `stop()`，mDNS 刚发现一半就被掐掉。

## JS 桥（`home.html` ↔ `MainActivity.Bridge`）

| JS 侧 | 作用 |
|---|---|
| `MRApp.scan()` | 开始找（1.5 秒内重复调用会被忽略） |
| `MRApp.recent()` | 返回最近用过的机器（JSON 字符串） |
| `MRApp.open(ip, name)` | 打开 `http://<ip>/`，并记进最近列表 |
| `MRApp.openRemote()` | 打开内置 `remote.html` |
| `MRApp.home()` / `MRApp.reload()` / `MRApp.forget(ip)` / `MRApp.toastMsg(s)` | 回首页 / 刷新 / 删记录 / 提示 |
| `MRApp.apInfo()` | 热点名/密码/地址/`SDK_INT`（JSON 串），页面据此写文案 |
| `MRApp.provProbe()` | 探测手机够不够得着机器（同网直连 / 借热点），结果走 `__mrProv` |
| `MRApp.provRescan()` | 只重扫手机附近的 WiFi |
| `MRApp.provSend(ssid, pass)` | 把家里的 WiFi 发给收音机 |
| `MRApp.provCancel()` | 收起面板、停掉等待（1.42.0：不再占着热点） |
| `MRApp.openWifiSettings()` | 跳系统 WiFi 列表（need-net 兜底时用） |
| `MRApp.provFinish(ip)` | 配完网：放热点 → 等手机恢复联网 → 打开设备页 |
| `window.__mrDev(ip,name,fw,how)` / `__mrNote(s)` / `__mrDone(n)` | Java → JS 的发现回调 |
| `window.__mrProv({stage,...})` | 配网进度回调：`ready / need-net / nets / locked / ok / fail / noperm`（1.42.0 起，`joining/older/provDirect` 已删） |

## 预览（不用手机也能看）

```bash
python tools/gen_app_preview.py     # → preview/app-setup.html + MyRadio-App配网预览.html
python tools/verify_provision.py    # 64 项断言（headless Chrome --dump-dom），改页面必跑
```

预览页 = `assets/home.html` 原文件 + 一份假 `MRApp` 桥，所以版式/文案和 APK 里那份是同一个。
选带 `5G` 的名字会演示失败那条路。

## 已知限制

- **没在真机上装过**（开发这台机器没有安卓机，也没有模拟器）。编译 + 签名 + `aapt dump
  badging` 都验过，运行时行为只有代码级保证。
- **配网在真机上没跑过**：`WifiNetworkSpecifier` 的弹窗文案、用户点取消时 `onUnavailable()`
  的时序，各家 ROM（MIUI / EMUI / ColorOS）不一样；`bindProcessToNetwork()` 之后是否真走热点，
  也要在真机上确认。
- 家里路由器开了 **AP 隔离**（客户端之间不能互访）时，mDNS 和扫描都找不到，只能手输 IP
  —— 但通常也连不上，得关掉隔离。
- 设备页里 `/api/screen` 是 MJPEG 流，WebView 走系统 Chromium，正常；某些定制 ROM 的
  WebView 版本太老可能不刷新屏幕镜像，其他功能不受影响。
- 换 `myradio.keystore` 后覆盖安装会失败，得先卸载旧版。
