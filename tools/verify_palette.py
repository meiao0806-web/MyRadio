# -*- coding: utf-8 -*-
"""1.46.4 回归：真机补偿色（信号柱/电量 = 青蓝，网络广播徽标 = 橙）

跑法：python tools/verify_palette.py
盯四件事：
  1) 设计色（预览页基准）没被改；新增的只是 *_PAINT 一层
  2) 所有绘制点都改用了 *_PAINT，ui.cpp 里不再有裸色名
  3) 补偿方向对、幅度有界：青蓝变暗 35~50%、橙只压绿通道 20~40%，红不动
  4) *_PAINT 不会被 gen_webpreview.py 的调色板正则抓走（预览页不被污染）
"""
import io, os, re, sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = io.open(os.path.join(ROOT, 'src', 'config.h'), encoding='utf-8').read()
UI = io.open(os.path.join(ROOT, 'src', 'ui.cpp'), encoding='utf-8').read()

OK = FAIL = 0


def chk(name, cond, extra=''):
    global OK, FAIL
    if cond:
        OK += 1
        print('OK   ' + name)
    else:
        FAIL += 1
        print('FAIL ' + name + ('  << ' + str(extra) if extra != '' else ''))


def define(name):
    m = re.search(r'#define\s+' + name + r'\s+0x([0-9A-Fa-f]{4})', CFG)
    return int(m.group(1), 16) if m else None


