# MyRadio — 极简网络收音机固件

面向 **一台旋钮、一块 320×170 屏** 的 ESP32-S3 收音机。为长辈设计：**上电就出声，只会转旋钮也能用**。
有网放网络台，**没网自动切到 SI4732 收 FM**，网一回来自己切回去 —— 不用管，不用按。
所有设置放在手机 App（设备自带网页，五个页签）里：打开即自动绑定，配对码保护可选。
**1.8.0 起人不在家也能管它**：机器自己连公网中转，手机在外地打开「远程版」网页就能换台、调音量、调亮度。
**1.8.1 起 App 换了皮肤**：新拟物 —— 底色、卡片、按钮同一个色，靠光与影塑形（凸起＝能点，凹陷＝能输/能拖）。
改样式只看 `tools/app/style.css` 顶部那两组阴影变量和 `:root` 的色值，别在下面写死十六进制。
**1.15.0 起机器上没有「分组」了**：所有台串成一个列表，旋钮从第 1 台一路转到底；
22 部有声书均匀穿插在电台之间，「标签」只留在 App 里做筛选（见 6、7 节）。
**1.16.0 起 290 台 / 8 标签；1.20.0 起 307 台 / 9 标签；1.22.0 起 237 台**：1.16.0 加了蜻蜓 FM 的三部点播书（多出「科普」标签），1.20.0 又从原厂固件里扒出 CCTV-1~17（多出「央视」标签），
1.21.0 按用户点名单删掉 47 个台 + 1 本书（央视 5 个频道、沪陕川渝晋蒙辽吉的城市新闻台等），并加入「东广新闻」；
1.22.0 再删 23 个台（黑龙江 5、江苏 6、浙江 2、粤桂琼 7、甘宁 2 等），并给「换台乱跳」加了两道刹车，
音频库因此 vendored 进 `lib/` 并打了两处补丁 —— 蜻蜓的 m4a 被 CDN 标成 mp3，见 3.5c。
**1.19.0 起它会说话了**：电量低于 15% 念「电量低，请充电」，开路电压掉到 3550mV 撑满 90 秒念「没电了，关机」
再断 IO15 —— 两句中文预先合成进固件（142KB rodata），断网也能念，见 3.5d。
同时**跨网不用再手抄遥控码**：机器每 30 秒往 `myradio/dev/<短码>` 报一次到（retain），
任何一台装了 App 的手机打开就能看见它，遥控码由设备码算出来（FNV-1a），见 8.1。

> **这份 README 是给维护的人看的。** 只想用、不想看代码 → 双击 `说明书.html`（或看 `说明书.md`）。
> 想知道每次改了什么 → `CHANGELOG.md`。

---

## 1. 设计取舍（先说清楚）

| 需求 | 实现方式 |
|---|---|
| 有网听网络台，没网也要能听 | WiFi 断 20 秒 → 自动切 SI4732 收 FM（64–108MHz）；网回来 → 自动切回网络台，位置不丢 |
| 旋钮只调台 | 旋转＝换节目（一格一部书，普通台逐台）；**点一下＝只亮屏**（1.15.0 起不换台也不换组）；按住旋转＝音量（遥控器丢了也能调） |
| 其他功能全在 App | 加台/删台/改名/拖拽排序/换标签/标签管理、WiFi 配网、收听记录、体检修复，屏上一个设置项都没有 |
| App 里能看到机器 | 主页顶部是屏幕快照的**实时镜像**（`/api/screen` 每 900ms，前端按 ui.cpp 坐标重绘，不是截图） |
| 只能操作这一台 | 设备首次生成 16 位令牌并固定存 flash，App 自动取走；可选 6 位配对码保护（屏上显示，60 秒） |
| 不在一个网也要能管 | 机器**主动**连公网 MQTT（`myradio/<机器码>/c`），App 用「远程版」+ 6 位遥控码；家里不开端口、不要公网 IP。换台 / 音量 / 亮度 / 暂停都能远程做（见 8.1） |
| 换手机 / 升级 App 不用重配对 | 机器每 30 秒往 `myradio/dev/<短码>` 发一条**保留**消息（名字 / 固件 / 电量），App 订 `myradio/dev/+` 就能列出来；遥控码由设备码算出（FNV-1a），见 8.1 |
| 没电了要会说一声 | 两句中文预先合成进固件（142KB rodata）→ 首次开机落 LittleFS → `audio.cpp` 的 `CMD_VOICE` 播；15% 念「电量低，请充电」，3550mV 撑满 90 秒念「没电了，关机」再断 IO15（见 3.5d、9 节 23~26 条） |
| 换 WiFi 不失联 | `/api/wifi` 只发起试连，25 秒内成功才写盘，失败 `WiFi.begin(旧 ssid)` 回退，**不重启** |
| 蜻蜓 FM | 台表支持「纯数字＝蜻蜓台号」，播放时拼 `http://lhttp.qtfm.cn/live/<台号>/64k.mp3` |
| 遥控器换台+音量 | 红外 NEC，带**学习模式**，不用改代码 |
| 高度适老化 | 32px 台名（屏上最大一块）、单动作操作、错误自动恢复、不弹任何提示框 |

**关于两条声音链路**（1.5.0 起都走）：

```
网络台   ESP32 → I2S → PCM5102 → 功放 → 喇叭
FM 收音  SI4732 → 模拟输出 → 功放 → 喇叭     （ESP32 只用 I2C 告诉它调到多少 MHz）
```

两条链路完全独立：I2S 脚填错只影响网络台（FM 照常响），SI4732 接不通只影响 FM（网络台照常响）。
1.10.0 起四个波段全开（长波 / 中波 / 调频 / 短波连成一个旋钮圈，见 `fm.cpp`）。
AM 段需要磁棒天线或拉杆天线 —— 这台机器到底有没有，**还没实机确认**（见 9 节第 20、21 条）。

---

## 2. 目录

```
fw-myradio/
├─ 说明书.md / 说明书.html   # ★ 给使用者的说明书（交互 / 功能 / 现有台）
├─ CHANGELOG.md            # 每次改了什么
├─ 台表-全国.md / .csv      # 出厂台表完整清单（237 台 / 9 个标签，由 preset 生成，资料）
├─ MyRadio-远程版.html      # 独立远程网页（存手机里 / 挂网页空间，外地用）
├─ tools/台表-精选.md       # 出厂台表的**真源**（人工精选，一行一个台）
├─ platformio.ini          # 编译配置（TFT 引脚也用宏写在这里）
├─ lib/ESP32-audioI2S/     # vendored 音频库 = 上游 2.0.6 + 蜻蜓 m4a 两处补丁（别改回在线依赖）
├─ partitions.csv          # 16MB flash 分区：双 OTA + 4MB LittleFS
├─ src/
│  ├─ config.h             # ★ 唯一需要按实机改的文件（FW_VER 也在这里）
│  ├─ state.h              # 全局状态
│  ├─ store.cpp/.h         # 台表 + 设置（LittleFS，纯文本，流式整表重写）
│  ├─ history.cpp/.h       # 收听记录（段计时 + 60 秒落盘 + 7 天日统计）
│  ├─ audio.cpp/.h         # 播放（ESP32-audioI2S，独立任务）
│  ├─ input.cpp/.h         # 旋钮 + 红外遥控（含学习模式）
│  ├─ album.cpp/.h         # 1.7.0 有声书：一部书一个「节目」+ 每部书的进度
│  ├─ epis.cpp/.h          # 1.9.0 集层：每部书听到第几集（/epis.txt），上下集循环
│  ├─ api.cpp/.h           # 动作 + 屏幕 JSON 共用层（局域网 handler 与云端命令都调它）
│  ├─ cloud.cpp/.h         # 跨网控制：MQTT 公共中转 + 设备码 / 遥控码 / 命令队列 + 在线广播
│  ├─ battery.cpp/.h       # 1.19.0 电量：ADC 中位数采样 / 曲线换算 / 低电与没电三段判定
│  ├─ voice.cpp/.h         # 1.19.0 播报：两句 MP3 落 LittleFS，首次开机自愈式重写
│  ├─ voice_data.h         # 两句播报的字节数组（142KB rodata，生成物，勿手改）
│  ├─ net.cpp/.h           # WiFi / 热点 / REST API / 网页 App
│  ├─ ui.cpp/.h            # 320×170 界面 + 配对码页
│  ├─ main.cpp             # 开机流程
│  ├─ webapp.h             # 手机 App（单页，生成物，勿手改）
│  ├─ preset_stations.h    # 出厂台表 237 台 / 9 个标签（生成，勿手改）
│  ├─ preset_episodes.h    # 22 部书 / 1823 集（生成，勿手改）
│  └─ font_cn.h            # 自动生成的中文字库，勿手改
└─ tools/
   ├─ app/                 # ★ 手机 App 的源代码（下面展开）
   │  ├─ base.html         # 外壳：head / meta / 占位符
   │  ├─ body.html         # 五个页签的结构 + 底部 tabbar
   │  ├─ style.css         # 设计系统（暖米白 + 琥珀主色 + 动效曲线）
   │  └─ js/
   │     ├─ cloud.js       # 跨网：手写 MQTT 3.1.1 over WebSocket + 请求/响应配对
   │     ├─ core.js        # 请求封装 / 配对 / 屏幕镜像绘制 / 主页（有云就自动走云）
   │     ├─ stations.js    # 电台页：整表列表 / 标签筛选 / 指针拖拽 / 增删改 / 标签管理
   │     └─ pages.js       # 记录页 / 体检页 / 设置页
   ├─ icons/lucide.json    # 用到的 58 个 Lucide 图标内层 SVG（MIT）
   ├─ gen_webapp.py        # app/* → src/webapp.h（内联图标 sprite、压空白）
   ├─ gen_voice.py         # edge-tts 合成两句播报 → src/voice_data.h（1.19.0）
   ├─ fetch_qtfm.py        # 抓蜻蜓全国地区电台 -> qtfm_all.json
   ├─ gen_preset.py        # qtfm_all.json -> src/preset_stations.h
   ├─ gen_preset_curated.py# tools/台表-精选.md -> src/preset_stations.h（出厂表真源）
   ├─ fetch_qtfm_album.py  # 蜻蜓 FM 点播专辑 -> 有声书源-蜻蜓.json（1.16.0）
   ├─ patch_audio_lib.py   # 给 lib/ 里的音频库打 m4a 补丁（幂等；--check 只检查）
   ├─ gen_stationlist.py   # preset_stations.h -> 台表-全国.md / .csv
   ├─ gen_remote.py        # webapp.h -> MyRadio-远程版.html + preview/remote.html
   ├─ gen_font.py          # 生成字库（改动中文文案/台表后必须重跑）
   ├─ gen_webpreview.py    # 用真字库/真台表/真 webapp.h 生成离线预览页
   ├─ preview_template.html# 预览页模板（设备模拟 + 手机 App 双标签 + mock 后端）
   ├─ gen_manual.py        # 把真台表注入说明书模板 -> 说明书.html
   ├─ manual_template.html # 说明书模板（含可搜索台表）
   ├─ make_release.py      # 一键打包成 zip
   ├─ store_sim.py         # 台表读写的 Python 复刻 + 100 多条断言（无板子时验正确性）
   ├─ mqtt_broker_sim.js   # 本地假 MQTT 服务器 + 假机器（不用真机也能端到端验远程）
   ├─ verify_cloud.js      # 53 条：配对解析 / MQTT 编码 / 9KB 大消息 / 403 / 并发 / 免配对广播 / 派生码
   ├─ verify_batt.js       # 71 条：电量曲线 / 播报 MP3 帧头与大小 / 固件接线（1.19.0）
   ├─ verify_remote.js     # 10 条：headless Chrome 走完「填码 → 连上 → 换台」
   ├─ sim_screen.py        # 早期版式渲染工具（已被 preview/index.html 取代）
   └─ probe/probe.ino      # 屏幕引脚探测器（独立 Arduino 小程序）

preview/index.html         # 成品预览页，双击即开（离线可用，无需装任何东西）
preview/remote.html        # 远程版预览（配合 mqtt_broker_sim.js 在本地试远程）
```

