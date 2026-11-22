# -*- coding: utf-8 -*-
"""1.46.52 m4a 续播的静态回归。

这些事桌面上跑不出来（要真机、要真网络），只能靠结构断言钉住 ——
其中「ASC 必须在最后一次 stopSong() 之后才设」这条，是我这次真栽过的：
stopSong() 开头无条件清 ASC（1.46.21 防「换一本书就崩」），
设早了尾巴模式下会拿默认 2ch/44100 去解 22050Hz 的蜻蜓音频，每帧都错。
"""
import io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
oks, fails = 0, 0


def ok(c, msg, extra=''):
    global oks, fails
    if c:
        oks += 1
        print('  OK  ' + msg)
    else:
        fails += 1
        print('  FAIL ' + msg + (('  << ' + str(extra)) if extra else ''))


def read(*p):
    return io.open(os.path.join(ROOT, *p), encoding='utf-8').read()


a = read('src', 'audio.cpp')
h = read('src', 'm4aseek.h')
c = read('src', 'm4aseek.cpp')
la = read('lib', 'ESP32-audioI2S', 'src', 'Audio.cpp')
lh = read('lib', 'ESP32-audioI2S', 'src', 'Audio.h')

print('[1] 固件侧：m4a 走的是「取头算字节」这条路，不是按码率估')
ok('static inline bool isM4aUrl()' in a, '有 isM4aUrl()')
ok('m4aSeekOffset(s_url, rsec, &ms)' in a, '起播时调了 m4aSeekOffset')
ok('s_audio.setM4aTailMode(m4aTailSz' in a, '调了 setM4aTailMode')
ok('m4aTailSz = ms.total - ms.absPos' in a, '尾巴长度 = 总长 - 起点字节')
ok('rangeFrom = ms.absPos' in a, 'Range 起点用的是算出来的绝对字节')

print('\n[2] ⭐ ASC 必须在最后一次 stopSong() 之后才设（这次栽过的坑）')
# 最后一次 stopSong() 在 connecttohost 之前那一行
i_conn = a.find('s_audio.connecttohost(s_url)')
i_last_stop = a.rfind('s_audio.stopSong()', 0, i_conn)
i_asc = a.find('s_audio.setM4aRawParams(m4aAot, m4aCh, m4aRate)')
ok(i_last_stop > 0, '找到 connecttohost 前那次 stopSong（位置 %d）' % i_last_stop)
ok(i_asc > 0, '找到 setM4aRawParams(m4aAot, ...)（位置 %d）' % i_asc)
ok(i_asc > i_last_stop, 'ASC 是在那次 stopSong 之后设的', 'asc=%d stop=%d' % (i_asc, i_last_stop))
ok(i_asc < i_conn, '而且在 connecttohost 之前')
# 确认库里 stopSong 真的会清 ASC（否则这条断言没意义）
i_ss = la.find('uint32_t Audio::stopSong')
seg = la[i_ss:i_ss + 1200]
ok('m_aacBaseRate = 0' in seg, '库里 stopSong() 确实清 m_aacBaseRate（所以顺序很要紧）')

print('\n[3] 取头要在解码器停掉之后（否则 PSRAM / TLS 抢不动）')
i_seek = a.find('s_audio.stopSong();\n        M4aSeek ms;')
ok(i_seek > 0, 'stopSong() 紧贴在 m4aSeekOffset 之前')
i_seek2 = a.find('m4aSeekOffset(s_url')
ok(i_seek2 > i_seek > 0, '顺序正确')

print('\n[4] 设置顺序：stopSong → setRangeFrom → setM4aTailMode → connecttohost')
i_rf = a.rfind('s_audio.setRangeFrom(rangeFrom)')
i_tm = a.rfind('s_audio.setM4aTailMode')
ok(i_last_stop < i_rf < i_tm < i_conn,
   '四步顺序对', 'stop=%d rf=%d tm=%d conn=%d' % (i_last_stop, i_rf, i_tm, i_conn))

print('\n[5] 失败要退回从头播（不能更差）')
ok('evlog("audio", "m4a 定位失败，第%d集从头播"' in a, '定位失败有日志')
# rangeFrom 初值 0，只有成功才被赋值
ok('uint32_t rangeFrom = 0;' in a, 'rangeFrom 初值 0（失败就是从 0 开始）')
ok('if (m4aSeekOffset(s_url, rsec, &ms) && ms.total > ms.absPos)' in a,
   '成功条件里还要求 total > absPos（防止算过头）')

