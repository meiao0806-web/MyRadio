# -*- coding: utf-8 -*-
"""
verify_resume.py —— 1.46.2「有声书断点续播」回归

三块：
  ① 秒 → 字节的换算（Range 定位的全部数学就在这一行，必须和真文件对得上）
  ② resume.cpp 的存取/淘汰语义（按源码结构 + python 复刻跑一遍）
  ③ 音频侧的接线守卫：起播 get → setRangeFrom → connecttohost；
     续播没出声要退回从头；换集要清断点；只有 mp3 才续
"""
import io
import os
import re
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL = []


def chk(name, cond, extra=''):
    if cond:
        print('  OK   %s' % name)
    else:
        print('  FAIL %s %s' % (name, extra))
        FAIL.append(name)


def src(rel):
    return io.open(os.path.join(BASE, rel), encoding='utf-8').read()


A = src('src/audio.cpp')
R = src('src/resume.cpp')
H = src('src/resume.h')
CFG = src('src/config.h')
LIB = src('lib/ESP32-audioI2S/src/Audio.cpp')
LIBH = src('lib/ESP32-audioI2S/src/Audio.h')


def cfg(name, default=None):
    m = re.search(r'^#define\s+%s\s+([-\w.]+)' % name, CFG, re.M)
    if not m:
        return default
    v = m.group(1).rstrip('f')
    try:
        return float(v) if '.' in v else int(v)
    except ValueError:
        return default


print('[1] 秒 → 字节')
# 与 audio.cpp 里那一行同构：rangeFrom = sec * kbps * 1000 / 8
def to_bytes(sec, kbps):
    return sec * kbps * 1000 // 8


chk('60 秒 @32kbps = 240000 字节（32kbps = 4KB/s）', to_bytes(60, 32) == 240000)
chk('600 秒 @32kbps = 2400000 字节', to_bytes(600, 32) == 2400000)
chk('1 小时 @64kbps = 28800000 字节', to_bytes(3600, 64) == 28800000)
# 真文件对照：民国奇案之枪毙刘汉臣第 1 集 12,004,877 B；32kbps 全程 ≈ 3000 秒（50 分钟）
chk('枪毙刘汉臣第 1 集（12004877 B）在 32kbps 下约 50 分钟',
    2900 <= 12004877 * 8 / 32000 <= 3100, 12004877 * 8 / 32000)
chk('换算用 64 位中间量（1 小时 320kbps 不会溢出 32 位）',
    'uint64_t)rsec * (uint64_t)rkbps * 1000ull / 8ull' in A)

print('\n[2] resume.cpp 语义')
chk('最多记 8 部书', re.search(r'#define\s+RS_MAX\s+8\b', R) is not None)
chk('落盘格式：book\\tep\\tsec\\tkbps', 'f.printf("%d\\t%d\\t%d\\t%d\\n"' in R)
chk('载入时拒绝残缺行', "if (t3 <= 0) continue;" in R)
chk('载入时拒绝负值', 'if (r.book >= 0 && r.ep >= 0 && r.sec >= 0) s_n++;' in R)
chk('改动后 2 秒合并写盘（不每块都写 flash）',
    re.search(r'#define\s+RS_SAVE_MS\s+2000', R) is not None and 'if (s_dirty && millis() - s_dirtyAt > RS_SAVE_MS)' in R)
chk('换集/播完可以清掉断点', 'void resumeClear(int book)' in R)
chk('对外只有 5 个接口（begin/remember/get/clear/tick）',
    all(k in H for k in ('resumeBegin', 'resumeRemember', 'resumeGet', 'resumeClear', 'resumeTick')))

# 复刻：写入 → 读回 → 淘汰
class Rs:
    def __init__(self):
        self.d = {}

    def remember(self, book, ep, sec, kbps):
        if book < 0 or ep < 0 or sec < 0:
            return
        if book not in self.d and len(self.d) >= 8:
            victim = min(self.d.items(), key=lambda kv: kv[1][1])[0]   # sec 最小
            del self.d[victim]
        self.d[book] = (ep, sec, kbps if kbps > 0 else self.d.get(book, (0, 0, 0))[2])

    def get(self, book):
        return self.d.get(book)