---

## 2.1 先看效果：离线预览页

不用编译、不用接板子，双击 `preview/index.html`（Chrome / Edge 打开）就能看到两样东西：

- **收音机**：320×170 屏幕用 canvas 按逻辑坐标绘制（1.4.2 起改成**矢量字**，不再内嵌点阵字库，
  页面约 362 KB，其中 ~145 KB 是内嵌的 App 本体）。拖旋钮转 = 换台（停手 0.25 秒才切，
  中途只显示预览不算切台），点一下 = 只亮屏（1.15.0 起不换台也不暂停），按住旋钮拖 = 音量，长按 3 秒 = 开关热点；
  屏幕下面 9 个按钮模拟红外遥控器。右侧面板可增删电台、改顺序，屏幕和调谐盘一起变。
- **手机 App**：`webapp.h` 里的原始 HTML 直接塞进 iframe 跑，`fetch` 被劫持到模板里的
  **mock 后端**（`/api/screen` `/api/history` `/api/diag` `/api/fix` `/api/mvgroup` …全部实现）。
  所以不是「照着画一遍」，是**同一份代码**：在 App 里拖拽排序，左边屏幕和台表一起变；
  点「下一个」，左边屏幕立刻跟。

改完源码后重跑 `python tools/gen_webpreview.py` 重新生成（约 362 KB，不再内嵌字库）。

> **调谐盘上的频率认台名**：`nameFreq10()` 先从台名里认频率（`FM89.1` / `电台914` /
> `1008` / `FM100` 四种写法，268 个台里 65 个认得出来），认到就以它为盘面中心；
> 认不到才退回 `freq10()` 的「整表里第 i 台 / 共 n 台」线性映射（87.5～108.0 MHz）。
> 三份实现（`src/ui.cpp`、`tools/app/js/core.js`、`tools/preview_template.html`）规则必须一致，
> `node tools/verify_freq.js` 会把 268 个台名在三处各算一遍、并要求逐字相同。

---

## 3. 上机前必须确认的引脚

引脚分三类：**屏和 SI4732 已有原机出厂固件的源码作依据（可以直接用）**，
**只有 I2S 一组没实机确认**（见 3.2，以及「9. 已知限制」第 10 条）。

### 3.1 屏幕 SPI（必须）→ 决定屏幕能不能点亮

跑 `tools/probe/probe.ino`（Arduino IDE 单独烧），它会逐组试 MOSI/SCLK/CS/DC/RST，屏幕整屏闪红/绿/蓝时记下串口编号，然后改 `platformio.ini` 里对应的 `-DTFT_MOSI` / `-DTFT_SCLK` / `-DTFT_CS` / `-DTFT_DC` / `-DTFT_RST`。

当前默认值（多数 S3 320×170 板）：

```
MOSI=11  SCLK=12  CS=10  DC=13  RST=14  BL=38
```

画面上下颠倒 → `config.h` 里 `SCR_ROT` 改 1。

### 3.2 I2S 音频（必须）→ 决定网络电台有没有声音

> **这是整份配置里唯一没实机验证过的一组。** 商品页只写了「已添加 5102 解码芯片」，
> 没写 BCK/LCK/DIN 接在哪三个脚。

本机的 IO15 / IO16 / IO17 被三件更重要的活占着，I2S 必须让开：

| 脚 | 本机实际用途 | 依据 |
|---|---|---|
| IO15 | **整板外设电源自锁**（开机必须拉高，否则屏幕都不亮） | 原机固件 `PIN_POWER_ON` |
| IO16 | SI4732 复位 | `RESET_PIN` |
| IO17 | SI4732 的 I2C 时钟（SCL） | `PIN_FM_SCL` |
| IO18 | SI4732 的 I2C 数据（SDA） | `PIN_FM_SDA` |

所以 `config.h` 在 `BOARD_ATSMINI` 下把 I2S 挪到排针空脚（**推断值，待实测**）：

```c
#define PIN_I2S_BCK 39
#define PIN_I2S_LCK 40
#define PIN_I2S_DIN 41
```

**怎么判断填错了**：烧上去以后 FM 有声音、网络电台没声音（或只有电流声、爆豆声），
就是这三个数不对。一次换一个脚，试到出声为止。

候选脚按推荐顺序排（**一次只改一个，改完重烧**）：

| 候选 | 说明 |
|---|---|
| `39 / 40 / 41` | 当前默认值。ESP32-S3 的 JTAG 脚，当 I2S 用**不影响启动**，只是以后接不了 JTAG 调试器 |
| `42` | 同属 JTAG 那组，可单独替换其中一个 |
| `45 / 46` | 通常空闲。**注意**：这两个是 ESP32-S3 的 strapping 脚（复位瞬间采样电平，45 影响 flash 电压、46 影响启动日志）。接 DAC 这种高阻输入没事，但**别在这两个脚外部加上拉/下拉电阻**，否则可能起不来 |
| `47 / 48` | 一般空闲，但某些板把 48 拿去驱动 RGB 灯，改前先看一眼板子 |

> 判断边界很简单：**改完能正常开机（屏幕亮、串口有日志）= 选的脚没问题**，出不出声是下一步的事。
> 起不来就换回上一组，不会损坏硬件 —— 真正会伤硬件的情况只有 3.6 列的那三类。

**PCM5102 接法**：BCK→BCK，LCK→LRCK，DIN→DIN，GND 共地，VIN 接 3.3V。

### 3.3 旋钮（已确认）

A=2、B=1、按下=21（来自同板开源固件）。方向反了改 `ENC_REVERSE 1`。

### 3.4 红外接收头（可选）

`PIN_IR` 默认 47。本机（`BOARD_ATSMINI`）已经设成 **`-1`**：出厂机器上没看到红外接收头。
**没有红外头就设成 `-1`**，遥控功能自动关闭，其余不受影响。

想自己加一个 VS1838B 的话，47 通常是空的（这块屏走 SPI，只占 11/12/10/13/14/38），
但要先确认板上 47 没被别的东西占着，再把这个宏改成实际引脚。

### 3.5 SI4732 收音：长波 / 中波 / 调频 / 短波（1.10.0 起四个波段全开）

```c
#define PIN_FM_SDA      18     // 1.21.2 修正：43/44 是 UART0，不可能应答
#define PIN_FM_SCL      17
#define PIN_FM_RST      16
#define PIN_FM_EN       15     // 整板外设电源自锁，不是收音机专用
#define PIN_FM_AMP_MUTE  3     // 原厂 AUDIO_MUTE=IO3；低=放声。1.43.0 起 App 不再有静音
```

来源：`ralphxavier/SI4735` → `Lilygo_T-Display_S3/ALL_IN_ONE_T-Display_S3.ino`
（本机出厂固件就是从这个改的，所以这几个数是可信的）。

- 波段按商品页写的 **64–108MHz**（`FM_BAND_BOTTOM 640`），不是标准 87.5 起
- **1.10.0 起 AM 也开了**：`POWER_UP(FUNC=1)` 就能收，**不需要灌 patch** ——
  AN332 第 2 页 Table 1 写明 SI4732-A10 的 `AM_SW_LW` 固件模块与 SI4735-D60 相同，
  只有 SSB 才要 patch（1.5.0 那句「AM 必须先灌 patch」是错的，已更正）。
  四个波段见 `config.h` 的「收音波段环」：153~279 / 531~1602 / 64000~108000 / 2300~26100 kHz，
  一个 `fmTune(kHz)` 走完，跨波段时由 `fm.cpp` 自己 `POWER_DOWN → POWER_UP` 换芯片模式。