def rgb565(v):
    return ((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31


def lum(c):
    def s2l(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = [s2l(x) for x in c]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b=(0, 0, 0)):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


# ---------------- 1) 设计色 = 预览页基准，必须原样 ----------------
chk('设计色 C_COOL 仍是 0x0413（预览页 #00819c）', define('C_COOL') == 0x0413, define('C_COOL'))
chk('设计色 C_BATT_HI 仍是 0x0413', define('C_BATT_HI') == 0x0413, define('C_BATT_HI'))
chk('设计色 C_BAND_BG 仍是 0xAB40（预览页 #ac6900）', define('C_BAND_BG') == 0xAB40, define('C_BAND_BG'))

# ---------------- 2) 补偿色存在且是定值 ----------------
PAINT = define('C_COOL_PAINT')
BATT_P = define('C_BATT_HI_PAINT')
BAND_P = define('C_BAND_BG_PAINT')
chk('C_COOL_PAINT 存在', PAINT is not None, PAINT)
chk('C_BATT_HI_PAINT 存在', BATT_P is not None, BATT_P)
chk('C_BAND_BG_PAINT 存在', BAND_P is not None, BAND_P)
chk('电量高档与信号柱同色（两个 PAINT 相等）', PAINT == BATT_P, (PAINT, BATT_P))

# ---------------- 3) 方向与幅度 ----------------
cool_d, cool_p = rgb565(define('C_COOL')), rgb565(PAINT)
band_d, band_p = rgb565(define('C_BAND_BG')), rgb565(BAND_P)

ld, lp = lum(cool_d), lum(cool_p)
drop = 1 - lp / ld
chk('青蓝变暗（真机量到 #64f0fa，说明屏上被提亮）', lp < ld, (round(ld, 3), round(lp, 3)))
chk('青蓝变暗幅度在 35%~50%（半档，不是一刀砍到黑）', 0.35 <= drop <= 0.50, round(drop, 3))
chk('青蓝仍几乎是纯青（红通道不动）', cool_p[0] <= 8, cool_p)
chk('青蓝仍比 B 少 G（保持 #00819c 的色相方向）', cool_p[1] < cool_p[2], cool_p)

g_drop = 1 - band_p[1] / float(band_d[1])
chk('橙：红通道不动（保住黑字可读性）', band_p[0] == band_d[0], (band_d[0], band_p[0]))
chk('橙：绿通道压掉 20%~40%（"发黄"就是绿顶满）', 0.20 <= g_drop <= 0.40, round(g_drop, 3))
chk('橙到黄的方向变弱（G/R 比下降）', band_p[1] / float(band_p[0]) < band_d[1] / float(band_d[0]),
    (round(band_d[1] / float(band_d[0]), 3), round(band_p[1] / float(band_p[0]), 3)))

# ---------------- 4) 可读性下限 ----------------
c_badge = contrast(band_p, (0, 0, 0))
chk('徽标黑字对比度 ≥3:1（老人要读得出）', c_badge >= 3.0, round(c_badge, 2))
c_bars = contrast(cool_p, (0, 0, 0))
chk('信号柱对黑底对比度 ≥2.8:1（还得看得见）', c_bars >= 2.8, round(c_bars, 2))
chk('徽标底仍明显比黑底亮（不是糊成一块）', lum(band_p) > 0.08, round(lum(band_p), 3))

# ---------------- 5) 绘制点全部改到 PAINT ----------------
chk('ui.cpp 不再有裸 C_COOL', not re.search(r'\bC_COOL\b', UI))
chk('ui.cpp 不再有裸 C_BAND_BG', not re.search(r'\bC_BAND_BG\b', UI))
chk('ui.cpp 不再有裸 C_BATT_HI', not re.search(r'\bC_BATT_HI\b', UI))
chk('信号柱用 PAINT', 'C_COOL_PAINT : C_BAR_BG' in UI)
chk('电量填充用 PAINT', 'low ? C_ERR : C_COOL_PAINT' in UI)
chk('电量高档用 PAINT', 'bar = C_BATT_HI_PAINT;' in UI)
chk('徽标底用 PAINT', 'P->fillRect(x, 2, mw, 20, C_BAND_BG_PAINT);' in UI)
chk('徽标字底也用 PAINT（否则文字后面露一块旧色）',
    'cnTextS(md, x + 4, 4, C_BAND_FG, C_BAND_BG_PAINT, FS16);' in UI)

# ---------------- 6) 预览页不被污染 ----------------
KEY = {'COOL': 'cool', 'BAND_BG': 'bandBg', 'BATT_HI': 'battHi'}
found = {}
for name, hexv in re.findall(r"#define\s+C_([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]{4})", CFG):
    if name in KEY:
        found[name] = hexv
chk('生成脚本的正则能抓到三个设计色', set(found) == set(KEY), sorted(found))
chk('正则抓不到 *_PAINT（COOL_PAINT 不在映射表里）', 'COOL_PAINT' not in found and 'BAND_BG_PAINT' not in found, sorted(found))
chk('设计色抠出来的值没变', found == {'COOL': '0x0413', 'BAND_BG': '0xAB40', 'BATT_HI': '0x0413'}, found)

pv = os.path.join(ROOT, 'preview', 'index.html')
if os.path.exists(pv):
    H = io.open(pv, encoding='utf-8', errors='ignore').read()
    chk('预览页 RAW.cool 仍是 #00819c', '"cool": "#00819c"' in H)
    chk('预览页 RAW.bandBg 仍是 #ac6900', '"bandBg": "#ac6900"' in H)
    chk('预览页里没有本版的补偿值', '00657b' not in H.lower() and 'ab4d00' not in H.lower())

# ---------------- 报告 ----------------
print('')
print('  设计色 → 真机补偿色（RGB565 还原后）')
print('  信号柱/电量  #%02x%02x%02x → #%02x%02x%02x   亮度 %.3f → %.3f（%+.0f%%）'
      % (cool_d + cool_p + (ld, lp, -drop * 100)))
print('  网络广播徽标 #%02x%02x%02x → #%02x%02x%02x   黑字对比度 %.2f:1 → %.2f:1'
      % (band_d + band_p + (contrast(band_d), c_badge)))
print('')
print('通过 %d / 失败 %d' % (OK, FAIL))
sys.exit(1 if FAIL else 0)
