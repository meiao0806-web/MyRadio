#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""verify_resume_sec.py —— 守住「有声书听到第 N 集 x 分 x 秒，换台回来接着播」

    python tools/verify_resume_sec.py

钉住的四件事（都会让这条功能悄悄失效）：
  1. 离开这一台时**立刻**记位置（不能只靠 30 秒定时，否则会退回到整十秒）
  2. 记的是「这一集的第几秒」，不是「这台开机多久」
  3. 暂停的时长不算进去
  4. 只有 mp3 才按字节续（m4a 的 mdat 是裸 AAC 块，没有同步字/长度前缀，从中间解不出来）
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

oks = fails = 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  + ' + msg + (('  ' + extra) if extra else ''))
    else:
        fails += 1
        print('  X ' + msg + (('  ' + extra) if extra else ''))


audio = io.open(os.path.join(SRC, 'audio.cpp'), encoding='utf-8').read()
res = io.open(os.path.join(SRC, 'resume.cpp'), encoding='utf-8').read()
resh = io.open(os.path.join(SRC, 'resume.h'), encoding='utf-8').read()
api = io.open(os.path.join(SRC, 'api.cpp'), encoding='utf-8').read()

print('[1] 离开这一台时立刻记位置')
i = audio.find('static void startIndex(int idx) {')
head = audio[i:i + 420] if i >= 0 else ''
ok('savePosNow()' in head, 'startIndex 开头调了 savePosNow()',
   '' if 'savePosNow()' in head else '→ 只能等 30 秒定时，位置会退到整十秒')
ok('idx != A.cur' in head, '只在**换到别的台**时记（同一本书换集不记）')
call_pos = head.find('savePosNow()')
pos_acur = head.find('A.cur = idx')
ok(call_pos >= 0 and pos_acur >= 0 and call_pos < pos_acur,
   '记位置发生在 A.cur = idx 之前（否则比的是新台）',
   'call=%d acur=%d' % (call_pos, pos_acur))

print('\n[2] 记的是「这一集的第几秒」')
body = audio[audio.find('static void savePosNow() {'):][:900]
ok('s_posBase' in body, '位置 = s_posBase（续播起点）+ 已播秒数')
ok('s_playAt' in body, '用的是真正出声的时刻 s_playAt')
ok('resumeRemember(' in body, '交给 resumeRemember 落盘')
ok('getM4aAsc' in body, '顺手把 ASC 一起记下（以后做 m4a 续播要用）')

print('\n[3] 暂停时长不算进去')
ok('s_pauseAt' in audio, '有暂停时刻变量')
j = audio.find('case CMD_PAUSE:')
seg = audio[j:j + 1400] if j >= 0 else ''
ok('s_pauseAt = millis()' in seg, '暂停时记下时刻')
ok('s_playAt += d' in seg, '恢复时把出声起点往后推（等价于扣掉暂停）')

print('\n[4] 只有 mp3 按字节续')
ok('static inline bool isSeekUrl()' in audio and '".mp3"' in audio, 'isSeekUrl 只认 .mp3')
ok('isM4aSeekUrl' not in audio,
   '没有残留「m4a 也走 Range」的判定',
   '' if 'isM4aSeekUrl' not in audio else '→ m4a 会先静音 12 秒再退回头播，是退步')
ok('m4aFindFrameStart' not in io.open(
    os.path.join(ROOT, 'lib', 'ESP32-audioI2S', 'src', 'Audio.cpp'), encoding='utf-8').read(),
   '库里没有残留的「扫帧边界」代码')
ok('RESUME_MIN_SEC' in audio, '有最短时长门槛（太短就不续）')

print('\n[5] 记录里带了 ASC（为 m4a 续播留的口子）')
ok('aot' in resh and 'ch' in resh and 'rate' in resh, 'resume.h 声明里有 ASC 字段')
ok('(int)s_r[i].rate' in res, '落盘多写三列 ASC')
ok('老文件没有就当 0' in res or 'r.aot = 0' in res, '读老文件缺列时按 0 处理（兼容）')

print('\n[6] App 能读到「听到第几秒」')
ok('"sec"' in api or '\\"sec\\"' in api, '/api/screen 带 sec 字段')

print('\n%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