- **IO15 必须在 `setup()` 第一句拉高**，`main.cpp` 里已经做了。删掉这句 = 屏幕直接不亮
- 接不通时先看串口：`fmDiag()` 会把 I2C 总线上扫到的地址打出来。
  一个都没有 = 接线或供电问题；扫到 `0x11` 但 `POWER_UP` 失败 = 复位时序问题
- 没接模块 / 接错 → `fmBegin()` 返回 false，FM 功能整块静默关闭，**网络电台完全不受影响**

### 3.5b 有声书来源（1.16.0 起 5 个源）

台表里的「一部书 = 一个台」现在由 **5 个源**拼起来（`tools/gen_preset_curated.py` 的 `load_books()`）：

| 源 | 清单文件 | 抓取器 | 特点 |
|---|---|---|---|
| 喜马 RSS | `有声书源.json` | `fetch_books.py` | 最老那 5 部，上传者开了 RSS 才能用 |
| 13 听书网 | `有声书源-听书网.json` | `fetch_t13.py` | 全量集数，但**按出口 IP 限流**，抓久了 520 |
| 喜马单专辑 | `有声书源-喜马.json` | `fetch_xm2.py` / `fetch_xm.py` | 只有免费部分（前几十到 100 集） |
| **云听 radio.cn** | `有声书源-云听.json` | `fetch_rcn.py` | **标准 mp3、免 Referer、链接永久、全量可取** |
| **蜻蜓 FM 点播** | `有声书源-蜻蜓.json` | `fetch_qtfm_album.py` | **m4a，整档可放**；要签名、且 CDN 把 Content-Type 标成 mp3（见 3.5c） |

云听是目前最稳的一路。加一本书：

```bash
python tools/fetch_rcn.py --column=16631244851730 --group=历史 --name=精神的力量
python tools/gen_preset_curated.py
```

`--column` 就是 `www.radio.cn/pc-portal/sanji/detail.html?columnId=` 后面那串数字。

### 3.5c 蜻蜓 FM 点播专辑（1.16.0）与那个 m4a 坑

蜻蜓的**点播专辑**跟直播台是两套接口。直播是常量的 `lhttp.qtfm.cn/live/<台号>/64k.mp3`；
点播要现算签名，规则从前端 `main.<hash>.js` 里挖出来：

```
sign = HMAC_MD5("7l8CZ)SgZgM_bkrw", "/audiostream/redirect/<channel>/<program>")
url  = https://audio.qtfm.cn/audiostream/redirect/<channel>/<program>?sign=<32位hex>
```

签名里**不用**带 `t` / `device_id`，所以这条地址是常量，可以写进固件（实测约 171~200 字符，
在 URL 上限 320 以内）。它自己 302 跳到 `tchwod-sign.qtfm.cn/m4a/xxxx_24.m4a?auth_key=…`，
那个 `auth_key` 每次现签，**不能存**，只能存签名这一条。

```bash
python tools/fetch_qtfm_album.py --channel=140696 --group=科普 --name=原来是这样？！
python tools/gen_preset_curated.py && python tools/gen_font.py
```

`--channel` 就是 `www.qtfm.cn/channels/<id>` 里那串数字。脚本按 40 条一页翻完索引、
逐集生成签名地址并发一次 302 探活（死链直接剔掉）、按集号排序写进
`tools/有声书源-蜻蜓.json`；播客类节目的集号是**倒序**的，脚本会整表反转后再存。

> **坑：蜻蜓的 m4a 被 CDN 标成 MP3。**
> 内容其实是 **AAC 装在 MP4 里**，但 `Content-Type` 回的是 `audio/mpeg`。
> `ESP32-audioI2S` 2.0.6 只按这个头判编码（`parseContentType`），于是拿 mp3 解码器去解 aac，
> 表现是 **HTTP 200、数据在流、就是没声音** —— 这种「连上了但静音」最难查。
>
> 1.16.0 的做法：把音频库**拷进 `lib/ESP32-audioI2S/`**（上游 2.0.6 原样 + 两处带
> `MyRadio 1.16.0 补丁` 注释的改动），`platformio.ini` 的 `lib_deps` 里删掉 GitHub 那条，
> 靠 PlatformIO 自动收 `lib/` 目录：
> 1. `connecttohost()`：URL 里出现 `.m4a` 时记 `m_expectedCodec = CODEC_M4A`；
> 2. `parseContentType()` 的 `CT_MP3` 分支：若 `m_expectedCodec == CODEC_M4A` 就仍解 `CODEC_M4A`。
>
> 条件不命中时行为与上游完全一致。**别改回在线依赖**，上游没修这条。
> 补丁由 `tools/patch_audio_lib.py` 施加（**幂等**，已打过就跳过；`--check` 只检查不改）：
> 将来升级音频库，把上游新的 `src/` 覆盖进 `lib/ESP32-audioI2S/src/`，再重跑这个脚本；
> 上游若改动了那两处锚点，脚本会直接报「锚点不唯一/找不到」而不是打歪。

### 3.5d 电量检测（1.19.0，可选）

```c
#define PIN_BATT_ADC       4     // -1 = 没接，整个电量功能关掉
#define BATT_DIV           2.0f  // 分压比：100k/100k → 2.0；100k/33k → 4.0
#define BATT_CAL           1.00f // 校准：显示 3.95V 而万用表 4.00V → 填 1.013
```

接线：`电池+ → 100k → IO4 → 100k → GND`（两个电阻中间那个点进 ADC）。
两块 100k 串起来是 200k 对地，漏电流 21µA，对 2000mAh 的电池可以忽略。

**为什么必须分压**：`analogReadMilliVolts()` 最高只到 3.1V（11dB 衰减），单节锂电满电 4.2V 直接接上去会削顶。

**`BATT_CAL` 怎么定**：烧进去，串口第一行会打
`[batt] 引脚 IO4，引脚 1970mV，电池约 3940mV（分压 ×2.00）`；
同时用万用表量电池真实电压，`BATT_CAL = 实测 / 显示`。只影响百分比，不影响关机判定
（判定用的是 `BATT_EMPTY_MV`，也可以照着实测改）。

软件侧的三段逻辑、两道安全闸（`BATT_SEEN_HIGH_MV` 见过高位才算数、`BATT_OFF_MIN_UP_MS`
开机 5 分钟不关机）和 IO15 自锁断电，见 `src/battery.cpp` 顶部注释与 `CHANGELOG.md` 1.19.0。

### 3.5e 电源 / 电流保护（1.21.0）——「一直插着电」也不伤电池

这台机器的实际用法是常年插着 USB 当桌面收音机，所以有两件事要管：**发热**和**长期满电**。

先把边界说死，免得白折腾：

| 想做的事 | 固件能不能 |
|---|---|
| 把充电电流限到 500mA | **不能** —— 电流由充电 IC 的外围电阻定死 |
| 从 USB 侧拒绝快充（9V/12V） | **不能** —— 那是 D+/D- / CC 的物理层握手 |
| 控制整机吃多少电（音量、亮度） | **能**，这是发热的主因 |
| 满电后让充电 IC 停一下 | **能**，前提是使能脚接在 GPIO 上 |
| 判断满电 / 插着电 / 充得太猛 / 电压异常 | **能**，全从电池电压反推 |

**三个配置项，默认都不用改**

```c
#define PWR_ENABLE      1     // 0 = 整块保护关掉，行为回到 1.20.0
#define PIN_CHG_CTRL   -1     // 充电使能脚（CE/EN）。接了才能"真停充"
#define PIN_CHG_STAT   -1     // 充电状态脚（CHRG/STDBY）。接了就不用猜
```

温度三档（`PWR_T1_C` / `PWR_T2_C` / `PWR_T3_C` = 52 / 60 / 68℃），
每档压音量与亮度上限，最热那档 `audioStop()` 暂停播放，凉下来自动接回原来那台。
回滞 4℃（`PWR_HYST_C`），不会在临界点抖。

**只有一个脚要接：充电使能脚（可选）**

想让「充满后真的不再充」这一步生效，需要板上充电 IC 的 CE/EN 脚**引出一根线到空闲 GPIO**：

```c
#define PIN_CHG_CTRL        20    // 举例：接到 IO20
#define CHG_CTRL_ACTIVE_LOW  0    // TP4056 的 CE 是高有效 → 填 0；低有效填 1
```

接好之后的行为 = 笔记本的「保养模式」：
满电（4180mV 稳住 2 分钟）→ 关断充电 → 电池自然放到 4050mV → 重新开充。
长期插电时电池大部分时间待在 4.05~4.20V，而不是死顶在 4.2V 上。

**没有这根线也有意义**：满电后亮度压到 60%、音量压到 90，整机电流下来，
充电 IC 更快进截止状态，也不会在满电点反复「充一点→放一点」。

**快充这条线怎么看**

快充本身不会烧机器 —— 充电头和机器不握手，就退到 5V。真正要防的是
「劣质充电头/破解线强行输出 9V/12V」把充电 IC 打穿，表现为电池端电压异常升高。
固件从电池端反推，读到 ≥4300mV 就提示「充电异常，请拔充电器」，
接了使能脚的话同时停充。

想直接监控 USB 输入电压（5V 还是 9V），需要给 VBUS 加一路分压：

```
USB 5V ── 100k ──┬── 33k ── GND
                 └── IO_x（PIN_VBUS_ADC，VBUS_DIV = 4.03）
```

接上之后 `/api/batt` 的 `pvbus` 就是实测输入电压，`pfast` 表示是否 ≥7V。

**诊断字段**（`GET /api/batt`，1.21.0 追加）

| 字段 | 含义 |
|---|---|
| `ptemp` | 芯片温度（℃，-999 = 读不到） |
| `plvl` | 降载档位 0/1/2/3 |
| `pvcap` `pccap` | 当前音量 / 亮度上限 |
| `pheat` | 是否因过热暂停了播放 |
| `pcut` | 是否正在「真停充」 |
| `pfull` | 是否判为已充满 |
| `pvbus` | USB 输入电压（mV，-1 = 没接分压） |
| `pfast` | 是否判为快充档 |
| `prate` | 充电速率（mV/分钟，5 分钟平均） |

