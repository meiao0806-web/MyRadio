# -*- coding: utf-8 -*-
"""
1.46.29 离线验证：音量下限 4 + 调完音量不再「空一下」

用户原话（1.46.29）：
  · 「声音下限改为4」
  · 「调完音量会空一会才继续播，缩短空的时间」

第二句的机理：音量一改 → storeSaveDebounced() → 停手 1.5 秒后 storeSaveCfg()
    写 LittleFS。写 flash 要「擦 4KB 块 + 写元数据」，这几十毫秒里 flash cache
    是停的，而音频任务的代码也在 flash 上 ⇒ 解码停一下 ⇒ I2S 断流。
    时间点又刚好卡在「手一松」那一刻，听起来就是「调音量把声音弄断了」。

本脚本查的都是**能一眼看假的硬事实**：
  A) 两路下限都是 4，且没有任何一处还留 6
  B) 音量那条路已经不走 LittleFS（改 NVS），其它设置只在「没声」时写
  C) 断电前一定强写一次（否则「调完音量关机」就丢）
  D) 开机顺序：cfg → NVS 音量（后者赢）

跑法：python tools/verify_quiet.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

ok_n = bad_n = 0


def chk(name, got, want):
    global ok_n, bad_n
    if got == want:
        ok_n += 1
        print('  OK   %s' % name)
    else:
        bad_n += 1
        print('  FAIL %s  期望 %r 实际 %r' % (name, want, got))


def rd(f):
    return io.open(os.path.join(SRC, f), encoding='utf-8').read()


cfg = rd('config.h')
aud = rd('audio.cpp')
audh = rd('audio.h')
sto = rd('store.cpp')
stoh = rd('store.h')
mn = rd('main.cpp')
bat = rd('battery.cpp')


def macro(k, src=None):
    m = re.search(r'#define\s+%s\s+(-?\d+)' % k, src if src is not None else cfg)
    return int(m.group(1)) if m else -999


print('=== 1) 音量下限 4 ===')
chk('FVOL_MIN = 4（收音那一路）', macro('FVOL_MIN'), 4)
chk('NVOL_MIN = 4（网络那一路）', macro('NVOL_MIN'), 4)
chk('上限没被顺手改掉：FVOL_MAX = 63', macro('FVOL_MAX'), 63)
chk('上限没被顺手改掉：NVOL_MAX = 21', macro('NVOL_MAX'), 21)
chk('config.h 里没有「下限锁 6」的旧说法', '下限锁 6' in cfg, False)
chk('audio.cpp 的夹取用的是 NVOL_MIN（不写死数字）',
    'if (v < NVOL_MIN) v = NVOL_MIN;' in aud, True)
chk('audioSetVol 网络那一路也用 NVOL_MIN 夹',
    'v = constrain(v, NVOL_MIN, NVOL_MAX);' in aud, True)

print('=== 2) 音量不再写 LittleFS ===')
chk('audio.h 有 audioWriteSafe()', 'bool audioWriteSafe();' in audh, True)
chk('audio.cpp 实现了 audioWriteSafe()', 'bool audioWriteSafe() {' in aud, True)

aw = aud[aud.find('bool audioWriteSafe() {'):]
aw = aw[:aw.find('\n}')]
chk('  FM 模式算安全（芯片模拟输出，不占 I2S）', 'A.fmMode' in aw, True)
chk('  暂停 / 念播报 / 沙沙 算安全', ('s_paused' in aw) and ('s_voiceOn' in aw) and ('s_noiseOn' in aw), True)
chk('  其余情况按「有没有在出声」判', 'return !audioIsPlaying();' in aw, True)

volt = sto[sto.find('void storeSaveVolFast() {'):]
volt = volt[:volt.find('\n}')]
chk('storeSaveVolFast 走 NVS（Preferences putBytes）', 'putBytes' in volt, True)
chk('storeSaveVolFast 里没有 LittleFS 写盘', ('storeSaveCfg' in volt) or ('LittleFS' in volt), False)
chk('一次写两个字节（vol + fmvol 不写两遍）', 'uint8_t b[2]' in volt, True)

# 音量那条链上不许再调用旧的 debounce
cmdvol = aud[aud.find('case CMD_VOL:'):]
cmdvol = cmdvol[:cmdvol.find('break;')]
chk('CMD_VOL 已经改调 storeSaveVolFast', 'storeSaveVolFast()' in cmdvol, True)
chk('CMD_VOL 不再调 storeSaveDebounced', 'storeSaveDebounced' in cmdvol, False)

fm = aud[aud.find('void audioSetVol(int v) {'):]
fm = fm[:fm.find('\n}')]
chk('FM 那一路的音量也改调 storeSaveVolFast', 'storeSaveVolFast()' in fm, True)
chk('FM 那一路不再调 storeSaveDebounced', 'storeSaveDebounced' in fm, False)

print('=== 3) 其它设置只在「没声」的时候写 ===')
tick = sto[sto.find('void storeTick() {'):]
tick = tick[:tick.find('\n}')]
chk('storeTick 里有 audioWriteSafe 闸门', 'audioWriteSafe()' in tick, True)
chk('闸门在 storeSaveCfg 之前（不是写完才发现不该写）',
    0 < tick.find('audioWriteSafe') < tick.find('storeSaveCfg'), True)
chk('被闸住时直接 return（不落盘）',
    re.search(r'if \(!audioWriteSafe\(\)\) return;', tick) is not None, True)
chk('合并期还是 1.5 秒（STORE_DEBOUNCE_MS = 1500）',
    re.search(r'#define STORE_DEBOUNCE_MS\s+1500', sto) is not None, True)

print('=== 4) 断电前强写一次 ===')
po = bat[bat.find('static void powerOffNow() {'):]
po = po[:po.find('\n}')]
chk('powerOffNow 里调了 storeFlush()', 'storeFlush()' in po, True)
chk('storeFlush 在断电（digitalWrite(15, LOW)）之前',
    0 < po.find('storeFlush') < po.find('digitalWrite(15, LOW)'), True)
flush = sto[sto.find('void storeFlush() {'):]
flush = flush[:flush.find('\n}')]
chk('storeFlush 忽略 dirty、直接写 cfg',
    ('s_dirty = false;' in flush) and ('storeSaveCfg();' in flush), True)

print('=== 5) 开机顺序：cfg 先读，NVS 音量后盖 ===')
chk('store.h 声明了 storeLoadVolNvs', 'void storeLoadVolNvs();' in stoh, True)
chk('main.cpp 调了 storeLoadVolNvs', 'storeLoadVolNvs();' in mn, True)
chk('它在 storeLoadCfg 之后（否则会被 cfg 里的旧音量盖回去）',
    0 < mn.find('storeLoadCfg();') < mn.find('storeLoadVolNvs();'), True)
ld = sto[sto.find('void storeLoadVolNvs() {'):]
ld = ld[:ld.find('\n}')]
chk('NVS 读不到就原样返回（老机器第一次跑新固件不会把音量清零）',
    re.search(r'if \(n != sizeof\(b\)\) return;', ld) is not None, True)
chk('读出来还要按上下限夹一次', 'constrain((int)b[0], NVOL_MIN, NVOL_MAX)' in ld, True)

print('\n%d 通过 / %d 失败' % (ok_n, bad_n))
sys.exit(0 if bad_n == 0 else 1)