rs = Rs()
rs.remember(3, 0, 65, 32)
chk('记一次再取回：ep=0 sec=65 kbps=32', rs.get(3) == (0, 65, 32), rs.get(3))
rs.remember(3, 1, 4, 32)
chk('同书换集会覆盖（不会两条并存）', rs.get(3) == (1, 4, 32), rs.get(3))
rs.remember(3, 1, 9, 0)
chk('没拿到码率时保留旧码率（否则下次没法定位）', rs.get(3)[2] == 32, rs.get(3))
chk('没记过的书取不到', rs.get(99) is None)
for b in range(1, 9):
    rs.remember(b, 0, 100 + b, 32)
rs.remember(42, 0, 1, 32)
# 8 条满后：进来第 9 条时，sec 最小的那条被顶掉（这里最小是刚满 8 条时的书 3 已先被顶，
# 所以 42 进来顶掉的是剩下的 sec 最小者 = 书 1）
chk('满 8 条后淘汰 sec 最小的那条', 42 in rs.d and 1 not in rs.d and len(rs.d) == 8, sorted(rs.d))

print('\n[3] 音频侧接线')
chk('起播前先查断点', A.find('resumeGet(bk, &rep, &rsec, &rkbps)') > 0)
chk('查到就用同一个集号（集号不同不续，避免串集）', 'rep == s_epiE' in A)
chk('太短的（<20 秒）不续',
    'rsec >= RESUME_MIN_SEC' in A and cfg('RESUME_MIN_SEC') == 20, cfg('RESUME_MIN_SEC'))
chk('没有码率就不续（算不出字节）', 'rkbps > 0' in A)
chk('只有 mp3 才续（m4a 的 moov 在文件头）', 'static inline bool isSeekUrl()' in A and '".mp3"' in A)
chk('先 stopSong 再 setRangeFrom 再 connecttohost',
    re.search(r's_audio\.stopSong\(\);\s*\n\s*s_audio\.setRangeFrom\(rangeFrom\);\s*\n\s*s_audio\.connecttohost\(s_url\);', A) is not None)
chk('续播没出声 12 秒退回从头', 'RESUME_FALLBACK_MS' in A and cfg('RESUME_FALLBACK_MS') == 12000)
chk('回退时把 Range 清零（否则会一直要同一个位置）', A.count('s_audio.setRangeFrom(0);') >= 1)
chk('换集/播完清掉断点', 'if (s_epiB >= 0) resumeClear(s_epiB);' in A)
chk('只有真的在出声才记进度（暂停/沙沙/播报都不记）',
    's_epiB >= 0 && s_gotAudio && !s_paused && !s_noiseOn && !s_voiceOn' in A)
chk('进度 = 续播起点 + 本次已播秒数', 's_posBase + (int)((pnow - s_playAt) / 1000)' in A)
chk('续播回退超时小于「死链」35 秒（否则永远轮不到回退）',
    cfg('RESUME_FALLBACK_MS') < 35000)

print('\n[4] 音频库：HTTP Range')
chk('请求头里加 Range', '"Range: bytes=%lu-' in LIB)
chk('Range 只发一次（不然重连一直从中间开始）', 'm_rangeFrom = 0;                    // one-shot' in LIB)
chk('Audio.h 有 setRangeFrom', 'void setRangeFrom(uint32_t b) { m_rangeFrom = b; }' in LIBH)
chk('Range 头拼在 GET 里（不是别的地方）',
    re.search(r'GET [\s\S]{0,900}Range: bytes=', LIB) is not None)

print('\n[5] 诊断可见')
NET = src('src/net.cpp')
chk('/api/diag 带 ep（第几集）', 'ep\\":' in NET and 'audioEpNo()' in NET)
chk('/api/diag 带 pos（第几秒）', 'pos\\":' in NET and 'audioPosSec()' in NET)
chk('/api/diag 带 rs（这次是不是续播）', 'rs\\":' in NET and 'audioResumed()' in NET)

print('\n' + '=' * 45)
if FAIL:
    print('失败 %d 项：' % len(FAIL))
    for f in FAIL:
        print('  - %s' % f)
    sys.exit(1)
print('全部通过 ✓')
