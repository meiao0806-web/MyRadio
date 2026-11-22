# -*- coding: utf-8 -*-
"""
verify_irlearn.py —— 1.46.3「遥控器学习模式不再顶掉台名」回归

背景：用户报「黑屏后转一下亮屏，台名显示按 下一台 (1/7)，没有台名了」。
「按 下一台 (1/7)」是遥控器学习提示（inputLearnHint），说明机器处在学习模式，
而学习模式原来只有「按满 7 个红外键」这一条出路 —— 于是台名被永久顶掉。

这一套检查三件事：
  ① 提示字符串确实是「按 <键名> (n/7)」——保证症状能被这条规则认出来
  ② 学习模式有三个出口（学完 / 30 秒超时 / 转旋钮），并且退出不整屏闪
  ③ 开机判定「按住旋钮」要二次确认，不能一读到 LOW 就进学习
  ④ 台名永远画在第一行，学习提示只占第二行
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


IN = src('src/input.cpp')
UI = src('src/ui.cpp')
CFG = src('src/config.h')


def macro(name, text, default=None):
    m = re.search(r'^#define\s+%s\s+([-\w.]+)' % name, text, re.M)
    if not m:
        return default
    v = m.group(1).rstrip('f')
    try:
        return int(v)
    except ValueError:
        return default


IR_N = macro('IR_N', IN, 7)

print('[1] 症状能对上：提示就是「按 <键名> (n/7)」')
chk('IR_N = 7（所以括号里是 /7）', IR_N == 7, IR_N)
chk('提示格式 "按 %s (%d/%d)"',
    'snprintf(b, sizeof(b), "按 %s (%d/%d)", kIrName[s_learnStep], s_learnStep + 1, IR_N);' in IN)
chk('第 1 个键名是「下一台」（用户看到的正是「按 下一台 (1/7)」）',
    re.search(r'kIrName\[IR_N\]\s*=\s*\{\s*"下一台"', IN) is not None)
chk('inputLearnActive() 判的就是 s_learnStep >= 0',
    'bool inputLearnActive() { return s_learnStep >= 0; }' in IN)

print('\n[2] 学习模式必须有出口（原来只有「学完 7 个键」）')
m = re.search(r'static void learnExit\(const char\* why\) \{(.*?)\n\}', IN, re.S)
chk('有 learnExit()', m is not None)
body = m.group(1) if m else ''
chk('learnExit 会清掉 s_learnStep', 's_learnStep = -1;' in body)
chk('learnExit 不置 A.dirty（只重画台名区，不整屏闪）', 'A.dirty' not in body)
chk('学到的键先存盘（irSave）', 'irSave();' in body)

calls = re.findall(r'learnExit\("([^"]+)"\)', IN)
chk('三个出口都在：超时 / 转旋钮（+ 学完走原路径）', len(calls) == 2, calls)
chk('超时出口：30 秒没按遥控器', any('30' in c for c in calls), calls)
chk('转旋钮出口：人在听广播就退出学习', any('旋钮' in c for c in calls), calls)

chk('超时阈值是 30 秒',
    macro('IR_LEARN_IDLE_MS', IN, 0) == 30000, macro('IR_LEARN_IDLE_MS', IN, 0))
chk('inputLoop 顶上按超时判退出',
    re.search(r'void inputLoop\(\) \{[\s\S]{0,300}IR_LEARN_IDLE_MS[^\n]*\n[^\n]*learnExit', IN) is not None)
chk('每按一个红外键就重置超时', 's_learnAt = millis();' in IN and
    re.search(r'if \(s_learnStep >= 0 && s_learnStep < IR_N\) \{\s*\n\s*s_learnAt = millis\(\);', IN) is not None)
chk('转旋钮那条在 d != 0 分支里（真的转才退，不是每帧都退）',
    re.search(r'if \(d != 0\) \{\s*\n\s*if \(ENC_REVERSE\) d = -d;\s*\n\s*actTouch\(\);\s*\n[^\n]*\n\s*if \(s_learnStep >= 0\) learnExit', IN) is not None)

print('\n[3] 开机判定「按住旋钮」要二次确认（误判 = 学习模式开局）')
ib = re.search(r'void inputBegin\(\) \{(.*?)\n\}', IN, re.S)
ibb = ib.group(1) if ib else ''
chk('inputBegin 里读两次 PIN_ENC_SW',
    len(re.findall(r'digitalRead\(PIN_ENC_SW\)', ibb)) >= 2)
chk('两次之间要等一下（>= 300ms，顺手按一下不算）',
    re.search(r'if \(s_lastRaw\) \{ delay\((\d+)\); s_lastRaw = \(digitalRead\(PIN_ENC_SW\) == LOW\); \}', ibb) is not None and
    int(re.search(r'if \(s_lastRaw\) \{ delay\((\d+)\)', ibb).group(1)) >= 300)
chk('只有确认按住才 s_learnStep = 0',
    re.search(r'if \(s_stable\) \{\s*\n\s*s_learnStep = 0;', ibb) is not None)
chk('进学习时也起超时计时', re.search(r's_learnStep = 0;\s*\n\s*s_learnAt\s*=\s*millis\(\);', ibb) is not None)

print('\n[4] 台名永远在第一行（学习提示只占第二行）')
chk('ui.cpp 不再把台名替换成提示',
    'if (inputLearnActive()) raw = inputLearnHint();' not in UI)
chk('主区拿到的是真台名', 'raw = (idx >= 0 && idx < g_stN) ? g_st[idx].name : "没有电台";' in UI)
chk('学习分支先画台名（第一行 36px）',
    re.search(r'if \(learn\) \{\s*\n\s*drawScrolling\(nm, MAIN_Y \+ 8, col, FS36\);', UI) is not None)
chk('学习提示画在第二行（16px，强调色）',
    re.search(r'if \(learn\) \{[\s\S]{0,220}drawScrolling\(inputLearnHint\(\), MAIN_Y \+ MAIN_H - 20, C_ACCENT, FS16\);', UI) is not None)
chk('学习分支画完就 return，不会又盖一层「第N集」',
    re.search(r'if \(learn\) \{[\s\S]{0,260}\n\s*return;\s*\n\s*\}', UI) is not None)
chk('有声书分支不再排学习模式（台名画在它前面了）', 'if (bk >= 0 && !inputLearnActive())' not in UI)
chk('mainScrolling 不再因为学习模式停工（长台名照常滚）',
    re.search(r'if \(!A\.screenOn \|\| A\.fmMode \|\| A\.volHold\) return false;', UI) is not None)

print('\n[5] 防回归：别的模式没被顺带改坏')
chk('音量模式仍然优先占主区', 'if (A.volHold) {' in UI and 'snprintf(vb, sizeof(vb), "音量 %d", showV);' in UI)
chk('FM 分支仍在主区之前 return', UI.find('if (A.fmMode) {') < UI.find('const bool learn = inputLearnActive();'))
chk('版本号已推进（>= 1.46.3）',
    re.search(r'#define\s+FW_VER\s+"1\.46\.[3-9]"', CFG) is not None,
    re.search(r'#define\s+FW_VER\s+"([^"]+)"', CFG).group(1) if re.search(r'#define\s+FW_VER', CFG) else '?')

print('\n' + '=' * 45)
if FAIL:
    print('失败 %d 项：' % len(FAIL))
    for f in FAIL:
        print('  - %s' % f)
    sys.exit(1)
print('全部通过 ✓')