print('\n[6] 断点记录：m4a 不依赖码率')
ok('if (kb <= 0 && isM4aUrl()) kb = 1;' in a,
   'm4a 拿不到码率也记一条有效断点（否则永远续不上）')
ok('int kb = curKbps();' in a, '仍用 curKbps() 取真实码率（mp3 需要）')

print('\n[7] m4aseek 模块本身')
ok('bool m4aSeekOffset(const char* url, int sec, M4aSeek* out);' in h, '头文件声明')
for name in ('findChild', 'mdhdTimescale', 'stszSum', 'parseHead', 'httpGetHead'):
    ok(name in c, '有 %s()' % name)
ok('mdhd' in c and 'stsz' in c, '解析 moov 里的 mdhd / stsz')
ok('be32(buf + stsz.body + 4)' in c or 'be32(b + stsz.body + 4)' in c or 'sample_size' in c.lower()
   or 'const uint32_t ss    = be32(b + stsz.body + 4);' in c, '读了 stsz 的 sample_size')
ok('(uint64_t)sec * timescale / 1024ull' in c, '帧号 = 秒 × timescale ÷ 1024')
ok('M4A_SEEK_CAP1' in c and 'M4A_SEEK_CAP2' in c, '两档读取（160KB / 384KB）')
ok('Range: bytes=0-' in c, '发的是 Range 请求（只取头部）')
ok('setInsecure()' in c, 'TLS 用 setInsecure（和库里 clientsecure 口径一致）')
ok('heap_caps_malloc(cap, MALLOC_CAP_SPIRAM)' in c, 'buffer 走 PSRAM（不抢内部堆）')

print('\n[8] 库侧：尾巴模式')
ok('void setM4aTailMode(uint32_t tailBytes, uint32_t startSec)' in lh, 'Audio.h 有 setM4aTailMode')
ok('m_m4aTail' in lh and 'm_m4aTailSize' in lh and 'm_m4aTailSec' in lh, '三个成员都在')
# 尾巴分支必须在 read_M4A_Header 之前拦截
i_rah = la.find('size_t Audio::readAudioHeader')
i_tail = la.find('if(m_m4aTail){', i_rah)
i_m4ah = la.find('read_M4A_Header(InBuff.getReadPtr()', i_rah)
ok(i_tail > 0, 'readAudioHeader 里有尾巴分支')
ok(0 < i_tail < i_m4ah, '尾巴分支在 read_M4A_Header 之前拦截', 'tail=%d m4a=%d' % (i_tail, i_m4ah))
seg = la[i_tail:i_tail + 900]
ok('m_controlCounter = 100' in seg, '尾巴模式直接把控制计数置 100（= 头已就绪）')
ok('m_audioDataStart = 0' in seg, 'm_audioDataStart 相对流起点 = 0')
ok('m_audioDataSize  = m_m4aTailSize' in seg, 'm_audioDataSize = 尾巴长度（EOF 判定要用）')
ok('m_audioCurrentTime = m_m4aTailSec' in seg, 'm_audioCurrentTime 从续播点算（屏上时间对）')
ok('m_m4aTail     = false;' in seg or 'm_m4aTail = false;' in seg, '一次性：用完即清')
ok('m_codec       = CODEC_M4A' in seg or 'm_codec = CODEC_M4A' in seg,
   '强制 codec=M4A（不依赖扩展名判定）')

print('\n[9] 别把尾巴模式带进下一次正常连接')
# setDefaults 里不该清 m_m4aTail（它是「下一次连接的意图」，清了就白设）
i_sd = la.find('void Audio::setDefaults()')
sd = la[i_sd:i_sd + 4000]
ok('m_m4aTail' not in sd, 'setDefaults() 里没有清 m_m4aTail（否则 connecttohost 一开始就失效）')

print('\n[10] 离线算法验证脚本还在且能跑')
ok(os.path.exists(os.path.join(ROOT, 'tools', 'verify_m4aseek.py')), 'tools/verify_m4aseek.py 在')
vy = read('tools', 'verify_m4aseek.py')
ok('cpp_impl' in vy and 'indep_impl' in vy, '有「搬 C++ 逻辑」和「独立实现」两份互校')
ok('反推帧号正是目标帧' in vy, '有「落在帧边界」的判据')

print('\n---------------- 汇总 ----------------')
print('%d / %d 通过' % (oks, oks + fails))
sys.exit(1 if fails else 0)