**回归**：`node tools/verify_power.js`（48 项：配置自洽、档位镜像含回滞、源码接线、安全性）

### 3.6 填错会烧坏机器吗

结论：**这份配置填错，最可能的结果是「没声音 / 屏幕不亮」，烧板子的概率很低**。
下面按风险分三档，心里有个数就行。

**基本无害（放心试）**

- I2S 三个脚填错 → 只是没声音。输出的是 3.3V 数字音频信号，撞上别的输入脚最多造成干扰
- SI4732 的 SDA/SCL 接反 → 通信失败，不会烧（I2C 是开漏加外部上拉，不会互相对打）
- RST / 功放静音 / 背光 / 屏幕 SPI 填错 → 控制无效、花屏或不亮，不会损坏

**要当心的三处**

1. **IO15（电源自锁）**：这是整板电源的开关脚，固件必须开机就拉高并一直保持。
   - 被误配成输入/浮空 → 板子反复上下电，一般不死，但对电源部分不友好
   - 被误配成输出低 → 每次开机立刻断电，表现为「一通电就黑」，不是烧，但很像板子坏了
2. **某个脚被配成输出高，而板上它直接连到 GND 或电源轨**：
   这时 IO 会持续灌/拉电流，ESP32 内部有限流（几十 mA 级），短时间能扛，**持续几小时可能损伤那个 IO**。
   判断方法：烧上去后如果某个 IO 明显发烫，立刻断电，把这个脚从配置里去掉。
3. **43 / 44 与 UART0 重叠**：这两脚同时是 UART0 的 TX/RX。本项目开了 `ARDUINO_USB_CDC_ON_BOOT=1`，
   串口日志走原生 USB（19/20），不会和 I2C 抢。但如果板子在 43/44 上另外焊了 USB 转串口芯片（CH340 之类），
   两个推挽输出会互相对打 —— **不会烧**（电流有限），但 I2C 会不稳，表现为时通时不通。

**首烧前的四条做法**

1. **先用 USB 供电，不装电池** —— 万一有短路，USB 口限流比电池安全得多
2. **开着串口**：`pio device monitor -b 115200`，第一行就能看到 `[fm] 引脚 ... 地址=0x11`
3. **烧完摸一下板子**：主控、SI4732、功放这三处如果明显发烫，立刻拔电
4. **第一次音量别开到最大**：FM 空台时底噪会被放大，容易啸叫

> 真正会烧的场景，基本只有「输出脚被硬短到地或电源」这一类。
> 这份配置里没有任何脚是这么接的（15/16/17 是控制脚，39/40/41 是排针空脚），所以正常烧录是安全的。

---

## 4. 编译与烧录

```bash
pip install platformio          # 若没装
cd fw-myradio                   # 解压后的工程目录（zip 包里叫 MyRadio-v1.4.0）
pio run -t upload               # 编译并烧录
pio device monitor -b 115200    # 看串口日志
```

- 平台：ESP32-S3（`esp32-s3-devkitc-1`），16MB flash / 8MB PSRAM
- 烧完不启动 → `platformio.ini` 里 `qio_opi` 改 `qio_qspi`
- 中文文案改动后：`python tools/gen_font.py` 重新生成字库（否则新字显示为空白）
- 改了手机 App（`tools/app/`）后：`python tools/gen_webapp.py` 重新生成 `src/webapp.h`
- 界面改了想先看效果：`python tools/gen_webpreview.py`

---

## 5. 第一次使用

1. **通电** —— 屏幕 1.5 秒后进入收音界面，自动播第一个台
2. **手机连热点** `我的收音机` —— **没有密码，点一下就连**；连上会自动弹配网页
3. **浏览器打开** `http://192.168.4.1` —— App 自动绑定，默认不需要配对码
4. App 里「设置 → 换一个 WiFi」填家里 WiFi —— **不重启**，25 秒内连上就切过去，
   失败自动退回原网络；之后用 `http://myradio.local` 访问

热点**默认是关的**（1.43.0 起）：家里 WiFi 换了、密码错了，就**按住旋钮 3 秒**把
`我的收音机` 热点打开（没有密码），手机连上去、浏览器开 `192.168.4.1` 改完，**再按 3 秒关掉**。
（开热点用的是 `WIFI_AP_STA`，所以不会把正在连着的家里 WiFi 踢掉。）

> 已进入家里 WiFi 时也可以直接把 App「添加到主屏幕」，用起来和原生 App 一样（PWA，
> 带 `manifest.webmanifest` 和 `/icon.svg`）。

---

## 6. 操作方式

全机只有两个操作源：**旋钮**（换台）和**遥控器**（换台 + 音量）。

### 旋钮（机器上唯一的按键）

| 手法 | 功能 |
|---|---|
| 旋转 | 上一台 / 下一台（237 台串成一圈；停转 250ms 才切，中间不会连一堆杂音） |
| 按住旋转 | 音量 ± |
| 短按 | **什么都不做**，只点亮屏幕（1.15.0 起。分组没了，也**不改成换台** —— 旋转已经在换台，再派活就会误碰跳台） |
| 双击 | **没有功能**（1.38.0 起双击不再静音；1.43.0 起机器上根本没有静音这个状态） |
| 长按 0.7 秒 | 重连当前台（老人最常用的一招） |
| 长按 3 秒 | **开关 WiFi 热点**（1.43.0 新增；默认关，开/关都是按满 3 秒后松手生效） |

### 红外遥控器（学习模式）

| 按键 | 功能 |
|---|---|
| ◀ / ▶ | **没有功能**（1.15.0 起不再分组；键位保留，老遥控器不用重学） |
| 上一台 / 下一台 | 换台（和旋钮一样） |
| 音量 + / − | 音量 |
| 播放/暂停 | 播放暂停 |

**上电时按住旋钮不放**，屏上出现「按 下一台 (1/7)」，然后依次按遥控器的 7 个键：

`下一台 → 上一台 → 音量+ → 音量- → 播放暂停 → 上一组 → 下一组`

学完自动存 flash，永久生效。没学过也能用常见 NEC 遥控器的默认码（每个动作内置 2–3 个候选码）。
注意顺序是**固定**的，上/下一组那两个键排在最后 —— 1.15.0 起它们没有功能，但**键位保留**，
所以老的 `/ir.txt` 既不用重学，也不会串键。

### 为什么 1.15.0 把分组整个去掉了

1.14.0 之前是「点一下换组、旋钮在组内换台」。实测下来长辈记不住**现在在第几组**：
想听评书要连点好几下，点过头又得绕一圈回来；进错组还以为台少了。
所以 1.15.0 **把组这一维删掉**：

- 所有台串成**一个列表**（237 台），旋钮从第 1 台一路转到第 237 台，到头绕回第 1 台
- **点一下旋钮不再换组**（只亮屏），遥控器 ◀ ▶ 也不再换组
- 22 部有声书**均匀穿插**在电台之间（约每 13 台一本），不会全堆在末尾
- 屏上顶栏的琥珀色小块（如 `广播`）还在，但它现在是**当前这台所属的标签**，
  只是一句内容说明，不是「第几组」，也没有左右三角

**「标签」只给 App 用**：App 顶部一排 chips（广播 / 军事历史 / 历史 / 评书 / 广播剧 / 悬疑 / 都市），
点一下只在本机筛选列表，方便快速找到、删掉、移动、改名 —— 机器上的顺序一台都不动。

最后一次听到哪台记在 `/cfg.txt` 的 `cur`（台序号），开机接着放。

---

## 7. 台表：307 个台 / 9 个标签

出厂台表是**人工挑过的**（去音乐台、去广告重灾区），真源 `tools/台表-精选.md`：

```bash
vim tools/台表-精选.md                 # 改台名 / 台号；小节名只决定「来自哪个地区」
python tools/gen_preset_curated.py     # 生成 src/preset_stations.h（加删台记得 PRESET_VER +1）
python tools/gen_stationlist.py        # 同步 台表-全国.md / .csv
python tools/gen_font.py               # 新台名里的生僻字要进字库，否则屏上是空白
python tools/gen_manual.py             # 说明书.html 里的台表跟着换
```

**表的先后就是播放顺序**（1.15.0 起没有「组」了，第一栏降级成标签，只给 App 筛选）：

| # | 标签 | 台数 | 内容 |
|---:|---|---:|---|
| 1 | 广播 | 268 | **所有直播电台**。**顺序有讲究**：中央台 9 个最前（中国之声打头，开机就播它）→ 央视 17（1.20.0）→ 陕西 9 → 四川 / 重庆 / 兵团 22 → 其余 217 → 苏州 11 最后 |
| 2 | 央视 | 17 | **CCTV-1 ~ CCTV-17**。1.20.0 新增，源从原厂固件的 flash 里扒出来（TS 封装、单 AAC 音轨 48kHz 立体声，`myalicdn` HLS） |
| 3 | 军事历史 | 2 | 抗战韩疯子(80 集)、二战战争史(20 集) |
| 4 | 历史 | 5 | 上下五千年、每晚一个历史故事、宇宙真相大探索、中国绝密档案、精神的力量 |
| 5 | 科普 | 1 | 原来是这样？！(488 集)（蜻蜓，1.16.0） |
| 6 | 评书 | 6 | 三国演义、官场现形记、乱世枭雄(200 集)、赵亮：白眉大侠外传(101 集)、民国奇案之枪毙刘汉臣(17 集)、春晚相声小品大全(100 集) |
| 7 | 广播剧 | 1 | 红楼梦(15 集) |
| 8 | 悬疑 | 3 | 鬼吹灯(100 集)、第九特区(60 集)、如临大敌(44 集) |
| 9 | 都市 | 4 | 战神出狱(200 集)、为民无悔(100 集)、首席医官(80 集)、一路高升(39 集) |

