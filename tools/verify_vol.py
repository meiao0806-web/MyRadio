# -*- coding: utf-8 -*-
"""verify_vol.py —— 1.44.0 声音链回归（源码级，不需要设备）

钉住的规则（用户 2026-11-02 的指令）：
  · 音量上限 / 数值计算 / 发声逻辑  = 原厂口径
      FM    → SI4732 RX_VOLUME 原生 0..63，直接写，默认 35
      网络  → audioI2S setVolume 原生 0..21，直接送，默认 18
  · 音量下限 = 4（两路都是；1.46.29 由 6 改到 4）
  · 不许再出现「档位」这一层换算

跑法：python tools/verify_vol.py
"""
import io, os, re, sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
okN = failN = 0
fails = []

def read(rel):
    return io.open(os.path.join(BASE, rel), encoding='utf-8', errors='replace').read()

def ok(name, cond, extra=''):
    global okN, failN
    if cond:
        okN += 1
        print('  OK   ' + name)
    else:
        failN += 1
        fails.append(name)
        print('  FAIL ' + name + ('  <' + str(extra) + '>' if extra else ''))

def macro(t, name):
    m = re.search(r'^#define\s+' + name + r'\s+(-?\d+)', t, re.M)
    return int(m.group(1)) if m else None

cfg  = read('src/config.h')
fm   = read('src/fm.cpp')
aud  = read('src/audio.cpp')
pwr  = read('src/power.cpp')
pwh  = read('src/power.h')
net  = read('src/net.cpp')
sto  = read('src/store.cpp')
ui   = read('src/ui.cpp')
core = read('tools/app/js/core.js')
pages= read('tools/app/js/pages.js')
body = read('tools/app/body.html')

print('\n=== 1) 音量域 = 原厂 ===')
ok('FM 下限 = 4（用户要求，1.46.29）',  macro(cfg, 'FVOL_MIN') == 4, macro(cfg, 'FVOL_MIN'))
ok('FM 上限 = 63（SI4732 满档）',      macro(cfg, 'FVOL_MAX') == 63, macro(cfg, 'FVOL_MAX'))
ok('FM 默认 = 50（1.46.65：35 在功放线路上太小）', macro(cfg, 'FVOL_DEFAULT') == 50, macro(cfg, 'FVOL_DEFAULT'))
ok('网络下限 = 4',                     macro(cfg, 'NVOL_MIN') == 4, macro(cfg, 'NVOL_MIN'))
ok('网络上限 = 21（audioI2S 顶）',      macro(cfg, 'NVOL_MAX') == 21, macro(cfg, 'NVOL_MAX'))
ok('网络默认 = 18（原厂 netvolume）',   macro(cfg, 'NVOL_DEFAULT') == 18, macro(cfg, 'NVOL_DEFAULT'))

print('\n=== 2) 旧的「档位」那一层已彻底删掉 ===')
src_all = ''.join(read('src/' + f) for f in sorted(os.listdir(os.path.join(BASE, 'src')))
                  if f.endswith(('.cpp', '.h')) and f != 'webapp.h')
for bad in ['VOL_MIN', 'VOL_MAX', 'VOL_DEFAULT', 'FMVOL_DEFAULT']:
    # 只允许出现在新名字里（FVOL_/NVOL_ 含 VOL_ 子串，用词边界再排一次）
    hit = [l for l in src_all.splitlines()
           if re.search(r'(?<![FN])' + bad + r'\b', l)]
    ok('源码里没有 %s 残留' % bad, len(hit) == 0, hit[:2])
ok('没有 toI2S（档位→库值的换算层）', 'toI2S' not in src_all)

print('\n=== 3) FM：直接写 RX_VOLUME，不换算 ===')
ok('fmSetVolume 直接写芯片值',
   re.search(r'setProp\(PROP_RX_VOLUME,\s*\(uint16_t\)v\);', fm) is not None)
ok('FM 音量已无 *63/ 之类的比例换算', fm.count('* 63 /') == 0, fm.count('* 63 /'))
ok('FM 音量夹在 [FVOL_MIN, FVOL_MAX]',
   'if (v < FVOL_MIN) v = FVOL_MIN;' in fm and 'if (v > FVOL_MAX) v = FVOL_MAX;' in fm)
ok('fmBegin/switchMode 也直接写芯片值',
   'setProp(PROP_RX_VOLUME,            (uint16_t)s_vol);' in fm)

print('\n=== 4) 网络：直接送 setVolume，不换算 ===')
ok('applyVol 直接 setVolume((uint8_t)v)',
   's_audio.setVolume((uint8_t)v);' in aud)
