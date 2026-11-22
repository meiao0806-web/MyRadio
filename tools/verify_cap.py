# -*- coding: utf-8 -*-
"""
1.46.1 离线验证：台表上限 380 -> 500 之后，四处约束都还站得住

「还能加多少台」不是一个拍出来的数，它同时被四样东西卡着，这个脚本全量一遍：

  A) 台表内存   g_st 走 PSRAM，500 x 360 B = 180 KB（8 MB 里 2.2%）
  B) 整表 JSON  /api/stations 把整表拼成一个 String —— 现在先 reserve 再拼，
                峰值 = 长度（不再翻倍），这是台数能干到 500 的前提
  C) 健康档案   HL_MAX=512，台表超过它就静默没有档案 => health.cpp 里 static_assert
  D) 存储       LittleFS 4 MB 分区 / stations.tsv（含 .tmp 与 .bak 三份）

跑法：python tools/verify_cap.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')

fails = oks = 0


def chk(name, got, want):
    global fails, oks
    if got == want:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-52s got=%r want=%r' % (name, got, want))


def chk_true(name, cond, detail=''):
    global fails, oks
    if cond:
        oks += 1
    else:
        fails += 1
        print('  FAIL %-52s %s' % (name, detail))


def src(fn):
    return io.open(os.path.join(SRC, fn), encoding='utf-8').read()


cfg = src('config.h')
main_c = src('main.cpp')
store_c = src('store.cpp')
health_c = src('health.cpp')
api_c = src('api.cpp')
preset = src('preset_stations.h')


def cfgint(k):
    m = re.search(r'#define\s+%s\s+(\d+)' % k, cfg)
    if not m:
        print('  FAIL 找不到宏 %s' % k)
        sys.exit(1)
    return int(m.group(1))


MAX_ST = cfgint('MAX_STATIONS')
HL_MAX = int(re.search(r'#define\s+HL_MAX\s+(\d+)', health_c).group(1))
PRESET_N = int(re.search(r'#define\s+PRESET_ST_N\s+(\d+)', preset).group(1))

# ------------------------------------------------------------
print('\n[1] 宏与源码守卫')
# ------------------------------------------------------------
_fw = re.search(r'#define\s+FW_VER\s+"([^"]+)"', cfg)
_fwv = _fw.group(1) if _fw else '?' 
chk('FW_VER 是合法版本号（%s）' % _fwv, bool(re.match(r'^1\.\d+\.\d+$', _fwv)), True)
chk('MAX_STATIONS = 500', MAX_ST, 500)
chk('出厂台表仍是 307 台（没被顺手改坏）', PRESET_N, 307)
chk('出厂表装得进上限', PRESET_N <= MAX_ST, True)
chk('空位 = 193', MAX_ST - PRESET_N, 193)

chk('Station = name[40] + url[320] = 360 B',
    bool(re.search(r'char\s+name\[40\]', src('state.h'))) and
    bool(re.search(r'char\s+url\[320\]', src('state.h'))), True)

# 1.45.1 起必须走 PSRAM；回到静态数组就会重演「https 全连不上」
chk('g_st 走 PSRAM（MALLOC_CAP_SPIRAM）', 'MALLOC_CAP_SPIRAM' in main_c, True)
all_src = ''
for fn in sorted(os.listdir(SRC)):
    if fn.endswith(('.cpp', '.h')):
        all_src += src(fn)
chk_true('g_st 不再是静态数组', not re.search(r'Station\s+g_st\s*\[', all_src),
         '又变回静态数组了')

chk('loadAll 有上限截断', 'g_stN >= MAX_STATIONS' in store_c, True)
chk('storeAdd 有上限拒绝', store_c.count('g_stN >= MAX_STATIONS') >= 2, True)

chk('health.cpp 有 static_assert 守住 HL_MAX', 'static_assert(MAX_STATIONS <= HL_MAX' in health_c, True)
chk('HL_MAX >= MAX_STATIONS', HL_MAX >= MAX_ST, True)

# reserve 必须在拼接之前
i_res = api_c.find('o.reserve(o.length() + need);')
i_asm = api_c.find('o = "{\\"count\\":";')
chk_true('apiStationsJson 先 reserve 再拼', 0 < i_res < i_asm, 'reserve@%d assign@%d' % (i_res, i_asm))
chk('reserve 的预留量含台名与地址', 'strlen(g_st[i].name) + strlen(g_st[i].url)' in api_c, True)

# ------------------------------------------------------------
print('\n[2] 体积核算（用出厂表逐台算，不用拍的均值）')
# ------------------------------------------------------------
body = preset[preset.find('static const PresetSt kPreset[]'):]
body = body[:body.find('};')]
rows = re.findall(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}', body)
chk('解析出 307 台', len(rows), 307)

urls = [u.encode() for u, n in rows]
names = [n.encode() for u, n in rows]

# 复刻 C++ 的 JSON 骨架：{"i":I,"n":"..","u":"..","q":Q,"ep":E,"t":T}
def st_bytes(i, q=1, ep=0, t=0):
    return (len('{"i":%d,"n":"","u":"","q":%d,"ep":%d,"t":%d}' % (i, q, ep, t))
            + len(names[i]) + len(urls[i]))

cur = sum(st_bytes(i) for i in range(len(rows)))
avg = cur / float(len(rows))
at_max = int(avg * MAX_ST)
CHUNK = 40 * 1024            # 单块音频解码缓冲的量级，用来对比
INTERNAL_PLAY = 165 * 1024   # 1.45.1 实测：播放时内部空闲

print('    当前 JSON %.1f KB（%.1f B/台）-> 500 台估算 %.1f KB' % (cur / 1024.0, avg, at_max / 1024.0))
print('    播放时内部空闲约 %d KB，reserve 之后峰值 = 长度，不再 ×2' % (INTERNAL_PLAY / 1024))

chk('500 台按出厂均值 < 60 KB', at_max < 60 * 1024, True)
chk_true('500 台 < 播放时内部空闲的 60%', at_max < INTERNAL_PLAY * 0.6,
         '%.1f KB vs %.1f KB' % (at_max / 1024.0, INTERNAL_PLAY * 0.6 / 1024))

# 最坏情况：全部是实测见过的最长地址（喜马拉雅 m4a 直链 234 B，jt= 签名不能删）
url_avg = sum(len(u) for u in urls) / float(len(urls))
url_max = max(len(u) for u in urls)
worst = int((avg - url_avg + url_max) * MAX_ST)
print('    最坏现实情况（500 台全是 %d B 长地址）%.1f KB' % (url_max, worst / 1024.0))
chk_true('最坏现实情况也 <= 播放时内部空闲（reserve 后是一次分配）', worst <= INTERNAL_PLAY,
         '%.1f KB vs %.1f KB' % (worst / 1024.0, INTERNAL_PLAY / 1024))

# 结构上限 319 B 只是 url[320] 的容量，不是现实能出现的长度，只提示不断言
theo = int((avg - url_avg + 319) * MAX_ST)
print('    理论天花板（500 台全是 319 B，url[320] 装满了）%.1f KB —— 只在提示里，不断言'
      % (theo / 1024.0))

# tsv：标签名 + 两个 tab + 换行；留 .tmp / .bak 两份
tsv_cur = sum(len(u) + len(n) + 4 for u, n in zip(urls, names)) + len(rows) * 9
tsv_max = int(tsv_cur / float(len(rows)) * MAX_ST)
fs_total = 4 * 1024 * 1024                      # partitions.csv: littlefs 0x400000
print('    stations.tsv 当前 %.1f KB -> 500 台约 %.1f KB（分区 4 MB，留三份也够）'
      % (tsv_cur / 1024.0, tsv_max / 1024.0))
chk_true('tsv x3 < 分区 10%', tsv_max * 3 < fs_total * 0.1, '%d vs %d' % (tsv_max * 3, int(fs_total * 0.1)))

# ------------------------------------------------------------
print('\n[3] 结论')
# ------------------------------------------------------------
print('    还能加 %d 台（%d -> %d）' % (MAX_ST - PRESET_N, PRESET_N, MAX_ST))
print('    内存 %.1f KB（PSRAM 8 MB 的 %.1f%%）'
      % (MAX_ST * 360 / 1024.0, MAX_ST * 360 / (8 * 1024 * 1024) * 100))
print('    再往上（>600 台）要做的是：把 /api/stations 改成分块 sendContent，而不是改宏')

print('\n通过 %d / 失败 %d' % (oks, fails))
sys.exit(1 if fails else 0)