> 1.14.0 起**只有「广播」标签收直播电台**，其余 7 个标签全是有声书（22 部 / 1823 集）。
> 1.20.0 起「央视」也收直播（CCTV-1~17），所以是 2 个直播标签 + 7 个有声书标签 = 9 个。
> 以前的「新闻」标签（97 台）并进广播后既没台也没书，生成时被跳过 ——
> `ORDER` 里有 9 个名字，实际只出来 8 个。
>
> 1.15.0 起**22 部书均匀穿插**在电台之间（`gen_preset_curated.py` 的 `interleave()`：
> 按 `round((j+1)*n/(m+1))` 算每本书插在第几个电台后面，实测间隔 12~13 台，
> 没有相邻两本、末尾也不堆书）。
> 标签**不再要求同标签的行连续** —— 这正是「表顺序」和「标签」解耦之后才可能做的事。
>
> 「广播」组最前那 9 个是中央台，地址是 `ngcdn001.cnr.cn` / `sk.cri.cn` 的**公开 HLS**
> （`index.m3u8`，无签名、免 Referer），台表里存的是完整 URL 而不是蜻蜓台号。

> 1.8.x 是 341 台，因为**一部书的 15 集各占一个台**。1.9.0 把 75 集收进 5 个台，
> 表里少 70 行，听到的内容一个没少。
>
> 1.11.0 加了《乱世枭雄》（喜马 526 集，本版只拿到 200 集，详见 `tools/fetch_xm.py`
> 顶部注释与 CHANGELOG 1.11.0），集表总计 6 部书 / 275 集。

**重排规则**（都是 `gen_preset_curated.py` 顶上的常量，改台表顺序只动这几个）：

| 常量 | 值 | 作用范围 |
|---|---|---|
| `HEAD_SECTIONS` | `["中央台"]` | 整节顶到**整张表最前**（排在陕西之前）；开机默认台就是它 |
| `SX` | `"陕西"` | 整节并进电台段；不再单独成段 |
| `FRONT_SECTIONS` | `["四川","重庆"]` | 这些小节里按内容落在广播的台提到电台段前面 |
| `interleave()` | 按台数比例 | 把 22 部书均匀插进电台段（1.15.0） |
| `FRONT_KEYWORD` | `"兵团"` | **只对广播组**：台名带「兵团」的也提到前面 |
| `BACK_SECTION` | `"苏州本地"` | **对所有组**：苏州的台一律放到所在组的末尾 |
| `PRESET_VER` | `7` | 台表版本；改了顺序/台数就 +1，老机器升级固件时才会重灌 |

1.14.0 起**不再按台名分内容组**，所以上面 `FRONT_SECTIONS` / `FRONT_KEYWORD` 的
「只对广播组」已经等于「对所有电台」；`BACK_SECTION` 也只作用于广播组（其余组没有电台）。

### 7.1 有声书（点播直链：一部书 = 一个台）

**加一本新书只需要一条命令，然后重跑生成器**（五个数据源，产出都是
`tools/有声书源*.json`，`gen_preset_curated.py` 会自动合并）：

| 源 | 脚本 | 什么时候用 |
|---|---|---|
| 喜马专辑 RSS | `fetch_books.py` | 上传者开了 RSS 的专辑（很少），能拿全量 |
| 喜马单专辑 | `fetch_xm.py --album=<id> --group=评书 --name=<书名>` | 没开 RSS 的专辑。**接口一次只给 100 条且不分页**，所以只能拿到首尾各 100 集 |
| 13 听书网 | `fetch_t13.py` | 站点按出口 IP 封，要换 IP 才抓得动；拿到后一本书可全量 |
| 我听评书网 | `fetch_qm.py --sid=<书号>` | **只适合 2025 年后的新书**；老书音频域名 403 全死 |
| 蜻蜓点播 | `fetch_qtfm_album.py --channel=<id> --group=<标签> --name=<书名>` | 签名直链、整档可放；**m4a**，要配合 3.5c 那个库补丁 |



**22 部有声书均匀穿插在 268 个直播台之间**（1.15.0 起；以前是各占一组、堆在一起）。
**1.9.0 起一部书只占一个台**，台名就是书名，集藏在书里面（`src/preset_episodes.h`）：

```
前 14 台直播  →  三国演义(1 台 / 15 集)  →  15 台直播  →  官场现形记  →  …  →  苏州交通广播
```

| 文件 | 里面是什么 |
|---|---|
| `src/preset_stations.h` | 237 台。书只占一行，地址是**第 1 集** |
| `src/preset_episodes.h` | `kEpisBook[]`（书名 / 起始下标 / 集数）+ `kEpisItem[]`（逐集 标题 + 直链） |
| `src/epis.cpp` | 每部书听到第几集，存 `/epis.txt`（`书号<TAB>集号`，2 秒合并写盘） |

播放规则（`audio.cpp` 的 `nextEpOrStation()`）：

```
一集放完（EOF）  →  是书？下一集（末集回第 1 集，循环）  不是书 → advance(+1)
死链 / 加载失败  →  是书？下一集（整本都试过才换台）      不是书 → advance(+1)
选中这个台       →  startIndex() 用 episUrl(书, episCur(书))，从上次听到的那集开始
```

接口 `POST /api/epis`：不带参数回这台书的集表 `{"prog","n","cur","list":[{i,t}]}`；
`dir=-1|1` 上一集/下一集；`e=5` 跳第 6 集。`/api/screen` 多给 `epn`（共几集）、
`epi`（第几集，0 基）、`ept`（集标题）、`prog`（书名）；普通台 `epn=0`。

App 侧：播放键下面一条 `[上一集] 第 3 集 共 15 集 [下一集]`，点中间弹出整本书的集表。
电台列表里书那行标 `共 15 集`，长按菜单第一项是「选集（共 15 集）」。

回归：`node tools/verify_album.js`（headless Chrome，需要先起
`--remote-debugging-port=9333` 的 Chrome）—— 75 条断言，覆盖台表结构（9 个标签 / 237 台）、
旋钮一格一本书、书均匀穿插、集层循环、每部书各记各的、`/api/epis` 的三种用法、
点一下空转、标签筛选不动播放、`/api/stations` 契约。

**1.7.0 的旧模型**（`albumSplit` 从台名后缀认专辑、一集一个台、`/album.txt` 存进度）
代码还在，`album.cpp` 仍管「旋钮一格一个单元」；只是 1.9.0 起**没有任何台名再带
` 001` 后缀**，所以每台自成一个单元，判据退化成恒等，不会跟新模型打架。

源是**喜马拉雅专辑的 RSS**：开了 RSS 输出的专辑，`https://www.ximalaya.com/album/<id>.xml`
每条 `<enclosure>` 就是一条 m4a 直链，**裸请求（无 Referer / 无 Cookie）返回 206 audio/mp4**，ESP32 直接播。
实测 2022 年发布的链接到 2026 年还有效。

```bash
vim tools/有声书.json       # 改书目：group / name / feed / skip / count
python tools/fetch_books.py # 抓 RSS → tools/有声书源.json
python tools/gen_preset_curated.py
```

几个必须知道的点：

| 点 | 说明 |
|---|---|
| `jt=` 参数不能删 | 删掉就退化成 `200 text/plain`，播不了。所以完整 URL 有 212~233 字符 |
| `Station.url` 是 320 字节 | 原来 200 装不下，连行缓冲一起从 384 加到 512 |
| 书内连播不靠相邻台 | 1.9.0 起一集不是一台，连播走 `nextEpOrStation()`，所以台表里**可以**把直播台插在书之间（1.15.0 就是这么穿插的） |
| 集数要手动改 | 现在每部书只取前 15 集（`count`），想听后面的集就改 `skip` 重新抓 |

> **为什么不直接用蜻蜓的「历史 / 评书 / 广播剧」分类**：蜻蜓这两个分类下是**专辑（点播）**，
> 专辑 id 走 `live/` 接口返回 404，而且页面要登录/会员，拿不到直链。
> 喜马拉雅 RSS 是唯一天然可播的免费路径。

**台号怎么来的**：台表里**存纯数字就是蜻蜓台号**，播放时拼成
`http://lhttp.qtfm.cn/live/<台号>/64k.mp3`。填 `http` 开头的地址则直接播任意流（mp3/aac/m3u8 都支持）。

**死台黑名单**：`gen_preset_curated.py` 里有个 `DEAD` 集合，记录已下架的台
（2026-10-31 逐个用 Range 请求验过，7 个 404）。不剔掉的话，旋钮转过去会卡住重连 5 次再跳台。
官方哪天恢复了，从集合里删掉台号重新生成即可。

**怎么存得下**：1.15.0 起**整张表都在内存里**（`g_st[]`，`Station` 每台 360 字节）。
1.45.1 起这块内存走 **PSRAM**（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`），
`MAX_STATIONS × 360 B` 在 8MB 里毫无压力：380 台 137KB、500 台 180KB。
以前（1.21.1~1.45.0）它是**静态数组躺在内部 DRAM**，500 台 176KB 会把堆挤到 57KB、https 台全连不上，
所以一度退回 380；搬去 PSRAM 之后这个顾虑没有了，1.46.1 又放回 500。
**现在真正的上限不是内存，是 `/api/stations`**：它把整表拼成一个 `String`（实测约 80 B/台），
500 台 ≈ 40KB，`std::string` 翻倍增长的峰值 ≈ 80KB，而播放时内部空闲仍有 165KB ⇒ 安全。
再往上（>600 台）必须先把这接口改成分块发送，只改宏会变成「App 电台页偶尔空白」。
台数上三位后，屏幕小框里的「播放中」会自动退成「播放」，给序号让位（`psTextShort()`）。

**改台表**：App 里增删改序改标签**全都只动内存**，改完**整份重写一次** ——
按内存顺序写 `/stations.tmp` → `commitTable()`（remove + rename），标签另存 `/tags.txt`。
掉电最多丢掉这次操作，不会把台表写成半截。

几个必须知道的细节：

| 概念 | 含义 |
|---|---|
| 文件格式 | 每行 `标签<TAB>地址<TAB>台名`；`#` 开头整行忽略；地址为空的行跳过（老格式的空占位行，1.15.0 不再写） |
| 顺序 | **文件顺序 = 播放顺序**。标签可以乱序、可以重复出现，都不影响播放 |
| `/tags.txt` | 一行一个标签名，**顺序 = App 里 chips 的顺序**。单独存一份是因为①新建的空标签也得显示 ②顺序不该被「哪台先出现」决定 |
| 标签下标 | 每台只存一个 `uint8_t` 下标（`s_tagOf[]`）。所以**改名自动生效**，删标签要整体前移下标 |
| 删标签 | **不删台**：这些台退回第 0 个标签（广播）。第 0 个标签**不许删** —— 它是退回目标，删了这些台就无处可退 |
| 上限 | `MAX_STATIONS=500` 台、`MAX_GROUPS=48` 个标签、标签名 15 字节 |