ok('网络音量已无 *21/ 之类的比例换算', aud.count('* 21 /') == 0, aud.count('* 21 /'))
ok('播报用 VOICE_VOL_NET（网络满档）', 'VOICE_VOL_NET' in aud and macro(cfg, 'VOICE_VOL_NET') == 21,
   macro(cfg, 'VOICE_VOL_NET'))

print('\n=== 5) 电源上限：改百分比（两路量程不同，绝对值没法共用）===')
ok('powerVolCapPct() 已改名并暴露', 'powerVolCapPct' in pwh and 'int powerVolCapPct()' in pwr)
ok('旧的绝对档位常量已删', 'PWR_T1_VOL ' not in cfg and 'PWR_FULL_VOL ' not in cfg)
ok('三个上限都是 _PCT', all(('PWR_%s_VOL_PCT' % k) in cfg for k in ('T1', 'T2', 'FULL')))
ok('audio.cpp 用百分比乘自己的量程', 'NVOL_MAX * powerVolCapPct() / 100' in aud)
ok('main.cpp FM 也用百分比乘 63', 'FVOL_MAX * powerVolCapPct() / 100' in read('src/main.cpp'))

print('\n=== 6) 存盘：老配置迁移一次，不重复迁 ===')
ok('有 lvToNet / lvToFm 迁移函数', 'lvToNet' in sto and 'lvToFm' in sto)
ok('落盘带 volscale=2 标记', 'volscale=2' in sto)
ok('读盘按标记判断要不要迁', 'scale2' in sto and 'A.vol   = scale2 ?' in sto)

print('\n=== 7) 界面 / 接口：按当前那一路的量程 ===')
ok('固件音量条按 FM/网络各自的 max 画',
   'const int vmax = A.fmMode ? FVOL_MAX : NVOL_MAX;' in ui)
ok('/api/state 同时送 volmin/volmax 与 fvolmin/fvolmax',
   'fvolmin' in net and 'fvolmax' in net)
ok('/api/vol 按 ch 分别夹', 'A.fmVol = constrain(v, FVOL_MIN, FVOL_MAX);' in net
   and 'A.vol = constrain(v, NVOL_MIN, NVOL_MAX);' in net)
ok('App 有 volMaxOf()（按当前模式取 max）', 'function volMaxOf' in core)
ok('App 音量胶囊/音量条都传了 max', 'volMaxOf(s)' in core)
ok('设置页两条滑条各用各的量程',
   'st.fvolmin, st.fvolmax' in pages and 'st.volmin,  st.volmax' in pages)
ok('body.html 三条滑条兜底量程正确',
   'id="vol" min="6" max="21"' in body and 'id="rgVol" min="6" max="21"' in body
   and 'id="rgFmVol" min="6" max="63"' in body)
ok('body.html 两路「原厂 volume / netvolume」标注没写反',
   '原厂 netvolume，0–21' in body and '原厂 volume，0–63' in body)

print('\n=== 8) 1.46.65：FM「开到最大还是小」 ===')
ok('满电不再压 FM 音量（90% 会把 63 压成 56 = 掉 7dB）',
   macro(cfg, 'PWR_FULL_VOL_PCT') == 100, macro(cfg, 'PWR_FULL_VOL_PCT'))
ok('弱台退单声道：FM_STEREO_MIN_RSSI > 0',
   macro(cfg, 'FM_STEREO_MIN_RSSI') is not None and macro(cfg, 'FM_STEREO_MIN_RSSI') > 0,
   macro(cfg, 'FM_STEREO_MIN_RSSI'))
ok('BLEND_STEREO_THRESHOLD 属性号 = 0x1102（AN332）',
   'PROP_FM_BLEND_STEREO_THRESHOLD = 0x1102' in fm)
ok('fmBegin / switchMode 两处都设（POWER_UP 会清属性）',
   len(re.findall(r'setProp\(PROP_FM_BLEND_STEREO_THRESHOLD', fm)) == 2,
   len(re.findall(r'setProp\(PROP_FM_BLEND_STEREO_THRESHOLD', fm)))
ok('FM 带宽也一起设（弱台窄带更干净）',
   len(re.findall(r'setProp\(PROP_FM_CHANNEL_FILTER', fm)) == 2)
ok('音量写失败会重试（否则「屏上 63、芯片 40」永远不纠正）',
   's_volLost = !setProp(PROP_RX_VOLUME' in fm
   and 's_volLost = false;' in fm
   and 'setProp(PROP_RX_VOLUME, (uint16_t)s_vol)' in fm)

print('\n---------------------------------------------')
print('通过 %d / 失败 %d' % (okN, failN))
if failN:
    print('失败项：')
    for f in fails:
        print('  -', f)
    sys.exit(1)