`tools/store_sim.py` 是这套逻辑的 Python 复刻 + 100 多条断言（整表往返、脏行 / 老格式容错、
增删改序、换标签、标签增删改名排序、容量上限、台表版本升级前的备份），
本机没 PlatformIO 时用它兜住正确性：

```bash
python tools/store_sim.py      # 全部通过 / 有几项失败
```

**字库要注意**：`preset_stations.h` 里的台名含生僻地名用字（亳、漯、睢、綦、衢、阆、颍…）
和韩文台名，`gen_font.py` 会扫 `src/` 下所有 `.h/.cpp` 的**非 ASCII 字符**把它们收进字库。
换台表后必须重跑 `python tools/gen_font.py`，否则那些台名会显示成空白。

**想换回全量地区表**：`python tools/fetch_qtfm.py && python tools/gen_preset.py`（31 个地区 1038 个台）。
注意 `gen_preset.py` 产出的头文件结构得跟 1.15.0 对齐（`kPreset` + `kPresetTag` + `kPresetTagOf`），
否则固件编不过；1038 台**内存上**装得下（374KB PSRAM），但 `/api/stations` 的 JSON 会到 83KB、
峰值 166KB —— 得先把那个接口改成分块发送，再动 `MAX_STATIONS`。


---

## 8. 手机 App 接口（自己写客户端时用）

先 `GET /api/auth` 拿 `{"token":"..."}`，之后所有请求带 `X-Token` 头（或 `?t=<token>`）。
鉴权失败返回 `401 {"err":"unauthorized"}`，参数错返回 `400 {"err":"..."}`，其余都是 `200`。
所有响应 `Cache-Control: no-store`，并带 `Access-Control-Allow-Origin: *`。

**读**

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/auth` | 取/建令牌。开着配对码保护时必须带 `?code=123456`，否则回 `{"locked":1}` 并让屏幕显示 6 位码 |
| GET | `/api/info` | `fw` `name` `paired` `needSetup` `ap` `locked` `ip` `count` `up`，1.8.0 起多 `devid` `rcode` `cloud` `cloudConn`，1.19.0 起再多 `open` `dcode` 和电量六件套（`bok` `batt` `bmv` `chg` `blow` `bpin`） |
| GET | `/api/state` | 当前台 / 组 / 播放状态 / 音量 / 信号 / 时钟（老接口，新 App 用 screen） |
| GET | `/api/screen` | **屏幕快照**：`on` `br` `clock` `wd` `grp` `gname` `idx` `cnt` `name` `ps` `vol` `mute` `wifi` `rssi` `pv` `learn` `pc` `tip`，1.9.0 起有声书多 `epn` `epi` `ept` `prog`（普通台 `epn=0`）。1.15.0：`grp` 恒为 0，`gname` 改成**当前这台所属的标签** |
| GET | `/api/stations` | `{count, tags:[{i,n,c}], list:[{i,n,u,q,ep,t}]}`。1.15.0 起 `list` 是**整张表**（237 台，下标即播放顺序），`tags` 是标签表（`c`=该标签几台），`t`=这台的标签下标；`q=1` 是蜻蜓台号，`ep` 是这部书共几集（普通台 0） |
| GET | `/api/history` | `{total, today, plays, days:[{d,s,p}], list:[{n,g,s,last,p}]}`（秒 / 次数 / 最后时间戳） |
| GET | `/api/diag` | 体检全量：`fw` `up` `reset` `heap` `heapMin` `psram` `fsTotal` `fsUsed` `wifi{}` `audio{ps,retry,dead,kbps,sec,url}` `time{ok,now}` `table{tags,count,total}` `token{set,fp}` `pair{locked}`（1.15.0 把 `groups` 换成 `tags`；1.19.0 起多 `batt{bok,pct,mv,chg,low,pin}` `voice{ready,busy}`） |
| GET | `/api/health` | **台的健康**（1.17.0）。整表摘要 `{n, s, f, c:{ok,bad,dim,unk}, cur:{i,st,lv,sec,why}}`：`s` 每台一个字符（`0` 没测过 / `1` 正常 / `2` 连不上 / `3` 没声音）、`f` 每台失败次数；带 `?i=12` 回单台详情 `{i,n,st,v,fail,lastOk,lastChk,lv,ep,live}` |
| GET | `/api/batt` | **电量**（1.19.0）：`bok`(读数可信=1) `batt`(0-100，不可信时 -1) `bmv`(平滑后的电池毫伏) `chg`(在充电) `blow`(低电中) `bpin`(ADC 脚)，外加播报状态 `vready`(0~2 段可用) `vbusy` |
| GET | `/api/pair` | `locked` `fp` `ip` `up` `dev`，锁定时多一个 `code` |
| GET | `/api/wifi/status` | 换网进度：`state`（0 空闲 / 1 连接中 / 2 成功 / 3 失败）`msg` `ssid` `ip` `rssi` `ok` |
| GET | `/api/scan` | 扫 WiFi：`[{s,r,e}]`（`e` 是 encryptionType，0 = 开放） |
| GET | `/api/cloud` | 跨网状态：`on` `conn` `devid` `short` `code` `host` `port` `cmds` `bad`，1.19.0 起多 `open`（免配对）`pres`（已广播几次） |
| GET | `/manifest.webmanifest`、`/icon.svg` | PWA 清单与图标（`/` 和任何未知路径都回 App 本体） |

**写**

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/api/play` | `id` | 播第 id 台（整表下标） |
| POST | `/api/step` | `dir`=1/-1 | 上一台 / 下一台 |
| POST | `/api/epis` | —（或 `id`=台号，`from`=起始集） | 回这台书的集表**一页**：`{"prog","n","cur","from","list":[{i,t}]}`；普通台 `n=0`。1.18.0 起按**字节预算**切页（`EPIS_PAGE_BYTE`=8500）—— 官场现形记 1167 集，整份 30KB+，跨网那条 16KB 的 MQTT 通道发不出去 |
| POST | `/api/epis` | `dir`=1/-1 | 正在播的这本书 上一集 / 下一集（两端循环） |
| POST | `/api/epis` | `e`=N | 直接跳第 N+1 集（0 基） |
| POST | `/api/group` | — | **空壳**：1.15.0 起不再分组，恒回 `{"ok":0,"msg":"1.15.0 起不再分组"}` |
| POST | `/api/vol` | `v` + `ch=net\|fm` | 设音量。**1.44.0 起是原厂口径**：网络那一路 `6..21`、收音那一路 `6..63`，各自直接写库/芯片，不再有「档位」换算 |
| POST | `/api/mute` / `/api/pause` | — | **1.43.0 起都是空操作**（机器上没有静音，暂停也删了）；`/api/mute` 恒回 `{"ok":1,"mute":0}`，留着只为不回老 App 的 404 |
| POST | `/api/add` | `name`,`url`[,`g`] | 加台（加到表末尾）；`url` 纯数字=蜻蜓台号，`http…`=任意流；`g` = 标签下标（省略=0 广播） |
| POST | `/api/edit` | `id`,`name`,`url` | 改名 / 改地址（空字段 = 不改） |
| POST | `/api/del` | `id` | 删第 id 台（删的是在播的就接着播下一台） |
| POST | `/api/move` | `id`,`to` 或 `id`,`dir` | `to` = 拖到第 N 位；`dir` = 上下挪一格 |
| POST | `/api/mvgroup` | `id`,`g` 或 `id`,`gn` | 换标签（**只改标签、不动位置**）；`gn` = 当场新建一个标签 |
| POST | `/api/groupop` | `op`=`add`/`rename`/`del`/`move`（+`n`,`g`,`dir`） | 标签增 / 改名 / 删 / 上移下移（删标签不删台） |
| POST | `/api/fix` | `action`[,`confirm=1`] | 一键修复，见下表 |
| POST | `/api/pair` | `op`=`lock`(带 `on`)/`newcode`/`unbind`/`forget` | 配对码开关、换码、换手机、取消保护 |
| POST | `/api/wifi` | `ssid`,`pass` | **只发起试连**，立即返回；结果去 `/api/wifi/status` 轮询（25 秒超时回退） |
| POST | `/api/cloud` | `on`=1/0 · `newcode=1` · `host=host:port` · `code=123456` · `open`=0/1 | 远程开关 / 换遥控码 / 换中转服务器（换服务器会立刻重连）/**免配对开关**（关掉立刻发空 retain 撤掉广播） |

**`/api/fix` 的 action**

| action | 做什么 | 要 `confirm=1` |
|---|---|---|
| `replay` | 重开当前台（不清台表） | |
| `reconnect` | 断开再连 WiFi（用 `/wifi.txt` 里的凭据） | |
| `reset_audio` | 停 → 150ms → 重开，播放器卡死时用 | |
| `fix_table` | 重建索引 + 整表重写（清掉文件里的空行）；**一台都读不出来时拒绝执行**，避免把台表写成空文件 | |
| `clear_history` | 清空收听记录 | |
| `reset_stations` | 删台表和 `/tags.txt`，重启后灌回出厂 237 台 / 9 个标签 | ✓ |
| `forget_wifi` | 删 `/wifi.txt`，重启后只剩热点 | ✓ |
| `reboot` | 重启 | |
| `factory` | 删台表 / 记录 / 配置 / 令牌 / 配对，重启 | ✓ |

危险动作一律要求 `confirm=1`，返回 `{"ok":1,"reboot":0|1,"msg":"…"}`；
`reboot=1` 的那些会先回响应、`delay(400)`、再 `ESP.restart()`。

---

## 8.1 跨网控制（MQTT 公共中转）

局域网 API 只在家里有效。要在外地控制，机器**主动**连一台公共 MQTT 服务器（默认
`broker-cn.emqx.io:1883`，可 `POST /api/cloud host=xxx:1883` 换），手机也连同一台，两边在服务器上碰头 —— 家里不用端口映射、不用公网 IP、不用 DDNS。

| 项 | 值 |
|---|---|
| 下行主题（手机 → 机器） | `myradio/<机器码>/c` |
| 上行主题（机器 → 手机） | `myradio/<机器码>/r` |
| 机器码 | WiFi STA MAC 的**后 6 位**十六进制（12 位全码在 `/api/cloud` 的 `devid`） |
| 遥控码 | 6 位数字，存 `/cloud.txt`；`POST /api/cloud newcode=1` 换一串 |
| 传输 | 机器走裸 TCP 1883（`esp_mqtt_client`）；App 走 WebSocket `wss://broker-cn.emqx.io:8084/mqtt` |
| 遗嘱（LWT） | `{"off":1}` 发到 `/r`，机器掉线手机立刻知道，不用等超时 |
| 在线广播（1.19.0） | `myradio/dev/<6位短码>`，**retain**，连上就发、之后每 30 秒一次：`{"d","id","n","fw","open","up","batt"}` |
| 免配对时的遥控码 | `FNV-1a32(盐 "MyRadio-1" + 12位设备码全大写) % 900000 + 100000` |

命令体直接复用局域网 API 的**路径 + form 参数**，省得再设计一套：

```json
{"i":12,"k":"482913","m":"POST","p":"/api/vol","b":"v=50"}
```

```json
{"i":12,"s":200,"d":{"ok":1}}
```

- `i` 自增序号，手机靠它把响应配回请求；`s` 是状态码（403 = 遥控码不对，400 = 路径/参数错）。
- 机器侧 `cloud.cpp` 收包 → 单条队列 → **主循环**执行（不能在 MQTT 任务里碰音频/台表）。
- 动作走 `api.cpp`，和局域网 handler 同一份实现 —— 远程「下一台」和家里点「下一台」行为完全一致。
- **没有把 HTTP 转发到 127.0.0.1**：本机 WebServer 是同步的，主循环里再请求自己必然超时。

**免配对怎么用**：机器开着 `open=1` 时，`runCmd()` 除了存盘那串随机码，还认 `deriveFrom(devid)`
算出来的码（所以 App 不问你码也能连）。`devid` 是 12 位 WiFi MAC，短码是它的后 6 位；
`myradio/dev/+` 上那条 retain 消息的 `id` 字段就是 `devid`。关掉 `open` 时发空 retain 注销，
并立刻只认随机码。**算法是公开的** —— 知道 `devid` 的人就能控，这是「免配对」的代价，见 9 节第 23 条。

**远程版首开那扇门（1.19.1）**：`gen_remote.py` 在 `webapp.h` 末尾插一段遮罩，
它订 `myradio/dev/+` 把在线机器列出来（**自己轮询** `CLOUD.S.devs`，不占 `S.onDevs` ——
那个槽留给设置页的「我的机器」）。免配对的机器点一下 → `pickDev()` 算码 → 连上 → `location.reload()`；
关掉免配对的标「要输码」，预填前 6 位让人补后 6 位；localStorage 里已有 `dev`+`code` 的直接跳过。

支持的路径：`/api/screen` `/api/stations` `/api/diag` `/api/health` `/api/state` `/api/batt` `/api/info` `/api/step`
`/api/play` `/api/pause` `/api/mute` `/api/vol` `/api/group` `/api/set`（`bright` / `offmin` / `screen`）
`/api/fix`（`replay` / `reconnect`）`/api/cloud`（只允许 `newcode`）`/api/auth` `/api/pair`（占位）。

**不支持的**：`/api/add` `/api/edit` `/api/del` `/api/move` `/api/mvgroup` `/api/groupop`（改台表）、
`/api/wifi*` `/api/scan`（配网）、`/api/fix` 里会重启的那几个；`/api/history` 目前回空
（记录还在机器上，只是没走中转）。关远程也不允许远程关 —— 一关就回不了话。

回归（不用真机）：

```bash
node tools/mqtt_broker_sim.js     # 本地假服务器 + 假机器（ws://127.0.0.1:8083）
node tools/verify_cloud.js        # 53 条：配对解析 / varint 编码 / 9KB 大消息 / 403 / 并发 / 广播 / 派生码
node tools/verify_batt.js         # 71 条：曲线单调性 / MP3 帧头 / 播报文件大小 / 固件接线（1.19.0）
node tools/verify_remote.js       # 21 条：首屏自动列出 2 台 → 点一下就连 / 关免配对退回手输 → 首页·台表·设置
```

## 8.2 装成手机 App（APK）

随包给的 `MyRadio-1.22.0.apk`（131 KB，minSdk 23 / Android 6.0 起，自签）。
装的时候手机要允许一次「未知来源」。

**1.42.0 起配网改成「填表 + 发送」**（用户明确要求去掉一键配网）：
首页「设置 WiFi」→ `provProbe` 探测手机到不到机器 → 到了就出表单（预填手机当前 SSID），
用户填 WiFi 名字 + 密码（家里路由器 / 手机热点是同一个入口，`btnPhoneHot` 只是提示去哪开）→
`provSend` → 等 `__mrProv({stage:'ok'})` 回新 IP 再 `provFinish` 进遥控页。
**App 不再替手机切网**（`WifiNetworkSpecifier` 那条路删了）；机器热点只用于
「手机够不着机器」的兜底（`need-net` 状态会指路，`openWifiSettings` 跳系统 WiFi 列表）。
网络列表仍在 **App 侧**（`provRescan`），固件 `/api/scan` 与这条链路无关。详见 `android/README.md`。

**同时「离线遥控」按钮也删了**：1.42.0 起远程控制恒开（固件 `cloudSetOn(false)` 直接被拒，
读盘时忽略 `on=0`）—— 人在外地打开 App 自动走远程，不再需要任何入口/开关。

它是**外壳 + 地址簿**，不是把网页塞进 APK —— 原因：界面里 64 处 `/api/...` 都是相对路径，
只有「页面由这台机器发出」时才同源，硬塞进去得重写全部请求前缀 + 处理 CORS + 代理
`/api/screen` 的流。

| 位置 | 行为 |
|---|---|
| 还没配网 | 首页「设置 WiFi」→ 填 WiFi 名字 + 密码（或从手机附近选 / 用手机热点）→「让收音机连上」→ 自动进遥控界面 |
| 家里同 WiFi | mDNS 秒级发现（路由器丢组播就扫 /24 网段，约 3 秒）→ 点一下 → WebView 打开 `http://<IP>/`，就是机器自己发的那份界面 |
| 外地 | 打开 App 自动走远程 —— 内置 `assets/remote.html`（= `MyRadio-远程版.html`），走 MQTT；**「离线遥控」按钮 1.42.0 起删了**，远程控制恒开 |
| 遥控界面里 | 按手机返回键 → 回首页换机器 |

发现细节：mDNS 订 `_http._tcp.`（固件 `MDNS.begin("myradio")` + `addService("http","tcp",80)`）；
扫描兜底并发探 80 端口后 `GET /api/info` 认人（避免把路由器管理页当机器）；
两者都失败的机器可以在首页展开「找不到？手动输地址」。最近 8 台存在 SharedPreferences，
下次开机直连最后那台。

**出包**

工具链是现成的（`C:\Users\lenovo\WorkBuddy\2026-08-07-21-09-35\shouzhang-app\.build-android`：
jdk17 + gradle 8.6 + sdk34，AGP 8.4.0 在 `~/.gradle/caches`）：

```bash
python tools/build_apk.py            # 出包 → 项目根 + 交付目录
python tools/build_apk.py --icon     # 顺带重画桌面图标
python tools/build_apk.py --no-copy  # 只编译
```

源码在 `android/`：`MainActivity.java` 只管 WebView 与跳转，`Discovery.java` 管找机器，
`Provision.java` 管配网，`assets/home.html` 是首页。
**改首页不用重编固件**，重跑 `build_apk.py` 即可；`assets/remote.html` 由脚本从
`MyRadio-远程版.html` 复制，别手改。

改首页之后必跑这两条：

```bash
python tools/verify_provision.py    # 64 项离线断言（headless Chrome --dump-dom）
python tools/gen_app_preview.py     # 出浏览器可点预览 preview/app-setup.html
```

## 8.3 遥控界面（`tools/app/`）—— 1.41.0 起把固件设置全接上

遥控界面**不是** APK 里那份 `assets/home.html`，而是固件自己发的 `src/webapp.h`
（源在 `tools/app/`）。APK 里的 `assets/remote.html` 也是它生成的 —— **改 UI 后必跑
`gen_remote.py` 再打 APK**，否则 APK 里的跨网遥控还是旧界面。

1.41.0 的界面改动：

- **首页把「上一台 / 暂停 / 下一台 + 集控件 + 音量」收进一个「控制收音机」按钮**，
  点一下才展开（展开状态存 `localStorage`）。收起时按钮副标题就是状态：
  `台名 · 音量 N · 正在放`（收音模式下是 `调频 98.4`）。
- **右上角加了配网按钮**（WiFi 图标），没网时闪琥珀 + 红点。
- **设置页补全了固件里剩下的设置**：两路音量、静噪门限、半步进、搜到的台（可点行跳台）、
  重新搜台、已存的 WiFi（3 组，可逐组忘掉）。

对应的固件接口（1.41.0 补齐的部分）：

| 接口 | 说明 |
|---|---|
| `POST /api/vol {v, ch=net\|fm}` | `ch` 指定改哪一路；目标那路不是当前模式时只落盘 |
| `POST /api/squelch {v}` | 静噪门限 0..100（0=关） |
| `POST /api/halfstep {on}` | 半步进 |
| `POST /api/fmscan` / `GET /api/fmstations` | 搜台 / 读清单 |
| `POST /api/fmtune {khz}` | 直接调到某个频率（**只在收音模式下**，否则 409） |
| `GET /api/wifi/list` / `POST /api/wifi/forget {ssid}` | 已存的 WiFi |
| `GET /api/state` | 新增 `sq` `hs` `fmvol` `volmin` `volmax`；1.44.0 再加 `fvolmin` `fvolmax`（两路量程不同，App 必须分开取） |

**收音模式下 App 是能真操作的**：`apiStep` 在 `A.fmMode` 时转成 `fmScanStep`（芯片寄存器）。
这条原来是空转 —— 只动网络流，而收音模式的声音根本不经过 ESP32。
1.43.0 起 `apiPauseToggle` / `apiMuteToggle` 在 FM 下**也是空操作**（只 `actTouch()`）：
芯片没有「暂停」，静音功能整个删了。

改这套 UI 之后必跑：

```bash
node --check tools/app/js/{core,pages,stations,cloud,health}.js   # ★ 先查语法
python tools/verify_appctl.py    # 59 项断言（false fetch 记录 __calls）
python tools/shot_app.py         # 4 张验收图
python tools/gen_webapp.py && python tools/gen_webpreview.py && python tools/gen_remote.py
```

> `gen_webapp.py` **不检查 JS 语法**。1.41.0 之前 `pages.js` 里 `async` 和 `function`
> 被注释隔开、`loadSettings` 少两个 `}`，整份文件解析失败 ⇒ App 一半按钮点不动，
> 而生成脚本一路绿灯。`node --check` 是唯一能在出包前拦住这类问题的东西。

签名：`android/myradio.keystore`（alias `myradio`，密码在 `app/build.gradle`）。
换 keystore 会导致覆盖安装失败，到时得先卸载旧版。

---

## 9. 已知限制

1. **默认自动配对＝同一 WiFi 内任何人都能取令牌**。这是「打开就能用」的取舍。想收紧：设置 → 配对码保护，
   打开后新手机必须输屏幕上的 6 位数字（`hAuth` 里判断 `s_locked`，令牌不落新设备就不给）。
2. **断流处理**：8 秒连不上或 20 秒没声音 → 自动重连 5 次 → 换下一台。串口有日志。
3. **无本地存储播放**，没有 SD 卡功能。
4. **倍速播放没做**：ESP32-audioI2S 的变速是改 I2S 采样率，1.5× 会明显变调，不适合长辈。
5. **字库 6038 字形**（font32 3982 + font16 2056，`gen_font.py` 扫源码里出现过的字），
   自己新加的台名 / 集名里若有冷僻字会显示成空白。
6. **App 是 145KB 的内联 HTML**（CSS/JS/图标全在一份里，零外部请求、断网可用），占的是 PROGMEM/flash，
   不占 RAM；代价是首屏要等一次 ~145KB 的传输（局域网内一两百毫秒）。
7. **屏幕镜像不是逐帧视频**：`/api/screen` 900ms 一次，靠的是「按坐标重绘」而不是拉图，
   所以台名、刻度、状态都会跟，但动画帧（比如缓动过程）不会跟。
8. **收听记录按「标签 + 台名」归属**：改名等于新开一个记录项；最多 48 项、日统计 7 天。
9. **换 WiFi 只试 25 秒**：企业级认证（802.1X）、隐藏 SSID 需要手动填，且超过 25 秒才连上的网络会被判失败并回退。
10. **PCM5102 的 I2S 三个脚没实机确认**（`39/40/41` 是推断值）。表现是「FM 有声音、网络电台没声音」，
    改 `config.h` 里的 `PIN_I2S_BCK/LCK/DIN` 即可，别的都不用动。详见 3.2。
11. **收音只在 WiFi 断开 20 秒后才接管**（`FM_NET_LOST_MS`）。这段时间里屏幕仍显示网络台但不出声，
    嫌久就把这个值调小，嫌切换太急就调大。
12. **收音模式下旋钮调的是频率**，不是台序：慢转一格 1 个台（中波 9kHz / 调频 0.1MHz），
    正常速度 5 个、快转 10 个、甩起来 30 个（`fm.cpp` 的 `radioGear()`）。
    想更粗/更细改 `config.h` 里的 `RADIO_*_STEP`。网络模式下旋钮照旧是换台。
    收音时点一下/长按都**不动**（原来会去播网络台，两路声音会叠）。
13. **集数是生成时写死的**（1.18.0 起按音源整本抓，`count: 0` = 全量）：想加减集数，
    改 `tools/有声书.json` 里的 `skip` / `count` / `rev` 重新生成烧录，机器上换不了集。
    （1.9.0 起一部书只占一个台，加书就是往 `有声书源*.json` 里多一条，台表只多一行。）
14. **有声书记的是「第几集」，不是「这一集听到第几分钟」**（1.7.0 起）：切回来从那一集开头放。
    1.9.0 起这本书的「第几集」存在 `/epis.txt`，一台一本，互不影响。
    评书一集 20~30 分钟，够听。
15. **机器上点一下旋钮：1.6.0 是播放/暂停 → 1.7.0~1.14.0 是换分组 → 1.15.0 起什么都不做**
    （只点亮屏幕）。分组删掉后这个手势没活可派，而「单击再换台」会让误碰直接跳台，
    所以宁可空着。暂停只剩 App 和遥控器。想改就动 `input.cpp` 的单击分支。
16. **专辑靠台名后缀识别**：名字必须是 `书名 001` 这种（空格 + 2~4 位数字）。
    App 里改成「三国演义第六集」就不再成组，会退回一集一格。
17. **跨网控制依赖公共服务器**：用的是免费公共 broker `broker-cn.emqx.io`。它挂了、或者家里外网断了，
    远程就控制不了 —— 局域网和旋钮完全不受影响。想自己搭：`POST /api/cloud host=<你的服务器>:1883`
    （服务器要支持 MQTT 3.1.1，不需要鉴权，靠主题里的机器码 + 遥控码兜住）。
18. **远程只能遥控，不能改台表 / 配网**：`/api/add` `/api/del` `/api/move` `/api/wifi` 这些没在云端实现
    （写盘和试连都需要本地上下文）。记录页在外地看到的是空的。
19. **远程比局域网慢**：MQTT 往返 + 1.6 秒轮询，按下去到屏幕变大约 0.3~1 秒；家里是 0.9 秒轮询。
20. **中波/短波没在实机上验过**：`fm.cpp` 的 AM 分支（`POWER_UP(FUNC=1)`、4kHz 带宽、
    关软静音）只按 AN332 写对了命令与属性号，**这台机器有没有磁棒天线、SW 靠不靠拉杆天线**
    都没实测。表现会是「调频有声、中波没声」—— 那就先看串口 `fmDiag()` 打的波段表和 `am=1`。
21. **中波用中国 9kHz 栅格（531~1602）**：改 `RADIO_MW_LO/HI` 就能切到美制 520~1710/10kHz，
    但栅格一变，加速档位落点也跟着变（步长永远是 `*_STEP × 加速倍数`）。
22. **远程版是个独立 HTML**：`MyRadio-远程版.html` 得存到手机里或挂网页空间，不是自动更新的 ——
    重发固件后如果改了界面，要重新导出一次发给家人。
23. **1.19.0 的「免配对」是拿安全换方便**：派生遥控码是公开算法（FNV-1a + 固定盐），
    谁把机器短码 / 12 位设备码抄走，谁就能控这台机器 —— 而且它同时在 `myradio/dev/+` 上公开广播。
    默认开着是为了「换手机、升级 App 都不用重连」；要收紧就在 App 里关掉（`POST /api/cloud open=0`），
    机器会撤销广播、只认随机码。别指望它防得住熟人。
24. **电量判定阈值是推断值**：`PIN_BATT_ADC=4` / `BATT_DIV=2.0` / `BATT_CAL=1.00` 没在真机上标定过。
    分压电阻装错了，表现是「百分比永远 100%」或「开机就报没电」—— 先看串口那行 `[batt]` 读数。
25. **播报占 142KB flash（rodata）**：`voice_data.h` 把两条 MP3 编进固件。4MB 的 app 分区装得下，
    但 1.19.0 **没在本机编译过**，首次 `pio run` 请看 Flash 占用；嫌大可以把 `VOICE_ENABLE` 设 0
    （功能关掉，但 rodata 仍会被链接器裁掉才算省下来）。
26. **1.19.0 全部改动没编译、没上机**：`battery.cpp` / `voice.cpp` / `audio.cpp` 只做过静态检查和
    离线回归（`verify_batt.js` 71 条 / `verify_cloud.js` 53 条 / `verify_album.js` 84 条 / `verify_app.js` 38 条）。
    播报能不能出声、低电会不会误报，只能真机听、真机看。

---

## 10. 下一步建议

1. 先用 `probe.ino` 把屏幕点亮，再试 I2S 三个脚 —— 这两步过了，剩下都能在桌面上验证
2. 台表稳定后，让家里人在 App 的「电台」页按自己的习惯排一遍序、打好标签（这一版专门为这事做的）
3. 想把界面再改一遍：改 `tools/app/` → `python tools/gen_webapp.py` → 双击 `preview/index.html` 看，
   满意了再烧板子
4. 如果要拆成真正能上架的原生 App，直接照第 8 节的接口写，设备侧不用改
