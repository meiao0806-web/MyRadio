// ============================================================
//  webapp.h —— 手机 App（单文件网页，由设备自己发出）
//
//  !! 这个文件是生成的，不要手改 !!
//  改 tools/app/ 下的 base.html / style.css / body.html / js/*.js
//  然后跑： python tools/gen_webapp.py
//
//  五个页签：收音机（实时屏幕镜像 + 控制）/ 电台 / 记录 / 体检 / 设置
//  图标来自 lucide-static（MIT），构建时内联成 <symbol>，不依赖任何 CDN。
// ============================================================
#pragma once

static const char WEBAPP_HTML[] = R"HTMLX(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover,interactive-widget=resizes-content">
<meta name="theme-color" media="(prefers-color-scheme: light)" content="#e6e9ef">
<meta name="theme-color" media="(prefers-color-scheme: dark)" content="#e6e9ef">
<meta name="color-scheme" content="light">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="操控收音机">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<meta name="format-detection" content="telephone=no">
<link rel="manifest" href="/manifest.webmanifest">
<link rel="icon" href="/icon.svg" type="image/svg+xml">
<title>操控收音机</title>
<style>
/* ============================================================
我的收音机 · 手机 App
------------------------------------------------------------
视觉语言：新拟物（Neumorphism）。
· 全站只有一个底色，卡片、按钮、滑槽全部「和底同色」——
靠一对双向阴影塑形：左上白光、右下暗影 = 凸起；
反过来两个 inset = 凹陷。没有边框、没有分隔色块。
· 圆角统一走大弧：卡片 24、按钮 18、药丸 999、弹层 28。
新拟物一旦出现直角，立刻退回「线框卡片」。
· 颜色近乎单色（中性灰蓝），主色只是一层薄雾青，
只出现在「正在播放 / 选中 / 主行动」。给多了就成彩色 App 了。
· 凹陷 = 可以输入 / 可以按住的地方（搜索框、滑条槽、分段槽、
开关槽、屏幕槽）；凸起 = 可以点的地方。
· 字号：正文 16/1.6；标题 22 收紧字距；说明 12.5 放松字距；
所有数字 tabular-nums，换数时不跳宽度。
============================================================ */
*{box-sizing:border-box}
:root{
/* 一色到底：底、卡、按钮同色，只靠阴影分前后 */
--paper:#e6e9ef;
--card:#e6e9ef;
--card2:#e9ecf2;      /* 极轻的抬起（hover 用） */
--ne-light:#ffffff;
--ne-dark:#c3c8d3;
--ink:#3c414b;
--ink2:#5c636f;
--ink3:#8d95a3;
--line:#cfd4de;
--line2:#d9dde5;
--brand:#3f7180;
--brand2:#335d69;
--brandSoft:#dde6ea;
--amber:#bb7546;
--amberSoft:#e8e0d9;
--ok:#4a8a63;
--okSoft:#dfe8e2;
--bad:#ab5147;
--badSoft:#ecdedb;
--col:49px;
--r-lg:24px;
--r-md:18px;
--r-sm:14px;
--sh-out:7px 7px 14px var(--ne-dark), -7px -7px 14px var(--ne-light);
--sh-out-sm:4px 4px 9px var(--ne-dark), -4px -4px 9px var(--ne-light);
--sh-out-xs:2px 2px 5px var(--ne-dark), -2px -2px 5px var(--ne-light);
--sh-in:inset 5px 5px 10px var(--ne-dark), inset -5px -5px 10px var(--ne-light);
--sh-in-sm:inset 3px 3px 7px var(--ne-dark), inset -3px -3px 7px var(--ne-light);
--sh-in-xs:inset 2px 2px 4px var(--ne-dark), inset -2px -2px 4px var(--ne-light);
/* 列表行之间的「雕刻线」：上白下暗，不用 border */
--carve:inset 0 -1px 0 rgba(163,172,188,.26);
--ease-out:cubic-bezier(.23,1,.32,1);
--ease-in-out:cubic-bezier(.77,0,.175,1);
--ease-drawer:cubic-bezier(.32,.72,0,1);
--tabh:66px;
}
html{
-webkit-tap-highlight-color:transparent;
-webkit-text-size-adjust:100%;
overscroll-behavior:none;
background:var(--paper);
}
body{
margin:0;
min-height:100dvh;
background:var(--paper);
color:var(--ink);
font:16px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB",
"Microsoft YaHei",system-ui,sans-serif;
letter-spacing:.01em;
-webkit-font-smoothing:antialiased;
}
button{
font:inherit;color:inherit;border:0;background:none;padding:0;
touch-action:manipulation;user-select:none;-webkit-user-select:none;
-webkit-touch-callout:none;cursor:pointer;
}
input,textarea,select{font-size:16px;font-family:inherit}
a{color:var(--brand);text-decoration:none}
.hide{display:none!important}
.ic{width:22px;height:22px;flex:0 0 auto;display:block}
.ic.sm{width:18px;height:18px}
.ic.lg{width:26px;height:26px}
/* 数字统一等宽，读数不抖 */
.tnum,.ix,.cv,.sv,.rt b,.stat .v,.ring .val,.screentip .sp{font-variant-numeric:tabular-nums}
/* ---------------- 外壳 ---------------- */
.app{max-width:520px;margin:0 auto;
padding-bottom:calc(var(--tabh) + env(safe-area-inset-bottom,0px) + 30px)}
/* 离线条：不铺满，做成一条浮在顶上的凸起药丸 */
.offbar{
position:fixed;left:12px;right:12px;top:calc(env(safe-area-inset-top,0px) + 8px);z-index:70;
display:flex;align-items:center;gap:8px;
max-width:496px;margin:0 auto;
padding:8px 10px 8px 14px;border-radius:999px;
background:var(--card);
box-shadow:var(--sh-out-sm);
color:#8a5a30;font-size:13px;font-weight:700;letter-spacing:.01em;
}
.offbar .tx{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.offbar button{
flex:none;border:0;border-radius:999px;background:var(--amber);color:#fff;
padding:6px 14px;font:inherit;font-size:12.5px;font-weight:700;cursor:pointer;
box-shadow:var(--sh-out-xs);
}
.offbar[hidden]{display:none}
body.off .app{padding-top:52px}
/* 1.46.7：跨网输码那一行（凹陷 = 可以输入，与搜索框同一套语言） */
/* 左右 18px = 卡片里所有内容的共同格线（.card-hd / .srow 都是 14~16px 18px）。
少了它，凹陷输入框会顶到卡片边上、圆角被 overflow:hidden 裁掉。 */
.pinrow{display:flex;gap:10px;align-items:center;padding:2px 18px 12px}
/* ⚠️ 必须写 min-width:0：input 的固有宽度（size 默认 20 字符≈200px）会让
flex 子项按 min-width:auto 撑住不缩，结果输入框顶出卡片右边、圆角被裁掉。 */
.pinrow input{flex:1;min-width:0;height:50px;border:0;border-radius:16px;background:var(--card);
text-align:center;font:inherit;font-size:22px;font-weight:800;letter-spacing:10px;
color:var(--ink);outline:none;font-variant-numeric:tabular-nums;
box-shadow:var(--sh-in)}
/* 这里**不能**用 text-indent 补偿字距：Chrome 的 ::placeholder 不认自带的
text-indent:0，会继承外层值，占位文字被顶偏 10px（截图里一眼能看出不居中）。 */
.pinrow input::placeholder{color:var(--ink3);font-size:14px;font-weight:600;
letter-spacing:0}
.pinrow button{flex:none;height:50px;padding:0 20px;border:0;border-radius:16px;
background:var(--brand);color:#fff;font:inherit;font-size:15px;font-weight:800;
cursor:pointer;box-shadow:var(--sh-out-sm)}
.pinrow button:active{background:var(--brand2);box-shadow:var(--sh-in-sm)}
.pinrow button[disabled]{background:var(--card);color:var(--ink3);box-shadow:var(--sh-in)}
.devrow .pn{font-size:11.5px;font-weight:700;color:var(--brand);
background:var(--brandSoft);padding:1px 7px;border-radius:999px;flex:none;
font-variant-numeric:tabular-nums}
/* 我的机器（1.19.0）：一行一台，点一下就切过去 */
.devlist{display:flex;flex-direction:column;gap:8px;padding:4px 0 8px}
.devrow{display:flex;align-items:center;gap:10px;width:100%;text-align:left;
padding:11px 13px;border-radius:14px;border:1px solid var(--line);
background:var(--card2);cursor:pointer;font:inherit;color:var(--ink)}
.devrow .nm{flex:1;font-weight:700;font-size:14.5px;overflow:hidden;
text-overflow:ellipsis;white-space:nowrap}
.devrow .mt{font-size:12px;color:var(--ink3);font-weight:600;flex:none}
.devrow.cur{border-color:var(--ok);background:var(--okbg, #eefaf3)}
.card-hd .hd-r{margin-left:auto;font-size:12px;font-weight:600;color:var(--ink3)}
.card-hd{display:flex;align-items:center}
/* 配网三步引导 */
.steps{display:flex;flex-direction:column;gap:9px;margin:4px 0 14px}
.steps .step{display:flex;gap:10px;align-items:flex-start;font-size:13.5px;line-height:1.55;color:var(--ink2)}
/* 只对「编号」那个 b 生效：句子里的 <b>MyRadio</b> 不能被套成圆形徽标 */
.steps .step > b{
flex:none;width:22px;height:22px;border-radius:50%;background:var(--card);color:var(--brand);
box-shadow:var(--sh-out-xs);
font-size:11.5px;font-weight:700;display:flex;align-items:center;justify-content:center;margin-top:1px;
}
.steps .step > span{flex:1;min-width:0}
.steps .step span b{color:var(--brand);font-weight:700}
/* 未配网 / 被点名的卡片：凹陷一圈，提示该点哪儿 */
.card.needset{background:var(--card);box-shadow:var(--sh-out),inset 0 0 0 2px rgba(187,117,70,.28)}
.card.needset .card-hd{color:#8a5a30}
.card.hot{box-shadow:var(--sh-out),0 0 0 3px rgba(63,113,128,.28);transition:box-shadow .3s}
.page{display:none;padding:0 16px}
.page.on{display:block;animation:pageIn .18s var(--ease-out) both}
@keyframes pageIn{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:translateY(0)}}
/* 页头：不再压发丝线，靠留白和标题字重分层 */
.hdr{
display:flex;align-items:flex-start;gap:10px;
padding:calc(env(safe-area-inset-top,0px) + 22px) 0 14px;
margin-bottom:6px;
}
.hdr > div{min-width:0}
.hdr h1{
margin:0;font-size:22px;line-height:1.15;font-weight:700;letter-spacing:-.4px;
color:var(--ink);
}
.hdr .sub{margin-top:4px;font-size:12.5px;color:var(--ink3);font-weight:600;letter-spacing:.02em}
.hdr .iconbtn{margin-left:auto;margin-top:-4px}
/* 图标按钮：凸起圆片；按下变凹陷 */
.iconbtn{
width:38px;height:38px;border-radius:50%;display:grid;place-items:center;
color:var(--ink2);background:var(--card);
box-shadow:var(--sh-out-sm);
transition:box-shadow .14s ease,transform .1s var(--ease-out),color .12s ease;
}
.iconbtn:active{box-shadow:var(--sh-in-sm);transform:scale(.97)}
.iconbtn.on{color:var(--brand);box-shadow:var(--sh-in-sm)}
/* 卡片：和底色同色的凸起面 */
.card{
background:var(--card);border:0;border-radius:var(--r-lg);
box-shadow:var(--sh-out);
margin-bottom:18px;overflow:hidden;
}
/* 卡片标题：小字 + 短横，落在统一格线上 */
.card-hd{
display:flex;align-items:center;gap:8px;
padding:16px 18px 10px;font-size:11.5px;font-weight:700;color:var(--ink3);
letter-spacing:.08em;
}
.card-hd::before{content:"";width:20px;height:2px;border-radius:2px;background:var(--ink3);
opacity:.5;flex:0 0 auto;margin-right:4px}
.card-hd .r{margin-left:auto;font-weight:600;color:var(--ink3);font-size:12px;letter-spacing:.02em}
/* ---------------- 设备屏幕 ---------------- */
/* 屏幕是一块「按进机身里」的凹槽，里面才是黑色面板 */
.screenwrap{
position:relative;border-radius:22px;padding:11px 11px 24px;
background:var(--card);
box-shadow:var(--sh-in);
overflow:hidden;
}
.screenwrap::after{
content:"MYRADIO · FM 87.5–108.0";
position:absolute;left:0;right:0;bottom:8px;text-align:center;
font:600 8.5px/1 ui-monospace,SFMono-Regular,Menlo,monospace;
letter-spacing:.22em;color:var(--ink3);
}
#scr{
display:block;width:100%;height:auto;border-radius:12px;
background:#000;
box-shadow:0 0 0 2px rgba(60,65,75,.10), 0 6px 18px -8px rgba(60,65,75,.5);
}
.screentip{
display:flex;align-items:center;gap:7px;
padding:12px 4px 0;font-size:12.5px;color:var(--ink3);font-weight:600;letter-spacing:.02em;
}
.screentip .mirror{
color:var(--ink2);font-weight:700;letter-spacing:.06em;
display:inline-flex;align-items:center;gap:5px;
}
.screentip .mirror::before{
content:"";width:7px;height:7px;border-radius:50%;background:var(--ok);
box-shadow:0 0 0 3px rgba(74,138,99,.14);
}
.screentip .mirror.lost{color:var(--bad)}
.screentip .mirror.lost::before{background:var(--bad);box-shadow:0 0 0 3px rgba(171,81,71,.14)}
.screentip .sp{margin-left:auto;display:flex;align-items:center;gap:7px;flex:0 0 auto}
/* 1.17.0 修：台名一长（「播放中 · 中央广播电视总台中国之声 · 第 3 集」）
原来会在这一行折成两行，把下面的控制区顶下去。一行到底，超出打省略号。 */
.screentip #tipTx{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.screentip .dot{width:7px;height:7px;border-radius:50%;background:var(--ink3);flex:0 0 auto}
.screentip .dot.ok{background:var(--ok)}
.screentip .dot.warn{background:var(--amber)}
.screentip .dot.bad{background:var(--bad)}
/* ---------------- 主控制 ---------------- */
/* 三颗都凸起：左右是胶囊，中间是大圆 */
.transport{
display:grid;grid-template-columns:1fr 84px 1fr;align-items:center;
gap:12px;margin:22px 0 4px;
}
.tbtn{
height:56px;border-radius:var(--r-md);background:var(--card);
display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;
padding:0 6px;color:var(--ink2);font-weight:700;font-size:12px;letter-spacing:.02em;
box-shadow:var(--sh-out-sm);
transition:box-shadow .14s ease,transform .1s var(--ease-out);
}
.tbtn:last-child{align-items:center;text-align:center}
.tbtn:active{transform:scale(.97);box-shadow:var(--sh-in-sm)}
.tbtn .ic{color:var(--ink2)}
.tbtn.main{
width:84px;height:84px;border-radius:50%;gap:0;padding:0;
align-items:center;justify-content:center;
background:var(--card);color:var(--brand);
box-shadow:var(--sh-out);
}
.tbtn.main:active{transform:scale(.96);box-shadow:var(--sh-in)}
.tbtn.main .ic{width:30px;height:30px;color:var(--brand)}
/* 暂停态（没在播）：凹陷，一眼看出「这里可以按下去开始」 */
.tbtn.main.paused{box-shadow:var(--sh-in);color:var(--brand2)}
.tbtn.main.paused .ic{color:var(--brand2)}
/* ---------------- 有声书集（一个台 = 一部书） ---------------- */
/* 整条凹进去当「槽」，两颗按钮凸起来：一眼看出按钮能按、中间那块是信息 */
.epbar{
display:grid;grid-template-columns:74px 1fr 74px;gap:8px;align-items:center;
margin:12px 0 2px;padding:8px;border-radius:var(--r-md);
box-shadow:var(--sh-in-sm);
}
.epbar.hide{display:none}
.epbtn{
height:46px;border-radius:14px;background:var(--card);box-shadow:var(--sh-out-sm);
display:flex;flex-direction:column;align-items:center;justify-content:center;gap:1px;
color:var(--ink2);font-size:10.5px;font-weight:800;letter-spacing:.02em;
transition:box-shadow .14s ease,transform .1s var(--ease-out);
}
.epbtn:active{transform:scale(.96);box-shadow:var(--sh-in-sm)}
.epbtn .ic{width:17px;height:17px;color:var(--ink2)}
.epmid{
height:46px;border-radius:14px;background:transparent;
display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;
min-width:0;padding:0 2px;
}
.epmid .line{display:flex;align-items:baseline;gap:6px;max-width:100%}
.epmid b{font-size:14.5px;font-weight:900;color:var(--ink);letter-spacing:.01em}
.epmid em{font-style:normal;font-size:11px;font-weight:700;color:var(--ink3)}
.epmid i{
font-style:normal;font-size:11px;font-weight:700;color:var(--ink2);
max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
}
.epmid:empty i{display:none}
/* 音量 */
.volrow{display:grid;grid-template-columns:40px 1fr 40px;gap:10px;align-items:center;margin:18px 0 4px}
.volrow .iconbtn{width:40px;height:40px}
input[type=range]{
-webkit-appearance:none;appearance:none;width:100%;height:30px;background:none;margin:0;
min-width:0;box-sizing:border-box;display:block;   /* flex 子项默认 min-width:auto 会把卡片撑宽 */
}
input[type=range]::-webkit-slider-runnable-track{
height:12px;border-radius:999px;
background:linear-gradient(to right,var(--brand2) var(--fill,var(--p,50%)),
var(--line2) var(--fill,var(--p,50%)));
/* 只往上下打阴影：对角阴影会在两端圆弧处堆出白角，像根管子 */
box-shadow:inset 0 1px 3px rgba(60,65,75,.20), inset 0 -1px 2px rgba(255,255,255,.75);
}
input[type=range]::-moz-range-track{
height:12px;border-radius:999px;background:var(--line2);
box-shadow:inset 0 1px 3px rgba(60,65,75,.20), inset 0 -1px 2px rgba(255,255,255,.75);
}
input[type=range]::-moz-range-progress{height:12px;border-radius:999px;background:var(--brand)}
input[type=range]::-webkit-slider-thumb{
-webkit-appearance:none;appearance:none;width:24px;height:24px;border-radius:50%;margin-top:-6px;
background:var(--card);border:0;
box-shadow:var(--sh-out-xs),inset 0 0 0 1px rgba(63,113,128,.16);
transition:transform .1s var(--ease-out);
}
input[type=range]:active::-webkit-slider-thumb{transform:scale(1.08)}
input[type=range]::-moz-range-thumb{
width:24px;height:24px;border-radius:50%;background:var(--card);border:0;
box-shadow:var(--sh-out-xs);
}
/* 分组 chips：凸起药丸，选中变凹陷 + 主色文字 */
.chips{
display:flex;gap:10px;overflow-x:auto;padding:14px 16px 18px;margin:0 -16px;
scrollbar-width:none;-webkit-overflow-scrolling:touch;touch-action:pan-x;
}
.chips::-webkit-scrollbar{display:none}
.chip{
flex:0 0 auto;display:flex;align-items:center;gap:6px;white-space:nowrap;
height:40px;padding:0 16px;border-radius:999px;
background:var(--card);border:0;
box-shadow:var(--sh-out-sm);
font-size:14.5px;font-weight:600;color:var(--ink2);
transition:transform .1s var(--ease-out),box-shadow .14s ease,color .12s ease;
}
.chip b{font-weight:600;font-size:11.5px;color:var(--ink3);letter-spacing:.02em}
.chip:active{transform:scale(.97)}
.chip.on{background:var(--card);color:var(--brand);box-shadow:var(--sh-in)}
.chip.on b{color:var(--brand);opacity:.65}
.chip.ghost{background:var(--card);box-shadow:var(--sh-out-xs);color:var(--ink3)}
/* 1.17.0：「有问题 N」——红字；一个都没有时退回灰的，不抢眼 */
.chip.warn{color:var(--bad)}
.chip.warn b{color:var(--bad);opacity:.8}
.chip.warn.zero,.chip.warn.zero b{color:var(--ink3)}
.chip.warn.zero b{opacity:.55}
/* ---------------- 列表 ---------------- */
.list{background:var(--card);border-radius:var(--r-lg);border:0;
box-shadow:var(--sh-out);overflow:hidden;margin-bottom:18px}
.row{
display:flex;align-items:center;gap:10px;
padding:12px 14px;min-height:62px;
background:var(--card);position:relative;
}
/* 行之间用雕刻线（上白下暗），比 1px 灰边干净 */
.row + .row{box-shadow:var(--carve)}
/* 当前台：整行按进去，左下留一颗主色小珠 */
.row.cur{background:var(--card);box-shadow:var(--sh-in-sm)}
.row.cur + .row{box-shadow:var(--carve)}
.list .row{animation:rise .22s var(--ease-out) backwards;animation-delay:calc(var(--i,0) * 20ms)}
@keyframes rise{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:translateY(0)}}
.row.drop-before::before,
.row.drop-after::after{
content:"";position:absolute;left:12px;right:12px;height:3px;border-radius:3px;
background:var(--brand);
box-shadow:0 0 0 3px rgba(63,113,128,.14);
}
.row.drop-before::before{top:0}
.row.drop-after::after{bottom:0}
.row .grip{
width:26px;height:42px;display:grid;place-items:center;color:var(--ink3);
opacity:.55;flex:0 0 auto;touch-action:none;cursor:grab;margin-left:-8px;
}
.row .nm{flex:1;min-width:0}
.row .nm .t{
font-size:17px;font-weight:600;line-height:1.3;letter-spacing:-.1px;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
}
.row .nm .m{margin-top:3px;font-size:12px;color:var(--ink3);font-weight:600;
display:flex;align-items:center;gap:6px;overflow:hidden}
.row .nm .m > span{white-space:nowrap}
.row.cur .nm .t{color:var(--brand)}
.row .ix{
width:22px;text-align:right;font-size:11.5px;color:var(--ink3);font-weight:600;
flex:0 0 auto;margin-right:2px;
}
.row.cur .ix{color:var(--brand);opacity:.7}
.row .more{width:36px;height:36px;border-radius:50%;display:grid;place-items:center;
color:var(--ink3);flex:0 0 auto}
.row .more:active{box-shadow:var(--sh-in-xs)}
.row.dragging{
z-index:20;box-shadow:var(--sh-out);
border-radius:var(--r-md);background:var(--card);
}
.row.drop-target::before{
content:"";position:absolute;left:12px;right:12px;top:0;height:3px;border-radius:3px;
background:var(--brand);
}
/* 记录行右侧：时长用墨色，单位是灰的 */
.rt{text-align:right;flex:0 0 auto}
.rt b{display:block;font-size:15px;font-weight:700;color:var(--ink);letter-spacing:-.1px}
.rt span{display:block;font-size:11px;color:var(--ink3);font-weight:600;margin-top:2px}
.tag{
font-size:10.5px;font-weight:700;padding:2px 8px;border-radius:999px;
background:var(--card);box-shadow:var(--sh-in-xs);color:var(--brand);letter-spacing:.04em;
}
.tag.gray{background:var(--card);box-shadow:none;color:var(--ink3)}
/* ---------------- 1.17.0 台的健康 ---------------- */
/* 行里的小标：连不上（红）/ 没声音（琥珀）/ 正常（绿） */
.hl{
font-size:11px;font-weight:700;padding:1px 7px;border-radius:999px;
white-space:nowrap;flex:0 0 auto;letter-spacing:.02em;
background:rgba(171,81,71,.10);color:var(--bad);
}
.hl.dim{background:rgba(187,117,70,.13);color:var(--amber)}
.hl.ok{background:rgba(74,138,99,.11);color:var(--ok)}
/* 主页屏幕下面那条：电平 + 「在出声」 */
.sndrow{display:flex;align-items:center;gap:9px;padding:10px 4px 0;
font-size:12px;font-weight:700;color:var(--ink3);letter-spacing:.02em}
.sndrow .lv{flex:0 0 54px;display:block;height:6px;border-radius:3px;
background:var(--card);box-shadow:var(--sh-in-xs);overflow:hidden}
.sndrow .lv b{display:block;height:100%;width:0;border-radius:3px;background:var(--ok);
transition:width .12s linear,background .2s ease}
.sndrow .lv b.bad{background:var(--bad)}
.sndrow .lv b.dim{background:var(--amber)}
.sndrow .snd{flex:1 1 auto;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sndrow .snd.ok{color:var(--ok)}
.sndrow .snd.bad{color:var(--bad)}
.sndrow .snd.dim{color:var(--amber)}
.empty{
padding:42px 20px;text-align:center;color:var(--ink3);font-size:14px;line-height:1.7;
}
.empty .ic{width:30px;height:30px;margin:0 auto 10px;color:var(--ink3);opacity:.45}
/* 搜索：一条按进机身里的槽 */
.search{
display:flex;align-items:center;gap:10px;margin:2px 0 14px;
height:48px;padding:0 14px;border-radius:999px;
background:var(--card);border:0;
box-shadow:var(--sh-in);
}
.search input{flex:1;border:0;background:none;outline:none;font-size:16px;color:var(--ink)}
.search input::placeholder{color:var(--ink3)}
.search .ic{color:var(--ink3)}
.search .iconbtn{width:30px;height:30px;box-shadow:none;color:var(--ink3)}
.search .iconbtn:active{box-shadow:var(--sh-in-xs)}
/* 主页底部那张「今天听了多久」 */
.today{display:flex;align-items:center;gap:12px;padding:0 18px}
.tnum{font-size:32px;font-weight:700;letter-spacing:-1.4px;color:var(--ink);
line-height:1.2}
.tnum small{font-size:11.5px;color:var(--ink3);font-weight:600;margin:0 2px;letter-spacing:0}
.lastone{padding:8px 18px 18px;font-size:12.5px;color:var(--ink3);font-weight:600}
/* ---------------- 历史 ---------------- */
.stats{display:grid;grid-template-columns:1.5fr 1fr 1fr;gap:12px;margin-bottom:18px}
.stat{background:var(--card);border-radius:var(--r-lg);border:0;
box-shadow:var(--sh-out);padding:14px 14px;min-width:0}
.stat .k{font-size:11px;color:var(--ink3);font-weight:700;letter-spacing:.12em}
.stat .v{font-size:21px;font-weight:700;margin-top:5px;letter-spacing:-.4px;
white-space:nowrap}
.stat .v small{font-size:10px;font-weight:700;color:var(--ink3);margin:0 2px 0 3px;
letter-spacing:.02em}
/* 主数字：凹进去的一块，数字用主色 */
.stat.hero{background:var(--card);box-shadow:var(--sh-in)}
.stat.hero .k{color:var(--ink3)}
.stat.hero .v{color:var(--brand)}
.stat.hero .v small{color:var(--brand);opacity:.65}
.bars{display:flex;align-items:flex-end;gap:10px;height:112px;padding:12px 18px 10px;
margin:0 0 8px}
.bars .b{flex:1;display:flex;flex-direction:column;align-items:center;gap:8px;height:100%}
.bars .bar{
flex:1;width:100%;max-width:22px;position:relative;min-height:4px;
background:var(--card);border-radius:9px;
box-shadow:inset 2px 2px 4px var(--ne-dark), inset -1px -1px 3px rgba(255,255,255,.7);
transform-origin:bottom;animation:grow .36s var(--ease-out) both;
}
@keyframes grow{from{transform:scaleY(.3);opacity:.5}to{transform:scaleY(1);opacity:1}}
.bars .bar i{position:absolute;left:2px;right:2px;bottom:2px;
background:rgba(63,113,128,.45);border-radius:7px 7px 4px 4px;
box-shadow:inset 0 1px 0 rgba(255,255,255,.4);
transition:height .38s var(--ease-out)}
.bars .bar.now i{background:rgba(187,117,70,.55)}
.bars .lb{font-size:10px;font-weight:700;color:var(--ink3);letter-spacing:.02em}
.bars .b.now .lb{color:var(--amber)}
/* ---------------- 体检 ---------------- */
.health{
display:flex;align-items:center;gap:18px;padding:20px 18px;
background:var(--card);border-radius:var(--r-lg);border:0;
box-shadow:var(--sh-out);
margin-bottom:18px;
}
.ring{position:relative;width:92px;height:92px;flex:0 0 auto}
.ring svg{transform:rotate(-90deg);display:block}
.ring .val{
position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
font-size:26px;font-weight:700;letter-spacing:-1.4px;color:var(--ink);
}
.ring .val small{font-size:11px;font-weight:600;color:var(--ink3);margin-left:2px;margin-top:7px}
.health .txt h2{margin:0 0 5px;font-size:16.5px;font-weight:700;letter-spacing:-.2px}
.health .txt p{margin:0;font-size:13px;color:var(--ink2);line-height:1.62}
/* 1.17.0：台的健康四格 */
.hlgrid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;padding:12px 14px}
.hcell{background:var(--card);border-radius:var(--r-md);box-shadow:var(--sh-out-xs);
padding:10px 4px;text-align:center}
.hcell b{display:block;font-size:19px;font-weight:700;letter-spacing:-.4px;color:var(--ink)}
.hcell span{display:block;font-size:10.5px;font-weight:700;color:var(--ink3);
margin-top:3px;white-space:nowrap}
.hcell.bad b{color:var(--bad)}
.hcell.dim b{color:var(--amber)}
/* 挨个试一遍的进度 */
.prog{height:8px;border-radius:4px;background:var(--card);box-shadow:var(--sh-in-xs);
overflow:hidden;margin:4px 0 10px}
.prog i{display:block;height:100%;width:0;border-radius:4px;background:var(--brand);
transition:width .25s ease}
.scrow{display:flex;justify-content:space-between;font-size:12.5px;font-weight:700;
color:var(--ink3);margin-bottom:4px}
.chk{display:flex;align-items:center;gap:12px;padding:14px 18px}
.chk + .chk{box-shadow:var(--carve)}
/* 状态只给异常上色：全部绿会变成「假仪表盘」 */
.chk .ci{
width:30px;height:30px;border-radius:50%;display:grid;place-items:center;flex:0 0 auto;
background:var(--card);box-shadow:var(--sh-out-xs);color:var(--ink3);
}
.chk.ok .ci{color:var(--ink3)}
.chk.warn .ci{color:var(--amber)}
.chk.bad .ci{color:var(--bad)}
.chk .ct{flex:1;min-width:0}
.chk .ct .t{display:block;font-size:15.5px;font-weight:600;line-height:1.35}
.chk .ct .m{display:block;font-size:12px;color:var(--ink3);margin-top:2px;font-weight:600;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chk .cv{font-size:13px;font-weight:700;color:var(--ink2);flex:0 0 auto;letter-spacing:.02em}
.chk.bad .cv{color:var(--bad)}
.chk.warn .cv{color:var(--amber)}
.chk.ok .cv{color:var(--ink2)}
.alertcard{
display:flex;gap:12px;padding:15px 16px;border-radius:var(--r-lg);
background:var(--badSoft);
box-shadow:var(--sh-out),inset 3px 0 0 var(--bad);
margin-bottom:18px;
animation:pageIn .18s var(--ease-out) both;
}
.alertcard .ic{color:var(--bad);flex:0 0 auto;margin-top:1px}
.alertcard .t{font-size:14.5px;font-weight:700;color:#8d4038}
.alertcard .m{font-size:13px;color:#8d4038;opacity:.9;margin-top:2px;line-height:1.55}
.alertcard.warn{background:var(--amberSoft);box-shadow:var(--sh-out),inset 3px 0 0 var(--amber)}
.alertcard.warn .ic,.alertcard.warn .t,.alertcard.warn .m{color:#7d4a28}
/* ---------------- 通用按钮 ---------------- */
.btn{
display:flex;align-items:center;justify-content:center;gap:8px;
height:50px;border-radius:var(--r-md);font-size:15.5px;font-weight:600;letter-spacing:.02em;
background:var(--card);border:0;color:var(--ink);
box-shadow:var(--sh-out-sm);
transition:box-shadow .14s ease,transform .1s var(--ease-out),color .12s ease;
}
.btn:active{transform:scale(.985);box-shadow:var(--sh-in-sm)}
.btn.pri{background:var(--brand);color:#fff;
box-shadow:var(--sh-out-sm),inset 0 1px 0 rgba(255,255,255,.22)}
.btn.pri:active{background:var(--brand2);box-shadow:var(--sh-in-sm)}
.btn.danger{color:var(--bad);background:var(--card);box-shadow:var(--sh-out-sm)}
.btn.ghost{background:var(--card);color:var(--ink2);box-shadow:var(--sh-out-sm)}
.btn.sm{height:38px;font-size:13.5px;border-radius:999px;padding:0 14px}
.btn:disabled{opacity:.45}
.btnrow{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:18px}
.btnrow.one{grid-template-columns:1fr}
/* 设置行 */
.srow{
display:flex;align-items:center;gap:13px;padding:14px 18px;
width:100%;text-align:left;background:var(--card);
}
.srow + .srow{box-shadow:var(--carve)}
.srow .si{width:34px;height:34px;border-radius:50%;display:grid;place-items:center;
background:var(--card);box-shadow:var(--sh-out-xs);
color:var(--ink2);flex:0 0 auto}
.srow .st{flex:1;min-width:0}
.srow .st .t{display:block;font-size:15.5px;font-weight:600;line-height:1.35}
.srow .st .m{display:block;font-size:12px;color:var(--ink3);margin-top:2px;font-weight:600;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.srow .sv{font-size:13px;color:var(--ink3);font-weight:600;flex:0 0 auto;max-width:44%;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap;letter-spacing:.02em}
.srow .chev{color:var(--ink3);opacity:.45;flex:0 0 auto}
.srow:active{background:var(--card);box-shadow:var(--sh-in-sm)}
.srow.danger .st .t{color:var(--bad)}
/* 设置页：整行竖排（滑条 / 分段选择） */
.srow.col{flex-direction:column;align-items:stretch;gap:12px;padding:15px 18px;min-width:0}
.srow.col .st{flex:none}
.srow.col:active{background:var(--card);box-shadow:none}
/* 分段选择：一条凹槽 + 一块凸起的选中片 */
.seg{display:flex;background:var(--card);border-radius:999px;padding:4px;gap:4px;min-width:0;
box-shadow:var(--sh-in-sm)}
.seg button{
flex:1;min-width:0;border:0;background:transparent;border-radius:999px;padding:8px 0;
font:inherit;font-size:13px;font-weight:700;color:var(--ink2);cursor:pointer;
transition:box-shadow .14s ease,color .12s ease;
}
.seg button.on{background:var(--card);color:var(--brand);box-shadow:var(--sh-out-xs)}
.srow.danger .si{color:var(--bad)}
/* 开关：凹槽 + 凸起圆钮 */
.sw{
width:50px;height:30px;border-radius:999px;background:var(--card);position:relative;
flex:0 0 auto;box-shadow:var(--sh-in-sm);
transition:box-shadow .18s ease;
}
.sw::after{
content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;
/* 1.41.0：滑块原来是和底同色，只靠阴影分辨 —— 截图上几乎看不出是「开关」，
更看不出开/关。加一层白面和细边，仍然是新拟物但一眼能读。 */
background:#fff;border:1px solid var(--line2);box-sizing:border-box;
box-shadow:var(--sh-out-xs);
transition:transform .2s var(--ease-out),background .18s ease;
}
.sw.on{box-shadow:var(--sh-in-sm),inset 0 0 0 2px rgba(63,113,128,.28)}
.sw.on::after{transform:translateX(20px);background:var(--brand);box-shadow:var(--sh-out-xs)}
/* ---------------- 底部 tab ---------------- */
/* 一条浮起来的凹槽：选中那一格再凸出来 */
.tabbar{
position:fixed;left:12px;right:12px;bottom:calc(10px + env(safe-area-inset-bottom,0px));z-index:40;
display:flex;justify-content:center;
max-width:496px;margin:0 auto;
background:var(--card);
border:0;border-radius:26px;
box-shadow:var(--sh-out);
padding:0;
}
.tabbar .in{display:flex;width:100%;max-width:496px;padding:6px}
.tab{
flex:1;height:var(--tabh);display:flex;flex-direction:column;align-items:center;
justify-content:center;gap:4px;color:var(--ink3);font-size:10.5px;font-weight:600;
letter-spacing:.04em;position:relative;border-radius:20px;
transition:color .12s ease;
}
.tab > *{position:relative;z-index:1}
.tab.on::before{
content:"";position:absolute;inset:4px 5px;border-radius:18px;
background:var(--card);box-shadow:var(--sh-out-sm);z-index:0;
}
.tab .ic{width:22px;height:22px;transition:transform .15s var(--ease-out)}
.tab:active .ic{transform:scale(.92)}
.tab.on{color:var(--brand)}
.tab .badge{
position:absolute;top:10px;right:calc(50% - 16px);
min-width:7px;height:7px;border-radius:50%;background:var(--bad);
box-shadow:0 0 0 3px var(--card);
}
/* ---------------- 弹层 ---------------- */
.mask{
position:fixed;inset:0;z-index:60;background:rgba(70,78,92,.30);
backdrop-filter:blur(2px);-webkit-backdrop-filter:blur(2px);
display:flex;align-items:flex-end;justify-content:center;
opacity:0;transition:opacity .18s var(--ease-out);
}
.mask.in{opacity:1}
.sheet{
width:100%;max-width:520px;background:var(--paper);
border-radius:30px 30px 0 0;padding:10px 20px calc(22px + env(safe-area-inset-bottom,0px));
max-height:88dvh;overflow-y:auto;overscroll-behavior:contain;
border:0;
transform:translateY(100%);
transition:transform .28s var(--ease-drawer);
box-shadow:0 -14px 40px -16px rgba(60,65,75,.5), inset 0 1px 0 rgba(255,255,255,.9);
}
.mask.in .sheet{transform:translateY(0)}
.grab{width:40px;height:5px;border-radius:999px;background:var(--line);
box-shadow:var(--sh-in-xs);margin:8px auto 18px}
.sheet h3{margin:0 0 6px;font-size:19px;font-weight:700;letter-spacing:-.3px}
.sheet .desc{margin:0 0 16px;font-size:13.5px;color:var(--ink2);line-height:1.65}
/* 1.18.0：选集翻页条 —— 一本书可能上千集（官场现形记 1167 集），
机器按字节预算切页，这里给「上一段 / 第 a–b 集 / 下一段」。
disabled 时压暗但**不隐藏**：位置不能跳，老人按惯了就有肌肉记忆。 */
.eppage{display:flex;align-items:center;gap:9px;margin:0 0 12px}
.eppage span{flex:1 1 auto;min-width:0;text-align:center;font-size:12.5px;
font-weight:700;color:var(--ink3);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.eppage .btn{flex:0 0 auto}
.eppage .btn[disabled]{opacity:.38;box-shadow:none}
.sheet .field{margin-bottom:14px}
.sheet .field label{display:block;font-size:11.5px;font-weight:700;color:var(--ink3);
margin-bottom:7px;letter-spacing:.1em}
.sheet input[type=text],.sheet input[type=password],.sheet input[type=search]{
width:100%;height:50px;padding:0 16px;border-radius:999px;
border:0;background:var(--card);color:var(--ink);outline:none;
box-shadow:var(--sh-in);
transition:box-shadow .14s ease;
}
.sheet input:focus{box-shadow:var(--sh-in),inset 0 0 0 2px rgba(63,113,128,.30)}
.sheet .acts{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:18px}
.sheet .acts.one{grid-template-columns:1fr}
.sheet .acts.stack{grid-template-columns:1fr}
.optlist{background:var(--card);border-radius:var(--r-lg);border:0;
box-shadow:var(--sh-out);overflow:hidden;margin-bottom:14px}
.opt{display:flex;align-items:center;gap:11px;padding:14px 16px;width:100%;text-align:left;
font-size:15.5px;font-weight:600;background:var(--card)}
.opt + .opt{box-shadow:var(--carve)}
.opt:active{box-shadow:var(--sh-in-sm)}
.opt .ic{color:var(--ink3)}
.opt.danger{color:var(--bad)}
.opt.danger .ic{color:var(--bad)}
.opt .r{margin-left:auto;font-size:12px;color:var(--ink3);font-weight:600;letter-spacing:.02em}
/* toast：浅色凸起片，不抢眼 */
.toast{
position:fixed;left:50%;bottom:calc(var(--tabh) + 34px + env(safe-area-inset-bottom,0px));
transform:translate(-50%,8px);z-index:80;
max-width:86%;padding:12px 18px;border-radius:999px;
background:var(--card);color:var(--ink);font-size:14px;font-weight:600;letter-spacing:.02em;
box-shadow:var(--sh-out);
opacity:0;pointer-events:none;
transition:opacity .18s var(--ease-out),transform .18s var(--ease-out);
display:flex;align-items:center;gap:9px;
}
.toast.show{opacity:1;transform:translate(-50%,0)}
.toast > span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.toast .ic{width:17px;height:17px;flex:0 0 auto;color:var(--ok)}
.toast.bad .ic{color:var(--bad)}
/* 开机 / 连接中 */
.boot{
position:fixed;inset:0;z-index:90;background:var(--paper);
display:flex;flex-direction:column;align-items:center;justify-content:center;gap:18px;
transition:opacity .28s var(--ease-out),visibility .28s;
}
.boot.gone{opacity:0;visibility:hidden}
.boot .logo{
width:76px;height:76px;border-radius:26px;background:var(--card);
display:grid;place-items:center;
box-shadow:var(--sh-out);
}
.boot .logo .ic{width:36px;height:36px;color:var(--brand)}
.boot .msg{font-size:14.5px;color:var(--ink2);font-weight:600;letter-spacing:.02em}
.boot .bar2{width:140px;height:10px;border-radius:999px;background:var(--card);
box-shadow:var(--sh-in-sm);overflow:hidden}
.boot .bar2 i{display:block;height:100%;width:40%;border-radius:999px;background:var(--brand);
animation:slide 1.1s var(--ease-in-out) infinite}
@keyframes slide{0%{transform:translateX(-100%)}100%{transform:translateX(320%)}}
/* 帮助折叠 */
.faq{padding:0 18px 6px}
.faq details{padding:14px 0}
.faq details + details{box-shadow:var(--carve)}
.faq summary{
font-size:15px;font-weight:600;cursor:pointer;list-style:none;
display:flex;align-items:center;gap:8px;
}
.faq summary::-webkit-details-marker{display:none}
.faq summary::after{
content:"";margin-left:auto;width:9px;height:9px;flex:0 0 auto;
border-right:2px solid var(--ink3);border-bottom:2px solid var(--ink3);
transform:rotate(45deg) translateY(-2px);transition:transform .18s var(--ease-out);
}
.faq details[open] summary::after{transform:rotate(-135deg) translateY(-2px)}
.faq p{margin:10px 0 0;font-size:13.5px;line-height:1.75;color:var(--ink2)}
@media (hover:hover) and (pointer:fine){
.chip:hover{box-shadow:var(--sh-out)}
.chip.on:hover{color:var(--brand2)}
.btn:hover{box-shadow:var(--sh-out)}
.btn.pri:hover{background:var(--brand2)}
.iconbtn:hover{color:var(--brand)}
}
@media (prefers-reduced-motion: reduce){
.page.on,.row,.alertcard{animation:none!important}
.bars .bar{animation:none!important}
.boot .bar2 i{animation:none!important}
.mask,.sheet,.toast{transition-duration:1ms!important}
.tbtn,.iconbtn,.chip,.btn{transition-duration:1ms!important}
}
/* ---------------- 1.41.0：「控制收音机」折叠面板 ---------------- */
/* 调台/暂停/音量收进一个按钮，点一下才展开。收起时按钮本身就是状态条。 */
.ctlbtn{
width:100%;display:flex;align-items:center;gap:13px;
padding:14px 16px;margin:16px 0 0;border:0;border-radius:var(--r-md);
background:var(--card);box-shadow:var(--sh-out);text-align:left;
font-family:inherit;color:var(--ink);cursor:pointer;
transition:box-shadow .18s ease;
}
.ctlbtn:active{box-shadow:var(--sh-in)}
.ctlbtn .ci{
width:38px;height:38px;border-radius:13px;flex:0 0 auto;
display:flex;align-items:center;justify-content:center;
background:var(--card);box-shadow:var(--sh-in-sm);color:var(--brand);
}
.ctlbtn .cx{flex:1;min-width:0;display:flex;flex-direction:column;gap:2px}
.ctlbtn .cx b{font-size:15px;font-weight:700;letter-spacing:.2px}
.ctlbtn .cx i{
font-style:normal;font-size:12px;font-weight:600;color:var(--ink3);
white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
}
.ctlbtn .chev{color:var(--ink3);flex:0 0 auto;transition:transform .24s cubic-bezier(.23,1,.32,1)}
.ctlbtn[aria-expanded="true"] .chev{transform:rotate(180deg)}
.ctlbtn[aria-expanded="true"] .ci{color:#fff;background:linear-gradient(180deg,var(--brand),var(--brand2));box-shadow:none}
/* 展开：**不做入场动画**。
原来写的是 `animation:ctlIn .26s ... both`（from opacity:0），
两个后果：① 任何让动画没跑完的情况（切页、截图、低端机掉帧）内容会停在半透明；
② headless 的 --virtual-time-budget 不推进 CSS animation 时钟，验收截图里永远是灰的。
这层动画带来的收益远小于风险，直接去掉。 */
.ctlpanel{overflow:hidden}
.ctlpanel .transport{margin-top:16px}
.volnum{
text-align:center;font-size:12.5px;font-weight:700;
color:var(--ink3);margin:2px 0 8px;
}
/* 静噪那一行的当前值 */
.sqnow{
align-self:flex-end;font-size:12px;font-weight:700;color:var(--ink3);
margin-top:-4px;
}
/* ---------------- 1.41.0 首页的配网按钮 ---------------- */
.iconbtn.wifi{position:relative}
.iconbtn.wifi.bad{color:var(--amber)}
.iconbtn.wifi.bad .ic{animation:wifiBlink 2.4s ease-in-out infinite}
@keyframes wifiBlink{0%,100%{opacity:1}50%{opacity:.45}}
.iconbtn.wifi .dot2{
position:absolute;top:5px;right:5px;width:8px;height:8px;border-radius:50%;
background:var(--bad);box-shadow:0 0 0 2px var(--card);
display:none;
}
.iconbtn.wifi.bad .dot2{display:block}
/* ---------------- 1.41.0 首页右上的按钮组 ---------------- */
/* .hdr 里的 .iconbtn 各自带 margin-left:auto，两个并列时会被推到页中间散开。
包一层之后由这层统一贴右，按钮之间只留固定间距。 */
.hdrbtns{
margin-left:auto;display:flex;align-items:center;gap:10px;flex:0 0 auto;
}
.hdrbtns .iconbtn{margin-left:0}
</style>
</head>
<body>
<svg xmlns="http://www.w3.org/2000/svg" style="display:none" aria-hidden="true"><symbol id="i-activity" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2" /></symbol><symbol id="i-arrow-down" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 5v14" /> <path d="m19 12-7 7-7-7" /></symbol><symbol id="i-arrow-up" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m5 12 7-7 7 7" /> <path d="M12 19V5" /></symbol><symbol id="i-battery-medium" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="16" height="10" x="2" y="7" rx="2" ry="2" /> <line x1="22" x2="22" y1="11" y2="13" /> <line x1="6" x2="6" y1="11" y2="13" /> <line x1="10" x2="10" y1="11" y2="13" /></symbol><symbol id="i-chevron-down" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6" /></symbol><symbol id="i-chevron-left" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m15 18-6-6 6-6" /></symbol><symbol id="i-chevron-right" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 18 6-6-6-6" /></symbol><symbol id="i-circle-alert" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <line x1="12" x2="12" y1="8" y2="12" /> <line x1="12" x2="12.01" y1="16" y2="16" /></symbol><symbol id="i-circle-check" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <path d="m9 12 2 2 4-4" /></symbol><symbol id="i-circle-help" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <path d="M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3" /> <path d="M12 17h.01" /></symbol><symbol id="i-circle-plus" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <path d="M8 12h8" /> <path d="M12 8v8" /></symbol><symbol id="i-clock" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <polyline points="12 6 12 12 16 14" /></symbol><symbol id="i-cloud-off" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m2 2 20 20" /> <path d="M5.782 5.782A7 7 0 0 0 9 19h8.5a4.5 4.5 0 0 0 1.307-.193" /> <path d="M21.532 16.5A4.5 4.5 0 0 0 17.5 10h-1.79A7.008 7.008 0 0 0 10 5.07" /></symbol><symbol id="i-cpu" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="16" height="16" x="4" y="4" rx="2" /> <rect width="6" height="6" x="9" y="9" rx="1" /> <path d="M15 2v2" /> <path d="M15 20v2" /> <path d="M2 15h2" /> <path d="M2 9h2" /> <path d="M20 15h2" /> <path d="M20 9h2" /> <path d="M9 2v2" /> <path d="M9 20v2" /></symbol><symbol id="i-download" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" /> <polyline points="7 10 12 15 17 10" /> <line x1="12" x2="12" y1="15" y2="3" /></symbol><symbol id="i-ellipsis" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="1" /> <circle cx="19" cy="12" r="1" /> <circle cx="5" cy="12" r="1" /></symbol><symbol id="i-eye" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0" /> <circle cx="12" cy="12" r="3" /></symbol><symbol id="i-eye-off" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.733 5.076a10.744 10.744 0 0 1 11.205 6.575 1 1 0 0 1 0 .696 10.747 10.747 0 0 1-1.444 2.49" /> <path d="M14.084 14.158a3 3 0 0 1-4.242-4.242" /> <path d="M17.479 17.499a10.75 10.75 0 0 1-15.417-5.151 1 1 0 0 1 0-.696 10.75 10.75 0 0 1 4.446-5.143" /> <path d="m2 2 20 20" /></symbol><symbol id="i-folder" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z" /></symbol><symbol id="i-folder-plus" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 10v6" /> <path d="M9 13h6" /> <path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z" /></symbol><symbol id="i-grip-vertical" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="9" cy="12" r="1" /> <circle cx="9" cy="5" r="1" /> <circle cx="9" cy="19" r="1" /> <circle cx="15" cy="12" r="1" /> <circle cx="15" cy="5" r="1" /> <circle cx="15" cy="19" r="1" /></symbol><symbol id="i-hard-drive" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="22" x2="2" y1="12" y2="12" /> <path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z" /> <line x1="6" x2="6.01" y1="16" y2="16" /> <line x1="10" x2="10.01" y1="16" y2="16" /></symbol><symbol id="i-history" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /> <path d="M3 3v5h5" /> <path d="M12 7v5l4 2" /></symbol><symbol id="i-info" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10" /> <path d="M12 16v-4" /> <path d="M12 8h.01" /></symbol><symbol id="i-key-round" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2.586 17.414A2 2 0 0 0 2 18.828V21a1 1 0 0 0 1 1h3a1 1 0 0 0 1-1v-1a1 1 0 0 1 1-1h1a1 1 0 0 0 1-1v-1a1 1 0 0 1 1-1h.172a2 2 0 0 0 1.414-.586l.814-.814a6.5 6.5 0 1 0-4-4z" /> <circle cx="16.5" cy="7.5" r=".5" fill="currentColor" /></symbol><symbol id="i-list-music" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15V6" /> <path d="M18.5 18a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5Z" /> <path d="M12 12H3" /> <path d="M16 6H3" /> <path d="M12 18H3" /></symbol><symbol id="i-lock" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2" ry="2" /> <path d="M7 11V7a5 5 0 0 1 10 0v4" /></symbol><symbol id="i-lock-open" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2" ry="2" /> <path d="M7 11V7a5 5 0 0 1 9.9-1" /></symbol><symbol id="i-move-right" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 8L22 12L18 16" /> <path d="M2 12H22" /></symbol><symbol id="i-pause" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="14" y="4" width="4" height="16" rx="1" /> <rect x="6" y="4" width="4" height="16" rx="1" /></symbol><symbol id="i-pencil" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z" /> <path d="m15 5 4 4" /></symbol><symbol id="i-play" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="6 3 20 12 6 21 6 3" /></symbol><symbol id="i-plus" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14" /> <path d="M12 5v14" /></symbol><symbol id="i-power" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v10" /> <path d="M18.4 6.6a9 9 0 1 1-12.77.04" /></symbol><symbol id="i-radio" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4.9 19.1C1 15.2 1 8.8 4.9 4.9" /> <path d="M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5" /> <circle cx="12" cy="12" r="2" /> <path d="M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5" /> <path d="M19.1 4.9C23 8.8 23 15.1 19.1 19" /></symbol><symbol id="i-refresh-cw" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8" /> <path d="M21 3v5h-5" /> <path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16" /> <path d="M8 16H3v5" /></symbol><symbol id="i-rotate-ccw" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /> <path d="M3 3v5h5" /></symbol><symbol id="i-rotate-cw" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8" /> <path d="M21 3v5h-5" /></symbol><symbol id="i-scan-line" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7V5a2 2 0 0 1 2-2h2" /> <path d="M17 3h2a2 2 0 0 1 2 2v2" /> <path d="M21 17v2a2 2 0 0 1-2 2h-2" /> <path d="M7 21H5a2 2 0 0 1-2-2v-2" /> <path d="M7 12h10" /></symbol><symbol id="i-search" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8" /> <path d="m21 21-4.3-4.3" /></symbol><symbol id="i-server" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="20" height="8" x="2" y="2" rx="2" ry="2" /> <rect width="20" height="8" x="2" y="14" rx="2" ry="2" /> <line x1="6" x2="6.01" y1="6" y2="6" /> <line x1="6" x2="6.01" y1="18" y2="18" /></symbol><symbol id="i-settings" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z" /> <circle cx="12" cy="12" r="3" /></symbol><symbol id="i-shield-check" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z" /> <path d="m9 12 2 2 4-4" /></symbol><symbol id="i-signal" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 20h.01" /> <path d="M7 20v-4" /> <path d="M12 20v-8" /> <path d="M17 20V8" /> <path d="M22 4v16" /></symbol><symbol id="i-skip-back" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="19 20 9 12 19 4 19 20" /> <line x1="5" x2="5" y1="19" y2="5" /></symbol><symbol id="i-skip-forward" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="5 4 15 12 5 20 5 4" /> <line x1="19" x2="19" y1="5" y2="19" /></symbol><symbol id="i-sliders-horizontal" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="21" x2="14" y1="4" y2="4" /> <line x1="10" x2="3" y1="4" y2="4" /> <line x1="21" x2="12" y1="12" y2="12" /> <line x1="8" x2="3" y1="12" y2="12" /> <line x1="21" x2="16" y1="20" y2="20" /> <line x1="12" x2="3" y1="20" y2="20" /> <line x1="14" x2="14" y1="2" y2="6" /> <line x1="8" x2="8" y1="10" y2="14" /> <line x1="16" x2="16" y1="18" y2="22" /></symbol><symbol id="i-smartphone" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="14" height="20" x="5" y="2" rx="2" ry="2" /> <path d="M12 18h.01" /></symbol><symbol id="i-sparkles" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9.937 15.5A2 2 0 0 0 8.5 14.063l-6.135-1.582a.5.5 0 0 1 0-.962L8.5 9.936A2 2 0 0 0 9.937 8.5l1.582-6.135a.5.5 0 0 1 .963 0L14.063 8.5A2 2 0 0 0 15.5 9.937l6.135 1.581a.5.5 0 0 1 0 .964L15.5 14.063a2 2 0 0 0-1.437 1.437l-1.582 6.135a.5.5 0 0 1-.963 0z" /> <path d="M20 3v4" /> <path d="M22 5h-4" /> <path d="M4 17v2" /> <path d="M5 18H3" /></symbol><symbol id="i-timer" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="10" x2="14" y1="2" y2="2" /> <line x1="12" x2="15" y1="14" y2="11" /> <circle cx="12" cy="14" r="8" /></symbol><symbol id="i-trash-2" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18" /> <path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6" /> <path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2" /> <line x1="10" x2="10" y1="11" y2="17" /> <line x1="14" x2="14" y1="11" y2="17" /></symbol><symbol id="i-triangle-alert" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3" /> <path d="M12 9v4" /> <path d="M12 17h.01" /></symbol><symbol id="i-upload" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" /> <polyline points="17 8 12 3 7 8" /> <line x1="12" x2="12" y1="3" y2="15" /></symbol><symbol id="i-volume-1" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /> <path d="M16 9a5 5 0 0 1 0 6" /></symbol><symbol id="i-volume-2" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /> <path d="M16 9a5 5 0 0 1 0 6" /> <path d="M19.364 18.364a9 9 0 0 0 0-12.728" /></symbol><symbol id="i-volume-x" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /> <line x1="22" x2="16" y1="9" y2="15" /> <line x1="16" x2="22" y1="9" y2="15" /></symbol><symbol id="i-wand-sparkles" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21.64 3.64-1.28-1.28a1.21 1.21 0 0 0-1.72 0L2.36 18.64a1.21 1.21 0 0 0 0 1.72l1.28 1.28a1.2 1.2 0 0 0 1.72 0L21.64 5.36a1.2 1.2 0 0 0 0-1.72" /> <path d="m14 7 3 3" /> <path d="M5 6v4" /> <path d="M19 14v4" /> <path d="M10 2v2" /> <path d="M7 8H3" /> <path d="M21 16h-4" /> <path d="M11 3H9" /></symbol><symbol id="i-waves" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 6c.6.5 1.2 1 2.5 1C7 7 7 5 9.5 5c2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1" /> <path d="M2 12c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1" /> <path d="M2 18c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1" /></symbol><symbol id="i-wifi" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h.01" /> <path d="M2 8.82a15 15 0 0 1 20 0" /> <path d="M5 12.859a10 10 0 0 1 14 0" /> <path d="M8.5 16.429a5 5 0 0 1 7 0" /></symbol><symbol id="i-wifi-off" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h.01" /> <path d="M8.5 16.429a5 5 0 0 1 7 0" /> <path d="M5 12.859a10 10 0 0 1 5.17-2.69" /> <path d="M19 12.859a10 10 0 0 0-2.007-1.523" /> <path d="M2 8.82a15 15 0 0 1 4.177-2.643" /> <path d="M22 8.82a15 15 0 0 0-11.288-3.764" /> <path d="m2 2 20 20" /></symbol><symbol id="i-x" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18" /> <path d="m6 6 12 12" /></symbol></svg>
<div class="boot" id="boot">
<div class="logo"><svg class="ic"><use href="#i-radio"></use></svg></div>
<div class="msg" id="bootMsg">正在连接收音机…</div>
<div class="bar2"><i></i></div>
</div>
<div class="offbar" id="offBar" hidden>
<svg class="ic sm"><use href="#i-wifi-off"></use></svg>
<span class="tx" id="offTx">没连上收音机</span>
<button id="offGo">去配网</button>
</div>
<div class="app">
<section class="page on" data-page="home">
<div class="hdr">
<div>
<h1>操控收音机</h1>
<div class="sub" id="hdrSub">正在连接…</div>
</div>
<div class="hdrbtns">
<button class="iconbtn wifi" id="btnWifiHome" aria-label="给收音机配网">
<svg class="ic"><use href="#i-wifi"></use></svg>
<i class="dot2" id="wifiDot"></i>
</button>
</div>
</div>
<div class="screenwrap">
<canvas id="scr" width="320" height="170" aria-label="收音机屏幕实时镜像"></canvas>
</div>
<div class="screentip">
<span class="dot" id="dot"></span>
<span id="tipTx">正在启动…</span>
<span class="sp"><span class="mirror" id="liveTx">实时</span><span id="lat"></span></span>
</div>
<div class="sndrow" id="sndRow">
<i class="lv"><b id="lvBar"></b></i>
<span class="snd" id="sndTx">没在放</span>
</div>
<button class="ctlbtn" id="btnCtl" aria-expanded="false" aria-controls="ctlPanel">
<span class="ci"><svg class="ic"><use href="#i-sliders-horizontal"></use></svg></span>
<span class="cx">
<b>控制收音机</b>
<i id="ctlSub">换台 · 音量 · 暂停</i>
</span>
<svg class="ic sm chev" id="ctlChev"><use href="#i-chevron-down"></use></svg>
</button>
<div class="ctlpanel hide" id="ctlPanel">
<div class="transport">
<button class="tbtn" id="btnPrev">
<svg class="ic"><use href="#i-skip-back"></use></svg>
<span>上一台</span>
</button>
<button class="tbtn main" id="btnPlay" aria-label="播放或暂停">
<svg class="ic" id="playIc"><use href="#i-pause"></use></svg>
</button>
<button class="tbtn" id="btnNext">
<svg class="ic"><use href="#i-skip-forward"></use></svg>
<span>下一台</span>
</button>
</div>
<div class="epbar hide" id="epBar">
<button class="epbtn" id="epPrev" aria-label="上一集">
<svg class="ic sm"><use href="#i-skip-back"></use></svg>
<span>上一集</span>
</button>
<button class="epmid" id="epOpen" aria-label="选集">
<span class="line"><b id="epNow">第 1 集</b><em id="epOf">共 15 集</em></span>
<i id="epTitle"></i>
</button>
<button class="epbtn" id="epNext" aria-label="下一集">
<svg class="ic sm"><use href="#i-skip-forward"></use></svg>
<span>下一集</span>
</button>
</div>
<div class="volrow">
<button class="iconbtn" id="btnVolDown" aria-label="音量减">
<svg class="ic sm"><use href="#i-volume-1"></use></svg>
</button>
<input type="range" id="vol" min="6" max="21" step="1" value="18" aria-label="音量">
<button class="iconbtn" id="btnVolUp" aria-label="音量加">
<svg class="ic sm"><use href="#i-volume-2"></use></svg>
</button>
</div>
<div class="volnum">音量 <span id="volNum">18</span></div>
</div>
<div class="chips" id="homeChips"></div>
<div class="card" style="margin-top:4px">
<div class="card-hd">今天听得怎么样<span class="r" id="todayPlays"></span></div>
<div class="today">
<div class="tnum" id="todayNum">—</div>
<button class="btn sm ghost" id="btnHist" style="margin-left:auto">
<svg class="ic sm"><use href="#i-history"></use></svg>看记录
</button>
</div>
<div class="lastone" id="lastOne">—</div>
</div>
</section>
<section class="page" data-page="stations">
<div class="hdr">
<div>
<h1>电台</h1>
<div class="sub" id="stHint">长按左侧把手可拖动排序</div>
</div>
<button class="iconbtn" id="btnAdd" aria-label="添加电台">
<svg class="ic"><use href="#i-plus"></use></svg>
</button>
</div>
<div class="search">
<svg class="ic sm"><use href="#i-search"></use></svg>
<input id="q" type="search" placeholder="搜台名、频道" autocomplete="off" enterkeyhint="search">
<button class="iconbtn hide" id="qClear" style="width:26px;height:26px;border-radius:9px" aria-label="清空">
<svg class="ic sm"><use href="#i-x"></use></svg>
</button>
</div>
<div class="chips" id="stChips"></div>
<div class="list" id="stList"></div>
</section>
<section class="page" data-page="history">
<div class="hdr">
<div>
<h1>收听记录</h1>
<div class="sub" id="histHint">机器自动记账</div>
</div>
</div>
<div class="stats" id="stats"></div>
<div class="card">
<div class="card-hd">最近 7 天<span class="r" id="weekSum"></span></div>
<div class="bars" id="bars"></div>
</div>
<div class="list" id="histList"></div>
<div class="btnrow one" style="margin-top:4px">
<button class="btn ghost" id="btnClearHist">
<svg class="ic sm"><use href="#i-trash-2"></use></svg>清空记录
</button>
</div>
</section>
<section class="page" data-page="diag">
<div class="hdr">
<div>
<h1>体检</h1>
<div class="sub" id="diagHint">检查这台机器哪儿不舒服</div>
</div>
<button class="iconbtn" id="btnRecheck" aria-label="重新检查">
<svg class="ic"><use href="#i-refresh-cw"></use></svg>
</button>
</div>
<div id="diagAlert"></div>
<div class="health">
<div class="ring">
<svg width="92" height="92" viewBox="0 0 92 92">
<circle cx="46" cy="46" r="38" fill="none" stroke="#dfe3ea" stroke-width="7"></circle>
<circle id="ringFg" cx="46" cy="46" r="38" fill="none" stroke="#4a8a63" stroke-width="7"
stroke-linecap="round" stroke-dasharray="238.8" stroke-dashoffset="0"
style="transition:stroke-dashoffset .6s cubic-bezier(.23,1,.32,1),stroke .3s"></circle>
</svg>
<div class="val" id="ringVal">--<small>分</small></div>
</div>
<div class="txt">
<h2 id="healthTitle">正在检查…</h2>
<p id="healthDesc">把网络、声音、存储都过一遍。</p>
</div>
</div>
<div class="card" id="chkList"></div>
<div class="btnrow one">
<button class="btn pri" id="btnFixAll">
<svg class="ic sm"><use href="#i-wand-sparkles"></use></svg>
<span id="fixAllTx">一键修复</span>
</button>
</div>
<div class="card">
<div class="card-hd">台的健康<span class="r" id="hlSum">—</span></div>
<div class="hlgrid">
<div class="hcell"><b id="hlOk">0</b><span>出过声</span></div>
<div class="hcell bad"><b id="hlBad">0</b><span>连不上</span></div>
<div class="hcell dim"><b id="hlDim">0</b><span>没声音</span></div>
<div class="hcell unk"><b id="hlUnk">0</b><span>没测过</span></div>
</div>
<button class="srow" id="btnHlList">
<span class="si"><svg class="ic sm"><use href="#i-triangle-alert"></use></svg></span>
<span class="st"><span class="t">只看有问题的台</span><span class="m">连不上、没声音的都挑出来</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" id="btnHlScan">
<span class="si"><svg class="ic sm"><use href="#i-play"></use></svg></span>
<span class="st"><span class="t">挨个试一遍</span><span class="m" id="hlScanSub">查一查哪些台真的有声</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
</div>
<div class="card">
<div class="card-hd">这些活 App 能替你干</div>
<button class="srow" data-fix="replay">
<span class="si"><svg class="ic sm"><use href="#i-play"></use></svg></span>
<span class="st"><span class="t">重开这一个台</span><span class="m">卡住、没声音、一直缓冲</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" data-fix="reconnect">
<span class="si"><svg class="ic sm"><use href="#i-wifi"></use></svg></span>
<span class="st"><span class="t">重新连 WiFi</span><span class="m">不用断电，10 秒左右</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" data-fix="fix_table">
<span class="si"><svg class="ic sm"><use href="#i-list-music"></use></svg></span>
<span class="st"><span class="t">整理台表</span><span class="m">台用着用着乱了、少了，点这个</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" data-fix="reboot">
<span class="si"><svg class="ic sm"><use href="#i-power"></use></svg></span>
<span class="st"><span class="t">重启收音机</span><span class="m">万能招，先试这个</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
</div>
</section>
<section class="page" data-page="settings">
<div class="hdr">
<div>
<h1>设置</h1>
<div class="sub" id="setHint"></div>
</div>
</div>
<div class="card">
<div class="card-hd">这台收音机</div>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-radio"></use></svg></span>
<span class="st"><span class="t">固件版本</span></span>
<span class="sv" id="svFw">—</span>
</div>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-signal"></use></svg></span>
<span class="st"><span class="t">网络地址</span></span>
<span class="sv" id="svIp">—</span>
</div>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-timer"></use></svg></span>
<span class="st"><span class="t">已运行</span></span>
<span class="sv" id="svUp">—</span>
</div>
<button class="srow" id="btnReboot">
<span class="si"><svg class="ic sm"><use href="#i-power"></use></svg></span>
<span class="st"><span class="t">重启收音机</span><span class="m">卡住了就重启，台表和设置都在</span></span>
</button>
</div>
<div class="card">
<div class="card-hd">屏幕与声音</div>
<div class="srow col">
<span class="st"><span class="t">音量 · 网络电台</span><span class="m">听手机台/有声书那一路（原厂 netvolume，0–21）</span></span>
<input type="range" id="rgVol" min="6" max="21" step="1" aria-label="网络电台音量">
</div>
<div class="srow col">
<span class="st"><span class="t">音量 · 收音机</span><span class="m">转 FM/中波那一路（原厂 volume，0–63）</span></span>
<input type="range" id="rgFmVol" min="6" max="63" step="1" aria-label="收音机音量">
</div>
<div class="srow col">
<span class="st"><span class="t">亮度</span><span class="m">平时嫌刺眼就调低；一会儿没人动还会自动再暗一档</span></span>
<input type="range" id="rgBr" min="5" max="100" step="5" aria-label="亮度">
</div>
<div class="srow col">
<span class="st"><span class="t">没人动就熄屏</span><span class="m">到时间自动黑屏省电，转一下旋钮就亮</span></span>
<div class="seg" id="segOff">
<button data-v="1">1 分</button><button data-v="3">3 分</button>
<button data-v="10">10 分</button><button data-v="0">从不</button>
</div>
</div>
<button class="srow" id="btnScreenOff">
<span class="si"><svg class="ic sm"><use href="#i-eye-off"></use></svg></span>
<span class="st"><span class="t">立即熄屏</span><span class="m">听广播不用看屏幕的时候用</span></span>
</button>
</div>
<div class="card" id="fmCard">
<div class="card-hd">收音机（调频 / 中波）<span class="hd-r" id="fmNow">—</span></div>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-radio"></use></svg></span>
<span class="st"><span class="t">现在收的是</span><span class="m" id="fmBandSub">没在收音模式</span></span>
<span class="sv" id="fmFreq">—</span>
</div>
<div class="srow col">
<span class="st"><span class="t">静噪门限</span><span class="m">台太弱时自动闭嘴，省得一直嘶嘶响；0 = 关</span></span>
<input type="range" id="rgSq" min="0" max="100" step="5" aria-label="静噪门限">
<span class="sqnow" id="sqNow">关</span>
</div>
<button class="srow" id="btnHalfStep">
<span class="si"><svg class="ic sm"><use href="#i-sliders-horizontal"></use></svg></span>
<span class="st"><span class="t">半步进</span><span class="m">调频按 0.05MHz 走，适合台挤在一起的地方</span></span>
<span class="sw" id="swHalf"></span>
</button>
<button class="srow" id="btnFmStations">
<span class="si"><svg class="ic sm"><use href="#i-list-music"></use></svg></span>
<span class="st"><span class="t">搜到的台</span><span class="m">点开看清单，点一行就调过去</span></span>
<span class="sv" id="fmCount">—</span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" id="btnFmScan">
<span class="si"><svg class="ic sm"><use href="#i-search"></use></svg></span>
<span class="st"><span class="t">重新搜台</span><span class="m" id="fmScanSub">收音机自己扫一遍，十几秒</span></span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
</div>
<div class="card" id="wifiCard">
<div class="card-hd">网络</div>
<button class="srow" id="btnWifi">
<span class="si"><svg class="ic sm"><use href="#i-wifi"></use></svg></span>
<span class="st"><span class="t" id="wifiTitle">WiFi</span><span class="m" id="wifiSub">点这里管理：已存的 + 空着的格子</span></span>
<span class="sv" id="wifiNow">—</span>
<svg class="ic sm chev"><use href="#i-chevron-right"></use></svg>
</button>
<button class="srow" id="btnReconn">
<span class="si"><svg class="ic sm"><use href="#i-rotate-ccw"></use></svg></span>
<span class="st"><span class="t">重新连接网络</span><span class="m">WiFi 没断但没网时用</span></span>
</button>
<button class="srow" id="battRow" style="display:none">
<span class="si"><svg class="ic sm"><use href="#i-battery-medium"></use></svg></span>
<span class="st"><span class="t">电量</span><span class="m" id="battSub">—</span></span>
<span class="sv" id="battNow">—</span>
</button>
<button class="srow" id="btnBattCal" style="display:none">
<span class="si"><svg class="ic sm"><use href="#i-sliders-horizontal"></use></svg></span>
<span class="st"><span class="t">校准电量</span><span class="m">显示的百分比和实际不一样？点这里对一下</span></span>
</button>
</div>
<div class="card" id="cloudCard">
<div class="card-hd">不在家也能控制</div>
<button class="srow" id="btnCloud">
<span class="si"><svg class="ic sm"><use href="#i-signal"></use></svg></span>
<span class="st"><span class="t">远程控制（始终开启）</span>
<span class="m">不在一个网、不在一个城市，也能换台调音量</span></span>
<span class="sv" id="svRSt">—</span>
<svg class="ic sm chev"><use href="#i-refresh-cw"></use></svg>
</button>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-wifi"></use></svg></span>
<span class="st"><span class="t">中转服务器</span><span class="m">机器自己连出去，家里不用开端口</span></span>
<span class="sv" id="svRHost">—</span>
</div>
<div class="srow">
<span class="si"><svg class="ic sm"><use href="#i-activity"></use></svg></span>
<span class="st"><span class="t">状态</span></span>
<span class="sv" id="svRSt">—</span>
</div>
<button class="srow" id="btnPinEdit">
<span class="si"><svg class="ic sm"><use href="#i-key-round"></use></svg></span>
<span class="st"><span class="t">连接码</span>
<span class="m">手机输这 4 位就能找到这台（出厂 1234）</span></span>
<span class="sv" id="svPin">—</span>
<svg class="ic sm chev"><use href="#i-pencil"></use></svg>
</button>
</div>
<div class="card" id="devCard">
<div class="card-hd">我的机器<span class="hd-r" id="devHint">正在找…</span></div>
<div class="pinrow">
<input id="devPin" inputmode="numeric" maxlength="4" autocomplete="off"
spellcheck="false" placeholder="输 4 位连接码">
<button id="devPinGo" type="button">连接</button>
</div>
<div class="devlist" id="devList"></div>
<button class="srow" id="btnDevRefresh">
<span class="si"><svg class="ic sm"><use href="#i-refresh-cw"></use></svg></span>
<span class="st"><span class="t">重新找一遍</span><span class="m">机器每 30 秒报一次到，等几秒就会出现</span></span>
</button>
</div>
<div class="card">
<div class="card-hd">数据</div>
<button class="srow danger" id="btnResetStations">
<span class="si"><svg class="ic sm"><use href="#i-list-music"></use></svg></span>
<span class="st"><span class="t">恢复出厂台表</span><span class="m">341 个台回到出厂那版</span></span>
</button>
<button class="srow danger" id="btnForgetWifi">
<span class="si"><svg class="ic sm"><use href="#i-cloud-off"></use></svg></span>
<span class="st"><span class="t">忘掉 WiFi</span><span class="m">之后只能连机器自己的热点</span></span>
</button>
<button class="srow danger" id="btnFactory">
<span class="si"><svg class="ic sm"><use href="#i-triangle-alert"></use></svg></span>
<span class="st"><span class="t">恢复出厂设置</span><span class="m">台表、记录、配对全部清空</span></span>
</button>
</div>
<div class="card">
<div class="card-hd">出问题了？先看这里</div>
<div class="faq">
<details>
<summary>没有声音</summary>
<p>先看手机上这块屏幕：显示「播放中」却没有声音，多半是音量被拧到 0 —— 在主页把音量拉大试试。显示「出错了」或一直「缓冲中」，去体检页点「重开这一个台」。全都不行就「重启收音机」。</p>
</details>
<details>
<summary>连不上这台收音机</summary>
<p>收音机自己会发一个叫 <b>我的收音机</b> 的热点，<b>没有密码，点一下就连上</b>。连上后手机会自动弹出配置页；没弹的话，打开浏览器输入 <b>192.168.4.1</b> 也能进。注意：连着这个热点时手机用不了流量，改完设置记得切回家里的 WiFi。</p>
</details>
<details>
<summary>换了路由器 / 改了 WiFi 密码</summary>
<p>连上「我的收音机」热点进来，在「网络 → 换一个 WiFi」里重新选一次。新的连不上时机器会自动退回原来那个，不会变成一块砖。</p>
</details>
<details>
<summary>屏幕不亮</summary>
<p>为了省电，3 分钟没人动它会熄屏，转一下旋钮或按一下就好。真的黑着没反应，拔电等 5 秒再插上。</p>
</details>
<details>
<summary>某个台老是没声音</summary>
<p>很多地方台在蜻蜓 FM 上会变地址，机器 20 秒听不到就会自动跳到下一台。你可以在「电台」里把那个台删掉，或者点它右边的 ⋯ 改成新的台号。</p>
</details>
</div>
</div>
<div style="text-align:center;font-size:12px;color:var(--ink3);font-weight:600;padding:6px 0 10px">
MyRadio · 给爸妈的收音机
</div>
</section>
</div>
<nav class="tabbar" id="tabbar">
<div class="in">
<button class="tab on" data-tab="home">
<svg class="ic"><use href="#i-radio"></use></svg><span>收音机</span>
</button>
<button class="tab" data-tab="stations">
<svg class="ic"><use href="#i-list-music"></use></svg><span>电台</span>
</button>
<button class="tab" data-tab="history">
<svg class="ic"><use href="#i-history"></use></svg><span>记录</span>
</button>
<button class="tab" data-tab="diag">
<svg class="ic"><use href="#i-activity"></use></svg><span>体检</span>
<span class="badge hide" id="diagBadge"></span>
</button>
<button class="tab" data-tab="settings">
<svg class="ic"><use href="#i-settings"></use></svg><span>设置</span>
</button>
</div>
</nav>
<div id="sheetHost"></div>
<div class="toast" id="toast"><svg class="ic"><use href="#i-circle-check"></use></svg><span id="toastTx"></span></div>
<script>
/* ==== cloud.js ==== */
/* ============================================================
cloud.js —— 跨网远程控制（1.8.0）
为什么需要它：
1.7.0 及以前，这个 App 只能在家里的 WiFi 里用 —— 因为页面本身就是
收音机发出来的，离开那个网连页面都打不开。
1.8.0 起收音机会自己挂到公网 MQTT 服务器上；手机无论在哪个城市，
只要知道「遥控码」，就能顺着这条路让它换台、调音量、调亮度。
为什么不用 mqtt.js：
设备端零外链是这条产品的底线（机器在别人家，CDN 挂了也得能用）。
这里手写一个 MQTT 3.1.1 over WebSocket，只实现用得到的四个包：
CONNECT / SUBSCRIBE / PUBLISH / PINGREQ。
协议（与 src/cloud.cpp 一一对应）：
发  myradio/<设备码>/c   {"i":1,"k":"482913","m":"POST","p":"/api/vol","b":"v=50"}
收  myradio/<设备码>/r   {"i":1,"s":200,"d":{"ok":1}}
1.19.0 加的两件事（"免配对"）：
① 发现：机器每 30 秒往 myradio/dev/<短码> 发一条 retain 的"我在线"。
这里订阅 myradio/dev/+，于是**任何一台手机**（不用是当初配网的那台）
一打开 App 就能列出家里那台机器。
② 免输码：遥控码改由设备码算出来（deriveCode，与 src/cloud.cpp 的
deriveFrom() 逐字节相同）。机器开着「免配对」时认这个算出来的码，
所以换手机、重装 App、App 升级，都不用再抄那 12 位。
!! 顺手修了 1.8.0 的一个错：原来 SUBSCRIBE 订的是 /c（命令主题），
可回包走的是 /r —— 真 EMQX 上根本收不到（本地假 broker 不看订阅，
所以测试一直是绿的）。现在订 /r。
============================================================ */
const CLOUD = (function () {
const LSKEY = 'mr_cloud';
// dev = 6 位设备码，code = 6 位遥控码，两个拼起来就是用户看到的「遥控码」
// 主题前缀：与 src/config.h 的 CLOUD_PREFIX / CLOUD_DEV_PREFIX 必须一致
const PREFIX = 'myradio';
const DEVPRE = PREFIX + '/dev/';
const S = {
dev: '', code: '', host: 'broker-cn.emqx.io', wss: 8084, secure: true,
name: '',           // 这台机器的名字（在线广播里带过来的）
devs: [],           // 发现的机器：[{dev,n,fw,open,batt,seen}]
want: false,        // 用户打开过「远程控制」
conn: false,        // MQTT 已连上
phase: 'idle',      // idle / connecting / online / err
err: '',
rx: 0, tx: 0,       // 收发条数，设置页显示着玩，排障时有用
down: 0,            // 重连次数
onDevs: null        // 设备列表变了就调一下（pages.js 挂的）
};
let ws = null;
let rxbuf = new Uint8Array(0);
let subId = 1;
let seq = 1;
const pending = new Map();
let pingT = 0, retryT = 0, retryMs = 2000;
const enc = s => new TextEncoder().encode(s);
const dec = b => new TextDecoder().decode(b);
/* ---------------- 存 ---------------- */
function load() {
try {
const j = JSON.parse(localStorage.getItem(LSKEY) || '{}');
if (j.dev)  S.dev  = String(j.dev);
if (j.code) S.code = String(j.code);
if (j.host) S.host = String(j.host);
if (j.name) S.name = String(j.name);
if (j.wss)  S.wss  = +j.wss || 8084;
if (j.secure === false) S.secure = false;
if (j.want) S.want = true;
} catch (e) {}
return S;
}
function save(patch) {
if (patch) Object.assign(S, patch);
try {
localStorage.setItem(LSKEY, JSON.stringify({
dev: S.dev, code: S.code, host: S.host, wss: S.wss,
secure: S.secure, want: S.want, name: S.name
}));
} catch (e) {}
}
/* ---------------- 遥控码 ---------------- */
// 用户输入 "A1B2C3-482913"（短码 + 密码），宽容一点：空格、大小写、横线随便
function setPair(text) {
const s = String(text || '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
if (s.length !== 12) return false;
S.dev = s.slice(0, 6);
S.code = s.slice(6);
save();
return true;
}
function pairText() {
return (S.dev && S.code) ? (S.dev + '-' + S.code) : '';
}
function paired() { return !!(S.dev && S.code); }
/* ---------------- 1.19.0 派生遥控码 ---------------- */
// 与 src/cloud.cpp 的 deriveFrom() 必须逐字节一致：
//   FNV-1a 32，先吃盐 "MyRadio-1"，再吃 12 位设备码（大写），取模 900000 加 100000。
// 测试向量见 tools/verify_cloud.js [10]：A1B2C3D4E5F6 → 274682
function deriveCode(dev) {
const t = 'MyRadio-1' + String(dev || '').toUpperCase();
let h = 2166136261 >>> 0;
for (let i = 0; i < t.length; i++) {
h ^= t.charCodeAt(i) & 0xFF;
h = Math.imul(h, 16777619) >>> 0;
}
return String(100000 + (h % 900000));
}
/* ---------------- 1.19.0 设备列表 ---------------- */
// 收到一条保留消息就更新一条。空消息 = 机器自己撤了广播（关了免配对 / 下线清理）。
function onDev(dev, text) {
const i = S.devs.findIndex(d => d.dev === dev);
if (!text) { if (i >= 0) S.devs.splice(i, 1); notifyDevs(); return; }
let j;
try { j = JSON.parse(text); } catch (e) { return; }
if (!j || !j.d) return;
const rec = {
dev: dev,
id: j.id || '',            // 12 位完整设备码：派生码是从它算出来的
n: j.n || ('MyRadio-' + dev),
fw: j.fw || '',
open: j.open === undefined ? 1 : (j.open ? 1 : 0),
batt: (j.batt === undefined || j.batt === null) ? -1 : +j.batt,
// 1.46.7：机器那 4 位连接码。1.46.7 之前的固件不发这个字段 —— 那就是空串，
//   输码找不到时界面要能说清「可能是机器固件太老」。
pin: j.pin ? String(j.pin).replace(/\D/g, '').slice(0, 4) : '',
seen: Date.now()
};
if (i >= 0) S.devs[i] = rec; else S.devs.push(rec);
notifyDevs();
}
function notifyDevs() {
if (typeof S.onDevs === 'function') { try { S.onDevs(S.devs); } catch (e) {} }
}
// 90 秒没再听到 = 离线（机器每 30 秒报一次）
function devAlive(d) { return d && (Date.now() - d.seen) < 90000; }
function findDev(dev) { return S.devs.find(d => d.dev === dev) || null; }
// 选一台机器来控：免配对的机器直接算码，闭的留空等用户手输
function pickDev(dev) {
const d = findDev(dev);
if (!d) return false;
const changed = (S.dev !== dev);
S.dev  = d.dev;
S.name = d.n || '';
if (d.open) S.code = deriveCode(d.id || d.dev);
else if (changed) S.code = '';
save();
if (S.want) { if (!S.conn) connect(); else subscribeDev(); }
return !!S.code;
}
function forgetDev() {
S.dev = ''; S.code = ''; S.name = '';
save();
}
/* ---------------- 1.46.7 输 4 位码连（跨网） ----------------
机器每 30 秒往 myradio/dev/<短码> 广播一次，payload 里已经带了自己的 4 位码。
所以跨网的「输码」就是**在已收到的广播里筛**，不用再问谁一句，也不用新主题：
· 只有一台用这个码  → 直接选它（这就是用户要的手感）
· 有多台（都用出厂 1234）→ 交给界面列出来点，机器不替人决定
· 一台都没有        → 可能没上线，也可能固件 < 1.46.7（那些机器不发 pin）
返回 {n, list, dev?, name?, needCode?}：
n=1 且 needCode=false → 界面直接进；
n=1 且 needCode=true  → 这台关了免配对，还得补 12 位码；
n>1 / n=0             → 界面按 list 显示。 */
function findByPin(pin) {
const p = String(pin || '').replace(/\D/g, '').slice(0, 4);
if (p.length !== 4) return [];
return S.devs.filter(d => devAlive(d) && d.pin === p)
.sort((a, b) => (b.seen || 0) - (a.seen || 0));
}
function pinLogin(pin) {
const hit = findByPin(pin);
if (hit.length === 1) {
const has = pickDev(hit[0].dev);       // 免配对机器顺手把派生码填好
return { n: 1, dev: hit[0].dev, name: hit[0].n, needCode: !has, list: hit };
}
return { n: hit.length, list: hit };
}
// 界面上「下面这些机器里挑一台」用：挑完就跟点列表一样
function pickPin(dev) {
const d = findDev(dev);
if (!d) return false;
return pickDev(dev);
}
// 有没有任何一台报了码？用来区分「没上线」和「固件太老不发码」
function anyPin() { return S.devs.some(d => devAlive(d) && d.pin); }
/* ---------------- MQTT 编码 ---------------- */
function pkt(type, body) {
const head = [type & 0xFF];
let x = body.length;
do {
let d = x % 128;
x = Math.floor(x / 128);
if (x > 0) d |= 0x80;
head.push(d);
} while (x > 0);
const r = new Uint8Array(head.length + body.length);
r.set(head, 0);
r.set(body, head.length);
return r;
}
function mstr(s) {                        // MQTT 里的字符串：2 字节长度 + UTF-8
const b = enc(s);
const r = new Uint8Array(2 + b.length);
r[0] = b.length >> 8;
r[1] = b.length & 255;
r.set(b, 2);
return r;
}
function cat(list) {
let n = 0;
for (const a of list) n += a.length;
const r = new Uint8Array(n);
let o = 0;
for (const a of list) { r.set(a, o); o += a.length; }
return r;
}
function connectPacket() {
const cid = 'mrr' + Math.random().toString(36).slice(2, 9);
return pkt(0x10, cat([
mstr('MQTT'),
new Uint8Array([4, 0x02, 0x00, 0x3C]),   // 协议级别 4 / cleanSession / keepalive 60s
mstr(cid)
]));
}
function subscribePacket(topic) {
const id = subId++;
return pkt(0x82, cat([
new Uint8Array([id >> 8, id & 255]),
mstr(topic),
new Uint8Array([0])                       // QoS 0
]));
}
function publishPacket(topic, payload) {
return pkt(0x30, cat([mstr(topic), payload]));
}
function pingPacket() { return pkt(0xC0, new Uint8Array(0)); }
// 从缓冲里切一个完整包；不够就返回 null 等下一片
function takePacket(buf) {
if (buf.length < 2) return null;
let mult = 1, len = 0, i = 1, d;
do {
if (i >= buf.length) return null;
d = buf[i++];
len += (d & 127) * mult;
mult *= 128;
if (mult > 128 * 128 * 128 * 128) return null;
} while (d & 128);
if (buf.length < i + len) return null;
return { type: buf[0] >> 4, flags: buf[0] & 15, body: buf.subarray(i, i + len), used: i + len };
}
/* ---------------- 收 ---------------- */
function push(chunk) {
const nx = new Uint8Array(rxbuf.length + chunk.length);
nx.set(rxbuf, 0);
nx.set(chunk, rxbuf.length);
rxbuf = nx;
for (;;) {
const p = takePacket(rxbuf);
if (!p) break;
rxbuf = rxbuf.subarray(p.used);
handle(p);
}
// 万一半包一直拼不齐，别把内存撑爆
if (rxbuf.length > 200000) rxbuf = new Uint8Array(0);
}
function handle(p) {
if (p.type === 2) {                       // CONNACK
const rc = p.body.length > 1 ? p.body[1] : 9;
if (rc === 0) {
S.conn = true;
S.phase = 'online';
S.err = '';
retryMs = 2000;
// 1.19.0：订 /r（回包），并订设备广播。1.8.0 误订了 /c。
// 还没选机器时别订 /r —— 否则订的是 "myradio//r" 这种空设备主题。
if (S.dev) ws.send(subscribePacket(topicR()));
ws.send(subscribePacket(DEVPRE + '+'));
startPing();
} else {
S.conn = false;
S.phase = 'err';
S.err = rc === 4 ? '遥控码/服务器拒绝了连接' : ('服务器返回 ' + rc);
try { ws.close(); } catch (e) {}
}
return;
}
if (p.type === 3) {                       // PUBLISH
const bl = p.body;
const tlen = (bl[0] << 8) | bl[1];
if (2 + tlen > bl.length) return;
const topic = dec(bl.subarray(2, 2 + tlen));
let off = 2 + tlen;
const qos = (p.flags >> 1) & 3;
if (qos > 0) off += 2;                  // 带 packet id（我们订的是 QoS0，理论上不会来）
if (off > bl.length) return;
S.rx++;
onMessage(topic, dec(bl.subarray(off)));
return;
}
if (p.type === 13) return;                // PINGRESP
if (p.type === 9) return;                 // SUBACK
}
function onMessage(topic, text) {
// 设备广播（1.19.0）：主题就是设备身份，正文是那台机器的自述
if (topic.indexOf(DEVPRE) === 0) { onDev(topic.slice(DEVPRE.length), text); return; }
if (text.indexOf('"off"') >= 0) {         // 遗嘱：机器掉线了
S.err = '收音机离线了';
return;
}
let j;
try { j = JSON.parse(text); } catch (e) { return; }
if (!j || !j.i) return;
const p = pending.get(j.i);
if (!p) return;
pending.delete(j.i);
clearTimeout(p.t);
if (j.s === 200) p.resolve(j.d || {});
else {
const e = new Error((j.d && j.d.err) || ('err ' + j.s));
e.code = j.s;
p.reject(e);
}
}
/* ---------------- 连接 ---------------- */
const topicC = () => 'myradio/' + S.dev + '/c';
const topicR = () => 'myradio/' + S.dev + '/r';
function startPing() {
stopPing();
pingT = setInterval(() => {
if (ws && S.conn) { try { ws.send(pingPacket()); } catch (e) {} }
}, 30000);
}
function stopPing() { if (pingT) clearInterval(pingT); pingT = 0; }
function connect() {
// 1.19.0：没配对也连 —— 得先连上才能"看见"机器（设备广播走同一条连接）
close();
S.phase = 'connecting';
S.err = '';
const url = (S.secure === false ? 'ws://' : 'wss://') + S.host + ':' + S.wss + '/mqtt';
try {
ws = new WebSocket(url, ['mqtt']);
} catch (e) {
S.phase = 'err';
S.err = '地址不对';
return;
}
ws.binaryType = 'arraybuffer';
ws.onopen = () => { try { ws.send(connectPacket()); } catch (e) {} };
ws.onmessage = ev => { try { push(new Uint8Array(ev.data)); } catch (e) {} };
ws.onerror = () => { if (!S.conn) { S.err = '连不上服务器'; S.phase = 'err'; } };
ws.onclose = () => {
S.conn = false;
stopPing();
pending.forEach(p => { clearTimeout(p.t); p.reject(new Error('closed')); });
pending.clear();
if (S.want) {
S.phase = 'connecting';
S.down++;
clearTimeout(retryT);
retryT = setTimeout(connect, retryMs);
retryMs = Math.min(retryMs * 2, 15000);
} else {
S.phase = 'idle';
}
};
}
function close() {
clearTimeout(retryT);
retryT = 0;
stopPing();
const w = ws;
ws = null;
S.conn = false;
if (w) { try { w.onclose = null; w.close(); } catch (e) {} }
}
// 换了要控的机器：补订它的 /r（已订的重复 SUBSCRIBE 无害）
function subscribeDev() {
if (!ws || !S.conn || !S.dev) return;
try { ws.send(subscribePacket(topicR())); } catch (e) {}
}
function enable(on) {
S.want = !!on;
save();
if (S.want) connect();
else { close(); S.phase = 'idle'; }
}
/* ---------------- 发请求 ---------------- */
function request(path, body, method) {
return new Promise((resolve, reject) => {
if (!ws || !S.conn) { reject(new Error('offline')); return; }
if (!S.dev) { reject(new Error('nodev')); return; }   // 还没选机器（1.19.0 会发现阶段）
const i = seq++;
const t = setTimeout(() => {
pending.delete(i);
reject(new Error('timeout'));
}, 9000);
pending.set(i, { resolve, reject, t });
let b = '';
if (body && typeof body === 'object') b = new URLSearchParams(body).toString();
else if (typeof body === 'string') b = body;
const js = JSON.stringify({ i: i, k: S.code, m: method || 'GET', p: path, b: b });
try {
ws.send(publishPacket(topicC(), enc(js)));
S.tx++;
} catch (e) {
clearTimeout(t);
pending.delete(i);
reject(e);
}
});
}
/* ---------------- 给界面看 ---------------- */
function active() { return S.want && S.conn; }
function statusText() {
if (S.phase === 'online' && !S.dev) return '已连服务器，等选机器';
if (S.phase === 'online' && !S.code) return '这台要输遥控码';
if (!S.dev) return S.want ? '正在找机器…' : '没开';
if (S.phase === 'online') return '已连上，随时可控';
if (S.phase === 'connecting') return '正在连服务器…';
if (S.phase === 'err') return S.err || '连不上';
return S.want ? '等 WiFi' : '没开';
}
return {
S: S, load: load, save: save,
setPair: setPair, pairText: pairText, paired: paired,
connect: connect, close: close, enable: enable,
request: request, active: active, statusText: statusText,
deriveCode: deriveCode, findDev: findDev, devAlive: devAlive,
pickDev: pickDev, forgetDev: forgetDev, subscribeDev: subscribeDev,
findByPin: findByPin, pinLogin: pinLogin, pickPin: pickPin, anyPin: anyPin,
_devpre: DEVPRE,
// 测试用：拿底层编码结果做断言
_pkt: pkt, _str: mstr, _take: takePacket, _topicC: topicC, _topicR: topicR
};
})();
;
/* ==== core.js ==== */
/* ============================================================
core.js —— 工具、接口、屏幕镜像、主页控制、导航
说明：三个 js 文件会被拼进同一个 <script>，所以这里定义的
const/S 是全局的，不要在别的文件里重名。
============================================================ */
'use strict';
const $  = (s, root) => (root || document).querySelector(s);
const $$ = (s, root) => Array.prototype.slice.call((root || document).querySelectorAll(s));
const INIT = [];
/* ---------------- 格式化 ---------------- */
function esc(s){
return String(s == null ? '' : s).replace(/[&<>"']/g, c => (
{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]
));
}
function fmtDur(sec){
sec = Math.max(0, Math.round(sec || 0));
if (sec < 60) return sec + ' 秒';
const m = Math.floor(sec / 60), h = Math.floor(m / 60);
if (!h) return m + ' 分钟';
return h + ' 小时' + (m % 60 ? ' ' + (m % 60) + ' 分' : '');
}
function fmtShort(sec){          // 统计卡用：13时55分 / 41分钟
sec = Math.max(0, Math.round(sec || 0));
const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60;
if (h) return h + '<small>时</small>' + m + '<small>分</small>';
if (m) return m + '<small>分钟</small>';
return sec + '<small>秒</small>';
}
function fmtRel(unix){
if (!unix) return '时间未知';
const d = new Date(unix * 1000), n = Date.now();
const diff = (n - d.getTime()) / 1000;
if (diff < 90) return '刚刚';
if (diff < 3600) return Math.round(diff / 60) + ' 分钟前';
const today = new Date(); today.setHours(0,0,0,0);
if (d >= today) return '今天 ' + String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
const y = new Date(today); y.setDate(y.getDate() - 1);
if (d >= y) return '昨天 ' + String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
return (d.getMonth() + 1) + '月' + d.getDate() + '日';
}
function fmtUp(sec){
sec = Math.round(sec || 0);
const d = Math.floor(sec / 86400), h = Math.floor(sec / 3600) % 24, m = Math.floor(sec / 60) % 60;
if (d) return d + ' 天 ' + h + ' 小时';
if (h) return h + ' 小时 ' + m + ' 分';
return m + ' 分钟';
}
function ic(name, cls){ return '<svg class="' + (cls || 'ic') + '"><use href="#i-' + name + '"></use></svg>'; }
/* ---------------- 轻提示 ---------------- */
let _toastT = 0;
function toast(msg, bad){
const t = $('#toast');
$('#toastTx').textContent = msg;
t.classList.toggle('bad', !!bad);
t.querySelector('use').setAttribute('href', bad ? '#i-circle-alert' : '#i-circle-check');
t.classList.add('show');
clearTimeout(_toastT);
_toastT = setTimeout(() => t.classList.remove('show'), 2200);
}
/* ---------------- 底部弹层 ---------------- */
let _sheetEl = null;
function openSheet(html, onOpen){
closeSheet(true);
const host = $('#sheetHost');
host.innerHTML = '<div class="mask" id="mask"><div class="sheet"><div class="grab"></div>' + html + '</div></div>';
const mask = $('#mask');
_sheetEl = mask;
mask.addEventListener('click', e => { if (e.target === mask) closeSheet(); });
requestAnimationFrame(() => requestAnimationFrame(() => mask.classList.add('in')));
if (onOpen) onOpen($('.sheet', host));
return mask;
}
function closeSheet(instant){
const m = _sheetEl;
if (!m) return;
_sheetEl = null;
if (instant) { $('#sheetHost').innerHTML = ''; return; }
m.classList.remove('in');
setTimeout(() => { if (!_sheetEl) $('#sheetHost').innerHTML = ''; }, 260);
}
function confirmSheet(o){
openSheet(
'<h3>' + esc(o.title) + '</h3>' +
(o.desc ? '<p class="desc">' + o.desc + '</p>' : '') +
'<div class="acts">' +
'<button class="btn ghost" data-x="no">' + esc(o.cancel || '取消') + '</button>' +
'<button class="btn ' + (o.danger ? 'danger' : 'pri') + '" data-x="yes">' + esc(o.ok || '确定') + '</button>' +
'</div>',
sheet => {
$('[data-x="no"]', sheet).onclick = closeSheet;
$('[data-x="yes"]', sheet).onclick = () => { closeSheet(); o.onOk && o.onOk(); };
}
);
}
/* ---------------- 接口 ---------------- */
const NET = { tk: '', fails: 0, lat: 0, busy: false, base: '', qTok: false };
/* ============================================================
1.38.0：自动找机器（用户要的「打开 app 就是配好的」）
------------------------------------------------------------
base 是「这台机器」的地址前缀：'' = 同源（App 就是从收音机加载的），
或者 'http://192.168.1.23' 这种（收音机在路由器后面，App 从别处打开）。
探测顺序有讲究：
同源优先 —— 如果页面本身就是收音机给的，那一定是对的，零延迟；
然后 myradio.local（mDNS，iOS/桌面可用，Android 上常常不通，所以不指望它）；
再是上次成功过的 IP（换了 WiFi 之后 IP 会变，但常常不变）；
最后 192.168.4.1（收音机自己的热点，用户刚配网时就在这个网里）。
谁先应答就用谁，之后所有请求都打到那个主机上。
============================================================ */
function hostOf(path){
// 已经确定过 base，直接用
if (NET.base) return NET.base + path;
return path;                       // base 还没定/同源
}
function rememberHost(){
try{
if (NET.base) localStorage.setItem('mr_host', NET.base);
else localStorage.removeItem('mr_host');
}catch (e){}
}
/* 试一个候选是不是真的收音机；是就返回它的 /api/info */
async function probeHost(base){
const ctl = new AbortController();
const t = setTimeout(() => ctl.abort(), 1800);
try{
const r = await fetch(base + '/api/info', { cache: 'no-store', signal: ctl.signal });
clearTimeout(t);
if (!r.ok) return null;
const j = await r.json();
return j && j.name ? j : null;
}catch (e){ clearTimeout(t); return null; }
}
/* 依次探测；找到就设 NET.base 并记住。返回设备信息或 null */
async function findRadio(){
const cands = [''];
const saved = (function(){ try{ return localStorage.getItem('mr_host') || ''; }catch(e){ return ''; } })();
if (saved) cands.push(saved);
cands.push('http://myradio.local', 'http://192.168.4.1');
const seen = new Set();
for (const c of cands){
if (seen.has(c)) continue;
seen.add(c);
const j = await probeHost(c);
if (!j) continue;
// 只认这一台：第一次连上就记下设备码；以后连到别的机器，不悄悄换过去
let want = '';
try{ want = localStorage.getItem('mr_dev') || ''; }catch (e){}
const dev = j.devid || '';
if (!want && dev){
try{ localStorage.setItem('mr_dev', dev); }catch (e){}
}
if (want && dev && dev !== want){
console.warn('[MyRadio] 找到的是另一台机器 ' + dev + '，这台是 ' + want);
continue;                     // 不是我家那台，跳过
}
NET.base = c;
rememberHost();
return j;
}
return null;
}
async function authOnce(){
const tk = localStorage.getItem('mr_tk') || '';
const r = await fetch(hostOf('/api/auth'), { cache: 'no-store' });
const j = await r.json();
if (j.locked) { const e = new Error('locked'); e.locked = true; throw e; }
localStorage.setItem('mr_tk', j.token);
NET.tk = j.token;
return j.token;
}
async function authCode(code){
const r = await fetch('/api/auth?code=' + encodeURIComponent(code), { cache: 'no-store' });
const j = await r.json();
if (j.locked || !j.token) { const e = new Error('code'); e.locked = true; throw e; }
localStorage.setItem('mr_tk', j.token);
NET.tk = j.token;
return j.token;
}
/* 1.8.0：两个通道
useCloud() 为真  → 走云端（MQTT），跨网也能用
否则             → 走局域网 HTTP（同网时最快）
云端那条路只有在「远程版」页面（window.MR_REMOTE）里才是首选；
局域网页面里只有当 HTTP 彻底不通、而云端又恰好连着时才兜底。 */
function useCloud(){
return !!window.MR_REMOTE || !!(window.NET && NET.forceCloud);
}
/* 1.46.19：固件那边如果没在 server.collectHeaders() 里登记 X-Token，
这个头会被整个丢掉（server.header() 永远是空串）—— 页面就会一路 401，
用户看到的就是「进得去首页、但顶上写没连上收音机」。
所以头这条路走不通时改走 ?t=<token>（固件 authed() 一直认这个参数）；
一旦发现 ?t= 能用，本次会话就都用它，别每次都白挨一轮 401。 */
function tokQ(url){
return url + (url.indexOf('?') < 0 ? '?' : '&') + 't=' + encodeURIComponent(NET.tk);
}
async function getLan(path){
if (!NET.tk) await authOnce();
const url = hostOf(path);
if (NET.qTok){
const rq = await fetch(tokQ(url), { cache: 'no-store' });
if (rq.ok) return rq.json();
if (rq.status !== 401) throw new Error('http ' + rq.status);
NET.qTok = false;                       // 令牌失效了，下面重走一遍认证
}
let r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' });
if (r.status === 401) { await authOnce(); r = await fetch(url, { headers: { 'X-Token': NET.tk }, cache: 'no-store' }); }
if (r.status === 401) {
r = await fetch(tokQ(url), { cache: 'no-store' });
if (r.ok) NET.qTok = true;
}
if (!r.ok) throw await httpErr(r);
return r.json();
}
// 1.46.36：非 2xx 时把 body 里的 {"err":"..."} 也读出来。
//   固件对「参数不对 / 没有这一组 / 写盘失败」都是 errJson(4xx, "xxx")，
//   只抛 "http 404" 的话，按 e.code 分流的分支全都进不去，用户只会看到一句「没成功」。
async function httpErr(r){
let code = '';
try{
const b = await r.json();
if (b && b.err) code = String(b.err);
}catch (e){}
const err = new Error(code || ('http ' + r.status));
if (code) err.code = code;
err.status = r.status;
return err;
}
async function postLan(path, body){
if (!NET.tk) await authOnce();
const url = hostOf(path);
const send = (u, tok) => fetch(u, {
method: 'POST',
headers: tok
? { 'X-Token': NET.tk, 'Content-Type': 'application/x-www-form-urlencoded' }
: { 'Content-Type': 'application/x-www-form-urlencoded' },
body: new URLSearchParams(body || {})
});
if (NET.qTok){
const rq = await send(tokQ(url), false);
if (rq.ok) return rq.json();
if (rq.status !== 401) throw new Error('http ' + rq.status);
NET.qTok = false;
}
let r = await send(url, true);
if (r.status === 401) { await authOnce(); r = await send(url, true); }
if (r.status === 401) {
const rq = await send(tokQ(url), false);
if (rq.ok) NET.qTok = true;
r = rq;
}
if (!r.ok) throw await httpErr(r);
return r.json();
}
async function get(path){
if (useCloud() && CLOUD.active()) return CLOUD.request(path, null, 'GET');
if (useCloud()) throw new Error('cloud down');
try{
return await getLan(path);
}catch (e){
if (CLOUD.active()) return CLOUD.request(path, null, 'GET');   // 家里也连不上时兜底
throw e;
}
}
// 1.46.33：动作类接口出错时固件回的是 200 + {"err":"..."}，fetch 根本不算失败。
//   以前一律 .then(() => toast('已删除')) —— 机器明明没删，App 说"已删除"，
//   用户再拉一次列表发现台还在，就成了「删除失败 / 没设上」。
//   凡是有"成功/失败"语义的调用都改走这个。
async function postOk(path, body){
const j = await post(path, body);
if (j && j.err){ const e = new Error(String(j.err)); e.code = j.err; throw e; }
if (j && (j.ok === 0 || j.ok === false)){ const e = new Error('nok'); e.code = 'nok'; throw e; }
return j;
}
async function post(path, body){
if (useCloud() && CLOUD.active()) return CLOUD.request(path, body, 'POST');
if (useCloud()) throw new Error('cloud down');
try{
return await postLan(path, body);
}catch (e){
if (CLOUD.active()) return CLOUD.request(path, body, 'POST');
throw e;
}
}
/* ---------------- 配对码门 ---------------- */
function pairGate(){
const b = $('#boot');
b.style.display = '';
b.classList.remove('gone');
b.innerHTML =
'<div class="logo">' + ic('key-round') + '</div>' +
'<div style="text-align:center;max-width:300px;padding:0 24px">' +
'<div style="font-size:17px;font-weight:800;margin-bottom:6px">这台收音机开了配对码保护</div>' +
'<div style="font-size:13.5px;color:var(--ink2);line-height:1.7;margin-bottom:16px">' +
'看一下收音机的屏幕，上面有 6 个数字，输进来就能控制它。' +
'屏幕没显示的话，按一下旋钮。</div>' +
'<input id="pcIn" inputmode="numeric" autocomplete="one-time-code" maxlength="6" ' +
'style="width:100%;height:54px;text-align:center;letter-spacing:10px;font-size:26px;' +
'font-weight:800;border-radius:16px;border:1px solid var(--line);background:var(--card);' +
'outline:none;padding-left:10px" placeholder="······">' +
'<button class="btn pri" id="pcGo" style="margin-top:12px;width:100%">连接</button>' +
'<button class="btn ghost" id="pcAgain" style="margin-top:8px;width:100%;height:42px;font-size:14px">' +
'再让屏幕显示一次</button>' +
'</div>';
$('#pcGo').onclick = async () => {
const v = ($('#pcIn').value || '').trim();
if (v.length !== 6) { toast('要 6 个数字', true); return; }
try { await authCode(v); location.reload(); }
catch (e) { toast('配对码不对，再看看屏幕', true); }
};
$('#pcAgain').onclick = async () => {
try { await fetch('/api/auth', { cache: 'no-store' }); } catch (e) {}
toast('看收音机的屏幕');
};
}
/* ---------------- 全局状态 ---------------- */
const S = {
scr: null,
tags: [],          // 1.15.0：标签清单（机器不再分组，标签只用于筛选）
tag: -1,           // 当前筛的标签下标，-1 = 全部（只在本机生效）
list: [],          // 整张台表（台数随台表变，别写死）
page: 'home',
diag: null,
hist: null,
pair: null,
q: '',
offline: false,     // 连不上收音机时置位：不拦路，让用户进来配网
hl: null,           // 1.17.0：/api/health 的最新一份（每台的健康档案）
hlOff: false        // 老固件没有这个接口时置位，界面就不显示那些标
};
/* ============================================================
屏幕镜像：320×170，坐标与 src/ui.cpp 一一对应
============================================================ */
const SCR_W = 320, SCR_H = 170;
const cv = document.getElementById('scr');
const ctx = cv.getContext('2d');
const C = {
bg:'#000000', name:'#e9e9e9', accent:'#b06b03', cool:'#01839d',
sub:'#cfcece', barBg:'#202020', namBg:'#004e61', namFg:'#ffff00',
bandBg:'#b06b03', bandFg:'#000000', modeBg:'#004e61', modeFg:'#e9e9e9',
time:'#cfcece', err:'#b20a0a', dim:'#8c8e8c',
// 调谐盘配色，与固件 config.h 的 C_DIAL_* 一致
dialBg:'#084d5a', dialFg:'#eef4f4', dialMk:'#c8e8e8', dialPtr:'#b20a0a'
};
const F16 = 'bold 15px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';
const F16R = '600 15px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';
const F32 = 'bold 31px "PingFang SC","Microsoft YaHei",system-ui,sans-serif';
function fitCanvas(){
const w = cv.clientWidth || 320;
const dpr = Math.min(window.devicePixelRatio || 1, 3);
cv.width  = Math.round(w * dpr);
cv.height = Math.round(w * SCR_H / SCR_W * dpr);
ctx.setTransform(cv.width / SCR_W, 0, 0, cv.height / SCR_H, 0, 0);
}
function rect(x, y, w, h, c){
ctx.fillStyle = c;
ctx.fillRect(x, y, w, h);
}
function textW(s, font){
ctx.font = font;
return ctx.measureText(s).width;
}
function text(s, x, y, c, font){
ctx.fillStyle = c;
ctx.font = font;
ctx.textAlign = 'left';
ctx.textBaseline = 'top';
ctx.fillText(s, x, y);
}
/* 在色块里「垂直居中」写字。
canvas 的 textBaseline='top' 对齐的是 em 框顶，中文字形实际在 y-1..y+11 之间（15px 字号实测），
measureText 的 actualBoundingBox 各浏览器符号不一致、这里不依赖它，直接用实测字形高。 */
function textInBox(s, x, boxY, boxH, c, font, align){
ctx.font = font;
ctx.fillStyle = c;
ctx.textAlign = align || 'left';          // 'left' 时 x 是左缘，'right' 时是右缘
ctx.textBaseline = 'top';
const gh = 13;                            // 15px 字形的实际高度
ctx.fillText(s, x, Math.round(boxY + (boxH - gh) / 2));
ctx.textAlign = 'left';
}
function textR(s, xr, y, c, font){
ctx.fillStyle = c;
ctx.font = font;
ctx.textAlign = 'right';
ctx.textBaseline = 'top';
ctx.fillText(s, xr, y);
ctx.textAlign = 'left';
}
function textC(s, y, c, font){
ctx.fillStyle = c;
ctx.font = font;
ctx.textAlign = 'center';
ctx.textBaseline = 'top';
ctx.fillText(s, SCR_W / 2, y);
ctx.textAlign = 'left';
}
function ellipsis(s, maxW, font){
ctx.font = font;
if (ctx.measureText(s).width <= maxW) return s;
let out = s;
while (out.length > 1 && ctx.measureText(out + '…').width > maxW) out = out.slice(0, -1);
return out + '…';
}
/* 台名里写着真实频率的，盘上就按它摆指针：FM89.1 / 电台914 / 1008 / FM100
规则与 src/ui.cpp 的 nameFreq10 一一对应，改一边必须改另一边，否则镜像会对不上。 */
function nameFreq10(s){
s = s || '';
for (let i = 0; i < s.length; ){
const ch = s[i];
if (ch < '0' || ch > '9'){ i++; continue; }
let j = i, v = 0, nd = 0;
while (j < s.length && s[j] >= '0' && s[j] <= '9'){
if (nd < 5){ v = v * 10 + (+s[j]); nd++; }
j++;
}
if (nd > 4){ i = j; continue; }             // 5 位以上：年份 / 流水号
let f = -1;
if (s[j] === '.' && s[j + 1] >= '0' && s[j + 1] <= '9'){
f = v * 10 + (+s[j + 1]);
j += 2;
while (j < s.length && s[j] >= '0' && s[j] <= '9') j++;
}
if (f < 0){
const tenths = v, whole = v * 10;
if (tenths >= 640 && tenths <= 1080) f = tenths;
else if (whole >= 640 && whole <= 1080) f = whole;
}
if (f >= 640 && f <= 1080) return f;
i = j > i ? j : i + 1;
}
return -1;
}
// 信号柱（4 格）
function sigBars(x, y, ok, rssi){
let bars = 0;
if (ok) bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
for (let i = 0; i < 4; i++){
const h = 4 + i * 3;
rect(x + i * 6, y + 13 - h, 4, h, i < bars ? C.cool : C.barBg);
}
}
// 1.44.0：音量两路量程不同（原厂 volume 0..63 / netvolume 0..21）。
//   显示、进度条一律按「当前这一路」的 max 算 —— 写死 100 会让 FM 的音量 40
//   只画出 40% 长，和屏幕上（ui.cpp）那条对不上。
function volMaxOf(s){
const fm = !!s && s.mode === 'fm';
const mx = fm ? s.fvolmax : s.volmax;
return (mx != null && +mx > 0) ? +mx : (fm ? 63 : 21);
}
// 音量胶囊（1.4.5：与色块同高 20，数字居中，不再顶出色块）
function volPill(x, y, vol, muted, vmax){
const w = 52, h = 20;
rect(x, y, w, h, C.barBg);
ctx.strokeStyle = muted ? C.accent : C.cool; ctx.lineWidth = 1;
ctx.strokeRect(x + .5, y + .5, w - 1, h - 1);
const fw = muted ? 0 : Math.round(vol * (w - 22) / (vmax || 100));
if (fw > 0) rect(x + 2, y + 2, fw, h - 4, C.cool);
const label = String(muted ? 0 : vol);
textInBox(label, x + w - 3, y, h, muted ? C.accent : C.sub, F16R, 'right');
}
function badge(x, y, label, fg, bg){
const w = textW(label, F16R) + 8;
rect(x, y, w, 20, bg);
textInBox(label, x + 4, y, 20, fg, F16R);   // 垂直居中，不贴上下边
return x + w + 4;
}
function tri(x1, y1, x2, y2, x3, y3, c){
ctx.fillStyle = c;
ctx.beginPath();
ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3);
ctx.closePath(); ctx.fill();
}
function psColor(s){
if (s.mode === 'fm') return s.mute ? C.sub : C.cool;   // 收音不靠网络，红点会让人以为坏了
if (!s.wifi) return C.err;
switch (s.ps){
case 2: return C.cool;
case 1: return C.accent;
case 3: return C.accent;
case 4: return C.err;
default: return C.sub;
}
}
// 台数到三位（100/140）时序号占掉半个框宽，状态词要退成 2 字
function psShort(lbl){
return ({ '播放中':'播放', '连接中':'连接', '缓冲中':'缓冲', '出错了':'出错',
'已暂停':'暂停', '已静音':'静音',
'调频收音':'调频', '中波收音':'中波', '长波收音':'长波', '短波收音':'短波' })[lbl] || lbl;
}
function psLabel(s){
if (s.mode === 'fm') return s.mute ? '已静音' : radioBandWord(radioKhz(s)) + '收音';
switch (s.ps){
case 2: return '播放中';
case 1: return '连接中';
case 3: return '缓冲中';
case 4: return '出错了';
default: return s.mute ? '已静音' : '已暂停';
}
}
/* ---------------- 收音波段环（1.10.0）：一个旋钮走完 长波→中波→调频→短波 ----------------
与 src/fm.cpp 的 kBands / radioWalk() 一一对应，频率单位全部是 kHz。
长波 153~279、中波 531~1602、调频 64000~108000、短波 2300~26100。
跨过波段边界直接跳到下一波段的第一格 —— 空白频段不占格子，所以「不用切波段」。 */
const RBANDS = [
{ k: 'LW', lo: 153,   hi: 279,    step: 9   },
{ k: 'MW', lo: 531,   hi: 1602,   step: 9   },
{ k: 'FM', lo: 64000, hi: 108000, step: 100 },
{ k: 'SW', lo: 2300,  hi: 26100,  step: 5   },
];
function radioClosest(khz){
let best = RBANDS[0].lo, bd = Infinity;
for (const b of RBANDS){
for (const e of [b.lo, b.hi]){
const d = Math.abs(khz - e);
if (d < bd){ bd = d; best = e; }
}
}
return best;
}
function radioBandOf(khz){
for (let i = 0; i < RBANDS.length; i++)
if (khz >= RBANDS[i].lo && khz <= RBANDS[i].hi) return i;
const near = radioClosest(khz);
for (let i = 0; i < RBANDS.length; i++)
if (near >= RBANDS[i].lo && near <= RBANDS[i].hi) return i;
return 0;
}
function radioSnap(khz){
for (const b of RBANDS) if (khz >= b.lo && khz <= b.hi) return khz;
return radioClosest(khz);
}
function radioWalk(khz, clicks, gear){
let k = radioSnap(khz), bi = radioBandOf(k);
if (!gear || gear < 1) gear = 1;
let n = Math.abs(clicks); if (n > 400) n = 400;
for (let i = 0; i < n; i++){
const st = RBANDS[bi].step * gear;
if (clicks > 0){
let v = k + st;
if (v > RBANDS[bi].hi){ bi = (bi + 1) % RBANDS.length; v = RBANDS[bi].lo; }
k = v;
} else {
let v = k - st;
if (v < RBANDS[bi].lo){ bi = (bi + RBANDS.length - 1) % RBANDS.length; v = RBANDS[bi].hi; }
k = v;
}
}
return k;
}
// 转速 → 加速倍数：慢转 1（精调）、正常 5、快转 10、甩起来 30
function radioGear(msPerClick){
if (msPerClick <= 60)  return 30;
if (msPerClick <= 130) return 10;
if (msPerClick <= 400) return 5;
return 1;
}
function radioBandWord(khz){
const k = RBANDS[radioBandOf(khz)].k;
return ({ LW: '长波', MW: '中波', SW: '短波' })[k] || '调频';
}
function radioFreqText(khz){
if (RBANDS[radioBandOf(khz)].k === 'FM') return 'FM ' + (khz / 1000).toFixed(1);
return radioBandWord(khz) + ' ' + khz;
}
// 老固件（1.9.0 及以前）只给 fmfreq（100kHz），新固件给 khz + band
function radioKhz(s){
if (s && typeof s.khz === 'number' && s.khz > 0) return s.khz;
return (s && s.fmfreq) ? s.fmfreq * 100 : 98000;
}
/* ---------------- 有声书（1.9.0）：一个台 = 一部书 ----------------
固件在 /api/screen 里给 epn（共几集）/ epi（第几集，0 基）/ ept（这一集讲什么），
普通台 epn=0，这里返回 null，界面就整块藏起来。 */
function epInfo(s){
const n = s && s.epn ? s.epn : 0;
if (n <= 1) return null;
return { n: n, i: Math.max(0, Math.min(n - 1, s.epi | 0)), t: s.ept || '', prog: s.prog || '' };
}
function drawScreen(s){
rect(0, 0, SCR_W, SCR_H, C.bg);
// 熄屏
if (!s.on){
textC('屏幕已熄灭', 62, '#4a4a4a', F16R);
textC('转一下旋钮就亮', 86, '#3a3a3a', F16R);
return;
}
// 配对码页（占满全屏）
if (s.pc){
rect(0, 0, SCR_W, 3, C.accent);
textC('配对码', 26, C.sub, F16R);
textC('······', 48, C.name, F32);
textC('在手机上输入这 6 个数字', 108, C.sub, F16R);
textC('输完就能控制收音机', 132, C.dim, F16R);
return;
}
/* ---- 顶栏 0..22：时钟 + 星期 + 组名 | WiFi + 信号 + 音量 ---- */
let x = 5;
const clk = s.clock || '--:--';
text(clk, x, 3, C.time, F16R);
x += textW(clk, F16R) + 6;
if (s.wd >= 0){
const wd = ['周日','周一','周二','周三','周四','周五','周六'][s.wd % 7];
text(wd, x, 3, C.sub, F16R);
x += textW(wd, F16R) + 8;
}
const wf = s.wifi ? 'WiFi' : '断网';
const wbg = s.wifi ? C.modeBg : C.err;
const ww = textW(wf, F16R) + 8;
const rightX = 232 - 4 - ww;
// 1.15.0：这块显示的是「当前这台所属的标签」，没有标签就不画
const gname = s.gname ? ellipsis(s.gname, 66, F16R) : '';
let gbw = gname ? textW(gname, F16R) + 8 : 0;
if (x + gbw > rightX - 8) gbw = 0;   // 标签色块不许压到右侧
if (gbw > 0){
rect(x, 1, gbw, 20, C.bandBg);
textInBox(gname, x + 4, 1, 20, C.name, F16R);
}
rect(rightX, 1, ww, 20, wbg);
textInBox(wf, rightX + 4, 1, 20, C.modeFg, F16R);
sigBars(232, 2, !!s.wifi, s.rssi);
volPill(SCR_W - 5 - 52, 1, s.vol, s.mute, volMaxOf(s));
/* ---- 台名 22..98（76 高）：FM 收音时这里是频率 ---- */
const n = s.cnt || 0;
const isFm = s.mode === 'fm';
const bigName = isFm ? radioFreqText(radioKhz(s))
: ellipsis(s.name || '没有电台', SCR_W - 20, F32);
const ep = isFm ? null : epInfo(s);
if (ep){
// 有声书：书名上移 6px，下面补一行「第 N 集 + 这一集讲什么」（与 ui.cpp 同坐标）
textC(bigName, 22 + 6, s.pv >= 0 ? C.sub : C.name, F32);
const sub2 = '第' + (ep.i + 1) + '集' + (ep.t ? ' ' + ep.t : '');
textC(ellipsis(sub2, SCR_W - 40, F16R), 22 + 76 - 22, C.sub, F16R);
} else {
textC(bigName, 22 + Math.round((76 - 31) / 2), s.pv >= 0 ? C.sub : C.name, F32);
}
/* ---- 三个小框 102..126 ---- */
const by = 102, bh = 24;
rect(5, by, 118, bh, C.barBg);
ctx.fillStyle = psColor(s);
ctx.beginPath(); ctx.arc(15, by + bh / 2, 4, 0, Math.PI * 2); ctx.fill();
// 有声书这一格显示「第几集 / 共几集」，普通台还是「第几台 / 共几台」
const ord = isFm ? ''                      // 收音模式：右边「第几台」没意义
: (ep ? ((ep.i + 1) + '/' + ep.n)
: ((n > 0 ? (s.idx + 1) : 0) + '/' + n));
const ordW = textW(ord, F16R);
let lbl = s.wifi ? psLabel(s) : '断网';
if (textW(lbl, F16R) + ordW > 123 - 23 - 6 - 8) lbl = psShort(lbl);
textInBox(lbl, 23, by, bh, C.name, F16R);
textInBox(ord, 5 + 118 - 6, by, bh, C.sub, F16R, 'right');
rect(128, by, 86, bh, C.barBg);
let bars = 0;
if (s.wifi) bars = s.rssi > -55 ? 4 : s.rssi > -67 ? 3 : s.rssi > -78 ? 2 : 1;
for (let i = 0; i < 4; i++){
const hh = 4 + i * 3;
rect(128 + 8 + i * 7, by + 19 - hh, 5, hh, i < bars ? C.cool : C.bg);
}
textInBox(String(s.rssi), 128 + 86 - 7, by, bh, C.sub, F16R, 'right');
rect(219, by, 96, bh, C.barBg);
textInBox('音量', 225, by, bh, s.mute ? C.accent : C.sub, F16R);
textInBox(String(s.mute ? 0 : s.vol), 219 + 96 - 6, by, bh, C.name, F16R, 'right');
rect(225, by + 21, 84, 2, C.bg);   // 条压到框底，上面整块留给文字
const vfw = s.mute ? 0 : Math.round(s.vol * 84 / volMaxOf(s));
if (vfw > 0) rect(225, by + 21, vfw, 2, C.cool);
/* ---- 调谐盘 134..170：指针钉在正中，刻度随台滚动（与 ui.cpp 一致） ---- */
const TUN_Y = 134, TUN_H = SCR_H - TUN_Y;
const TRI_H = 8, TRI_TOP = TUN_Y - TRI_H;  // 蓝条上方那条带子，放倒三角
const DIAL_PX = 5, DIAL_CX = SCR_W >> 1;   // 每 0.1MHz 占 5px，指针永不动
rect(0, TUN_Y, SCR_W, TUN_H, C.dialBg);
rect(0, TRI_TOP, SCR_W, TRI_H, C.bg);      // 空带擦干净，倒三角才不拖影
const base = TUN_Y + TUN_H - 1;
// 收音模式：按当前波段的频道栅格铺刻度（与 ui.cpp 的 drawTuner 同规则）
if (isFm){
const khz = radioKhz(s);
const band = RBANDS[radioBandOf(khz)];
const curT = (khz - band.lo) / band.step;
const half = Math.floor(SCR_W / (2 * DIAL_PX));
const lab  = (band.k === 'MW' || band.k === 'LW') ? 10 : 20;
for (let t = curT - half; t <= curT + half; t++){
const k = band.lo + t * band.step;
if (k < band.lo || k > band.hi) continue;
const x = DIAL_CX + (t - curT) * DIAL_PX;
if (x < 0 || x >= SCR_W) continue;
const isLab = (t % lab === 0) || (k === band.hi);
const isMid = (t % 5 === 0);
const hh = isLab ? 16 : (isMid ? 6 : 3);
rect(x, base - hh + 1, 1, hh, isLab ? C.dialFg : C.dialMk);
}
for (let t = curT - half; t <= curT + half; t++){
const k = band.lo + t * band.step;
if (k < band.lo || k > band.hi) continue;
if ((t % lab !== 0) && (k !== band.hi)) continue;
const lb = band.k === 'FM' ? (k / 1000).toFixed(1) : String(k);
const tw = textW(lb, F16R);
const tx = DIAL_CX + (t - curT) * DIAL_PX - Math.round(tw / 2);
if (tx < 1 || tx + tw > SCR_W - 1) continue;
text(lb, tx, TUN_Y + 2, C.dialFg, F16R);
}
rect(DIAL_CX - 1, TUN_Y, 2, TUN_H, C.dialPtr);
tri(DIAL_CX, TUN_Y + 2, DIAL_CX - 5, TRI_TOP, DIAL_CX + 5, TRI_TOP, C.dialPtr);
return;
}
const idx2 = Math.min(Math.max(0, n - 1), Math.max(0, s.idx | 0));
// 台名里写了频率就认它（FM89.1 → 891），没写才按第几台摊（与 ui.cpp 同规则）
let fcur = 975;
{
const nf = nameFreq10(s.name);
if (nf > 0) fcur = nf;
else if (n > 0) fcur = n <= 1 ? 975 : 875 + Math.floor(205 * idx2 / (n - 1));
}
const left = fcur - Math.round(DIAL_CX / DIAL_PX);
const right = left + Math.floor(SCR_W / DIAL_PX);
// 刻度：每 0.5 MHz 短（6px）、每 1 MHz 中（10px）、每 2 MHz 长（16px，带数字）
// 不夹在 87.5~108 之间——整条屏都铺刻度，第 1 台指针左边也有刻度可看
let f0 = Math.floor(left / 5) * 5;   // 向下取整，x=0 处就有刻度
let fEnd = right;
// 网络台的频率是按台序编出来的，整条屏都铺刻度
for (let f10 = f0; f10 <= fEnd; f10++){
const isHalf = (f10 % 5) === 0;
const isMhz = (f10 % 10) === 0;
const isBig = isMhz && (((f10 / 10) % 2) === 0);   // 88 / 90 / 92 … / 108
const hh = isBig ? 16 : (isMhz ? 10 : (isHalf ? 6 : 3));
const x = (f10 - left) * DIAL_PX;
rect(x, base - hh + 1, 1, hh, isMhz ? C.dialFg : C.dialMk);
}
// 频率数字：对准 2MHz 长刻度，站不下就整个不画
for (let f10 = f0; f10 <= fEnd; f10++){
if ((f10 % 10) !== 0 || ((f10 / 10) % 2) !== 0) continue;
if (f10 < 880 || f10 > 1080) continue;   // 波段外只画刻度不写数字
const lb = (f10 / 10) + '.0';
const tw = textW(lb, F16R);
const tx = (f10 - left) * DIAL_PX - Math.round(tw / 2);
if (tx < 1 || tx + tw > SCR_W - 1) continue;
text(lb, tx, TUN_Y + 2, C.dialFg, F16R);
}
// 指针最后画，压在刻度与数字「上面」。
// canvas 的 rect(x, w) 覆盖 [x, x+w)，所以 2px 从 DIAL_CX-1 起 → 中心正好 DIAL_CX，与三角尖同线
if (n > 0){
const pc = s.pv >= 0 ? C.sub : C.dialPtr;
rect(DIAL_CX - 1, TUN_Y, 2, TUN_H, pc);  // 只走蓝条高度
// 尖扎进蓝条 2px：三角形最后几行宽度近 0，抗锯齿后会被抹掉，看着像跟竖线断开了
tri(DIAL_CX, TUN_Y + 2, DIAL_CX - 5, TRI_TOP, DIAL_CX + 5, TRI_TOP, pc);
}
}
/* ---------------- 主页 ---------------- */
// 1.15.0：主页这排标签只是「跳到电台页并筛好」的快捷方式，不再切机器的组
function renderHomeChips(){
const box = $('#homeChips');
if (!S.tags || !S.tags.length){ box.innerHTML = ''; return; }
box.innerHTML = S.tags.map(t =>
'<button class="chip" data-t="' + t.i + '">' +
esc(t.n) + '<b>' + t.c + '</b></button>'
).join('') +
'<button class="chip ghost" data-go="stations">' + ic('list-music','ic sm') + '全部电台</button>';
$$('#homeChips .chip').forEach(b => {
b.onclick = () => {
if (b.dataset.go){ S.tag = -1; go('stations'); return; }
S.tag = +b.dataset.t;              // 只设筛选，机器上什么都不变
go('stations');
};
});
}
function updateHome(s){
// 顶栏副标题
const on = !!s.wifi;
// 1.15.0：gname 现在是「当前这台所属的标签」，不是分组名
$('#hdrSub').textContent = s.mode === 'fm'
? '正在收音' + (on ? '' : ' · 没网也能听') + (useCloud() ? ' · 外地控制' : '')
: (on
? (s.gname ? '标签 · ' + s.gname : '已连接') + (useCloud() ? ' · 外地控制' : '')
: '机器没连上 WiFi');
$('#hdrSub').style.color = on ? '' : 'var(--bad)';
// 状态行（有声书把「第几集」拼在台名后面，不用再点进去看）
const ep = epInfo(s);
// 1.41.0：收音模式下「正在放什么」不是网络台名，而是波段 + 频率。
//   原来这里一直写 s.name（网络台），机器在收音时 App 顶部还写着「中国之声」，
//   看起来像 App 没同步。
const fmOn = s.mode === 'fm';
const bandWord = { fm: '调频', am: '中波', lw: '长波', sw: '短波' }[s.band] || '';
// ⚠️ 单位：固件 /api/screen 的 fmfreq 是 **kHz/10**（98.4MHz → 9840），
//   所以换成 MHz 要 /100，不是 /10（原来写 /10，屏上显示成「984.0」）。
const fmTx = fmOn
? (bandWord + ' ' + (s.fmfreq ? (s.fmfreq / 100).toFixed(1) : (s.khz || '')))
: '';
const nm = fmOn ? fmTx : ((s.name || '') + (ep ? ' · 第 ' + (ep.i + 1) + ' 集' : ''));
const dot = $('#dot');
dot.className = 'dot ' + (!s.wifi ? 'bad' : s.ps === 2 ? 'ok' : (s.ps === 1 || s.ps === 3) ? 'warn' : s.ps === 4 ? 'bad' : '');
$('#tipTx').textContent = !s.wifi ? '机器断网了，去设置里看看'
: s.ps === 2 ? nm : psLabel(s) + (nm ? ' · ' + nm : '');
renderEp(s);
$('#lat').textContent = NET.lat ? NET.lat + 'ms' : '';
// 播放键。
//  1.41.0 时收音模式下它是「静音」开关；1.43.0 静音功能删了，芯片又没有「暂停」
//  ⇒ 这个键在收音模式下没有任何语义，直接**藏起来**。
//    留一个点了没反应的键，比没有这个键更糟：老人会以为机器坏了。
const playing = s.ps === 2 || s.ps === 3;
const pi = $('#playIc').querySelector('use');
pi.setAttribute('href', playing ? '#i-pause' : '#i-play');
$('#btnPlay').classList.toggle('paused', !playing);
$('#btnPlay').classList.toggle('hide', fmOn);
// ⚠️ 这里是 querySelector，必须带 # —— 写成 $('btnPrev') 会拿到 null，
//   然后 && 短路，按钮文案永远不改（断言一跑就露）。
const lblPrev = $('#btnPrev') && $('#btnPrev').querySelector('span');
const lblNext = $('#btnNext') && $('#btnNext').querySelector('span');
if (lblPrev) lblPrev.textContent = fmOn ? '上一个台' : '上一台';
if (lblNext) lblNext.textContent = fmOn ? '下一个台' : '下一台';
$('#btnPlay').setAttribute('aria-label', '播放或暂停');
// 音量
// 1.41.0：量程跟固件走。1.39.0 起音量是 3..10 档，滑条原来写死 max=100，
//   拖到中间就被固件夹回 10，数字会突然跳回 —— 看着像「调不动」。
const v = s.vol;
const vs = $('#vol');
setVolRange(vs, s.volmin, s.volmax);
if (document.activeElement !== vs) vs.value = v;
// 填充比例要按量程算，不能直接拿档位当百分比：
// 3..10 档时「5」只占 29%，写 5% 会让滑条看着几乎没有
vs.style.setProperty('--p', pctOf(vs, v) + '%');
$('#volNum').textContent = s.mute ? '静音' : s.vol;
// 1.41.0：首页那个配网按钮。没网时闪琥珀 + 红点 —— 老人也能一眼看出「机器没连上」。
const wb = $('#btnWifiHome');
if (wb) wb.classList.toggle('bad', !s.wifi);
// 1.41.0：控制面板收起时，按钮副标题就是唯一的状态显示
const cs = $('#ctlSub');
if (cs){
const parts = [];
// 台名太长会顶掉后面，先量一下长度再决定要不要带
// 收音模式下 s.name 还是网络台那个名字，写上去会让人以为在听网络台
const nmShort = !fmOn && (s.name || '').length <= 10 ? s.name : '';
if (nmShort) parts.push(nmShort);
parts.push('音量 ' + s.vol);      // 1.43.0：静音功能已删，不再有「静音」这一档
parts.push(fmOn ? fmTx : (playing ? '正在放' : '已暂停'));
cs.textContent = parts.join(' · ');
}
// 1.15.0：没有分组了，不再需要「组变了就重拉」
}
/* ---------------- 有声书集控件 ---------------- */
function renderEp(s){
const bar = $('#epBar');
if (!bar) return;
const ep = epInfo(s);
bar.classList.toggle('hide', !ep);
if (!ep) return;
$('#epNow').textContent = '第 ' + (ep.i + 1) + ' 集';
$('#epOf').textContent = '共 ' + ep.n + ' 集';
const tt = $('#epTitle');
tt.textContent = ep.t || '';
tt.style.display = ep.t ? '' : 'none';
}
// 选集：一本书可能上千集（官场现形记 1167 集），整份 JSON 三十多 KB，
// 跨网那条 MQTT 通道一次送不完 —— 所以机器按**字节预算**切页（EPIS_PAGE_BYTE），
// 这里一页页翻。别再想着「一次拉全」：那会把云那边的 16KB 缓冲撑爆。
const _EP = { from: 0, n: 0, tot: 0, cur: -1, prog: '', sid: -1 };
function epRow(o, cur){
const t = o.t || ('第 ' + (o.i + 1) + ' 集');
return '<div class="row' + (o.i === cur ? ' cur' : '') + '" data-e="' + o.i + '">' +
'<span class="ix' + (o.i === cur ? ' on' : '') + '">' + (o.i + 1) + '</span>' +
'<div class="nm"><div class="t">' + esc(t) + '</div>' +
'<div class="m"><span>第 ' + (o.i + 1) + ' 集</span>' +
(o.i === cur ? '<span>上次听到这儿</span>' : '') + '</div></div>' +
'</div>';
}
// 取一页：成功返回这一页的 list（同时写进 _EP），不是书返回 null
// 1.46.33：带上台号 —— 弹层可以给「不是正在播」的那一台打开，
//   不用先让机器切过去（那样一打开选集，正在听的就断了）。
async function epPage(from, sid){
const body = { from: from | 0 };
if (sid != null && sid >= 0) body.id = sid;
const d = await post('/api/epis', body);
const list = (d && d.list) || [];
if (!d || !d.n || !list.length) return null;
_EP.tot = d.n | 0;
_EP.cur = (typeof d.cur === 'number') ? d.cur : -1;
_EP.prog = d.prog || '';
_EP.from = (typeof d.from === 'number') ? d.from : (from | 0);
_EP.n = list.length;
_EP.sid = (sid != null && sid >= 0) ? sid : -1;
return list;
}
function epRangeText(){
return '第 ' + (_EP.from + 1) + '–' + (_EP.from + _EP.n) + ' 集 · 共 ' + _EP.tot + ' 集';
}
async function openEpisodes(stIdx){
const sid = (typeof stIdx === 'number' && stIdx >= 0) ? stIdx : -1;
let list = null;
try { list = await epPage(0, sid); } catch (e) {}
if (!list){ toast('这一台不是有声书', true); return; }
// 上次听到的那集不在第一页里 → 直接翻到它那一页，不然打开弹层看不见「上次听到这儿」
if (_EP.cur >= _EP.n){
try { const l2 = await epPage(Math.max(0, _EP.cur - 2), sid); if (l2) list = l2; } catch (e) {}
}
openSheet(
'<h3>' + esc(_EP.prog || '选集') + '</h3>' +
'<p class="desc">共 ' + _EP.tot + ' 集。点某一集就换到那一集，这个弹层不会关；播完会自动接下一集。</p>' +
'<div class="eppage">' +
'<button class="btn ghost sm" id="epgPrev">上一段</button>' +
'<span id="epRange"></span>' +
'<button class="btn ghost sm" id="epgNext">下一段</button>' +
'</div>' +
'<div class="list" id="epList"></div>',
sheet => {
const box = $('#epList', sheet);
const prev = $('#epgPrev', sheet), next = $('#epgNext', sheet);
const paint = () => {
box.innerHTML = list.map(o => epRow(o, _EP.cur)).join('');
$('#epRange', sheet).textContent = epRangeText();
prev.disabled = _EP.from <= 0;
next.disabled = _EP.from + _EP.n >= _EP.tot;
const c = $('.row.cur', box);
if (c) setTimeout(() => c.scrollIntoView({ block: 'center' }), 110);
};
const go = async from => {
if (from < 0) from = 0;
try {
const l = await epPage(from, sid);
if (l){ list = l; paint(); }
else toast('翻到头了', true);
} catch (e) { toast('没取到，再点一次', true); }
};
paint();
prev.onclick = () => { if (_EP.from > 0) go(Math.max(0, _EP.from - _EP.n)); };
next.onclick = () => go(_EP.from + _EP.n);
// 行是随时换掉的（翻页会整体重画），所以点事件挂在容器上，不在每行上
// 1.46.33：点集数**不关弹层**（用户要「留在选集页面」），
//   也不做任何事直到真的点了某一集 —— 打开弹层/翻页都不碰播放。
//   点的就是当前这一集时直接忽略：没意义，还白搭一次断流重连。
box.onclick = async e => {
const r = e.target.closest ? e.target.closest('.row[data-e]') : null;
if (!r) return;
const en = +r.dataset.e;
if (en === _EP.cur){ toast('正在播这一集'); return; }
const body = { e: en };
if (sid >= 0) body.id = sid;
try {
await postOk('/api/epis', body);
_EP.cur = en;
paint();                       // 高亮跟着走，页面留着
toast('从第 ' + (en + 1) + ' 集开始');
pollScreen();
} catch (err) { toast('没切成', true); }
};
}
);
}
/* ---------------- 今日收听小卡 ---------------- */
async function refreshToday(){
try{
const h = await get('/api/history');
S.hist = h;
$('#todayNum').innerHTML = fmtShort(h.today || 0);
$('#todayPlays').textContent = h.plays ? '开过 ' + h.plays + ' 次' : '';
const last = (h.list || [])[0];
$('#lastOne').textContent = last
? ('上次听：' + last.n + ' · ' + (last.g || '未打标签') + ' · ' + fmtRel(last.last))
: '还没有记录，听一会儿就有了';
}catch (e){}
}
/* ---------------- 操作 ---------------- */
let _actLock = 0;
async function act(path, body, msg){
const now = Date.now();
if (now - _actLock < 220 && !body) return;   // 防连点
_actLock = now;
try{
await post(path, body);
if (msg) toast(msg);
await pollScreen();
}catch (e){
toast('没成功，检查一下连接', true);
}
}
async function doVol(v){
const vs = $('#vol');
// 1.44.0：夹在滑条自己的量程里。原来写死 0..100 —— 新刻度下按 +5 两下就能
//   把值顶到 30，固件那边只会夹回来，App 与机器上的数字就对不上了。
const mn = +vs.min, mx = +vs.max;
v = Math.max(mn, Math.min(mx, Math.round(v)));
vs.value = v; vs.style.setProperty('--p', pctOf(vs, v) + '%');
$('#volNum').textContent = v;
try { await post('/api/vol', { v }); } catch (e) {}
}
let _volT = 0;
function volSoon(v){
clearTimeout(_volT);
_volT = setTimeout(() => doVol(v), 140);
}
/* ---------------- 离线态 ---------------- */
// 连不上收音机时不拦路：顶上挂条子，告诉用户点哪儿能修
function setOffline(on){
S.offline = !!on;
const bar = $('#offBar');
if (bar) bar.hidden = !S.offline;
document.body.classList.toggle('off', S.offline);
if (S.offline){
const tx = $('#offTx');
// 1.46.33：把"连不上"拆成两句人话 ——
//   收音机自己没网（它连不上 WiFi）跟"手机够不到它"是两件事，
//   以前一律写「没连上收音机」，用户不知道是机器的事还是手机的事。
const noWifi = !!(S.scr && S.scr.wifi === 0);
if (tx) tx.textContent = window.MR_REMOTE
? (CLOUD.S.conn && !CLOUD.S.dev ? '先选一台机器（设置 → 我的机器）'
: '没连上收音机（远程）')
: (noWifi ? '收音机没连上 WiFi' : '手机连不上收音机');
const sub = $('#hdrSub');
if (sub && S.page === 'home') sub.textContent = '点这条 → 换 WiFi / 重新连接';
}
}
function focusWifiCard(){
const card = $('#wifiCard');
if (!card) return;
card.classList.add('hot');
setTimeout(() => card.classList.remove('hot'), 1600);
card.scrollIntoView({ block: 'center', behavior: 'smooth' });
}
/* ---------------- 导航 ---------------- */
function go(page){
if (page === S.page) return;
closeSheet(true);
S.page = page;
$$('.page').forEach(p => p.classList.toggle('on', p.dataset.page === page));
$$('.tab').forEach(t => t.classList.toggle('on', t.dataset.tab === page));
window.scrollTo({ top: 0, behavior: 'auto' });
if (page === 'stations') loadStations();
if (page === 'history') loadHistory();
if (page === 'diag') loadDiag();
if (page === 'settings') loadSettings();
}
/* ---------------- 轮询 ---------------- */
let _pollT = 0, _polling = false;
async function pollScreen(){
if (_polling) return;
_polling = true;
const t0 = performance.now();
try{
const s = await get('/api/screen');
NET.lat = Math.round(performance.now() - t0);
NET.fails = 0;
if (S.offline) setOffline(false);      // 恢复了就把条子摘掉
S.scr = s;
drawScreen(s);
$('#liveTx').classList.remove('lost');
$('#liveTx').textContent = '实时';
if (S.page === 'home') updateHome(s);
}catch (e){
NET.fails++;
if (NET.fails >= 2){
$('#liveTx').classList.add('lost');
$('#liveTx').textContent = '断开';
}
if (NET.fails >= 3 && !S.offline) setOffline(true);   // 连续失败 3 次才判离线，避免闪
}finally{
_polling = false;
}
}
// 1.46.51：电量单独轮询 —— 用户报「App 的电量和收音机不同步」。
//   原因：电量只写在设置页，而设置页只在**进来那一刻**读一次 /api/state，
//   之后一直挂着不动（机器从 96% 掉到 90%，App 还写着 96%）。
//   这里单独 20 秒问一次 /api/batt（比 /api/state 轻），只在设置页可见时问。
let _battT = 0;
function startBattPoll(){
if (_battT) clearInterval(_battT);
_battT = setInterval(async () => {
if (document.hidden) return;
if (S.page !== 'settings') return;
const row = $('#battRow');
if (!row || row.style.display === 'none') return;   // 没接电量检测就别去问
try{ renderBatt(await get('/api/batt')); }catch (e){}
}, 20000);
}
function stopBattPoll(){ if (_battT) clearInterval(_battT); _battT = 0; }
function startPoll(){
stopPoll();
_pollT = setInterval(() => {
if (document.hidden) return;
pollScreen();
}, useCloud() ? 1600 : 900);   // 跨网走中转，1.6 秒一次够用也省流量
startBattPoll();
}
function stopPoll(){ if (_pollT) clearInterval(_pollT); _pollT = 0; }
/* ---------------- 启动 ---------------- */
async function bootApp(){
fitCanvas();
window.addEventListener('resize', () => { fitCanvas(); if (S.scr) drawScreen(S.scr); });
CLOUD.load();                       // 1.8.0：跨网用的遥控码与服务器
let online = false;
if (window.MR_REMOTE){
// 远程版：页面根本不在收音机那个网里，没有局域网可试，直接连云端。
// 1.19.0：没配对也要连 —— 先连上才能听到设备广播、才知道家里有哪几台机器。
CLOUD.enable(true);
online = await new Promise(res => {
if (CLOUD.active()) return res(true);
const t0 = Date.now();
const iv = setInterval(() => {
if (CLOUD.active()){ clearInterval(iv); res(true); }
else if (Date.now() - t0 > 7000){ clearInterval(iv); res(false); }
}, 200);
});
if (!online) S.offline = true;
} else {
try{
// 1.38.0：先把「这台机器在哪」确定下来，再认证。
//   原来是直接 fetch('/api/auth') 赌同源 —— 收音机换到路由器后面就必然失败，
//   用户只能自己去设置页手输 IP，也就是他说的「额外链接」。
if (!NET.base){
const j = await findRadio();
if (j) console.log('[MyRadio] 已找到机器 ' + (j.devid || '') + ' @ ' + (NET.base || '同源'));
}
await authOnce();
online = true;
}catch (e){
if (e.locked){ pairGate(); return; }
// 连不上也放行：先进界面，再去设置页配网。不再用遮罩把人挡在门外
S.offline = true;
}
}
if (online){
try{
await pollScreen();
await refreshStations();
}catch (e){ S.offline = true; }
}
$('#boot').classList.add('gone');
// 过渡结束后彻底摘掉，免得某些浏览器/截图工具停在半透明状态压着界面
setTimeout(() => { const b = $('#boot'); if (b && b.classList.contains('gone')) b.style.display = 'none'; }, 420);
INIT.forEach(f => { try { f(); } catch (e) {} });
startPoll();
refreshToday();
setInterval(refreshToday, 300000);   // 5 分钟刷一次"今天听了多久"
if (S.offline){
setOffline(true);                  // 顶上挂条子
go('settings');                    // 直接落到设置页，那儿能配网
setTimeout(focusWifiCard, 120);
} else {
// 方便直接打开某一页：?page=stations / history / diag / settings
const want = new URLSearchParams(location.search).get('page');
if (want && $('.page[data-page="' + want + '"]')) setTimeout(() => go(want), 60);
}
document.addEventListener('visibilitychange', () => {
if (!document.hidden){ pollScreen(); if (S.page === 'diag') loadDiag(); }
});
}
/* 事件绑定（不依赖接口的部分） */
function bindCore(){
// 1.38.2：电量行/校准按钮
const bRow = $('#battRow');
if (bRow) bRow.onclick = () => openBattCal();
const bCal = $('#btnBattCal');
if (bCal) bCal.onclick = () => openBattCal();
$$('.tab').forEach(t => t.onclick = () => go(t.dataset.tab));
// 离线条上的「去配网」：切到设置页并把配网卡顶到眼前
const offGo = $('#offGo');
if (offGo) offGo.onclick = () => { go('settings'); setTimeout(focusWifiCard, 80); };
$('#btnPrev').onclick = () => act('/api/step', { dir: -1 });
$('#btnNext').onclick = () => act('/api/step', { dir: 1 });
// 有声书：上一集 / 下一集 / 选集（循环，两端都能点）
const ePrev = $('#epPrev'), eNext = $('#epNext'), eOpen = $('#epOpen');
if (ePrev) ePrev.onclick = () => act('/api/epis', { dir: -1 });
if (eNext) eNext.onclick = () => act('/api/epis', { dir: 1 });
if (eOpen) eOpen.onclick = openEpisodes;
$('#btnPlay').onclick = () => act('/api/pause');
// 1.44.0：兜底值改成网络那一路的默认（原厂 netvolume 18），不再是 45
$('#btnVolDown').onclick = () => doVol((S.scr ? S.scr.vol : 18) - 5);
$('#btnVolUp').onclick = () => doVol((S.scr ? S.scr.vol : 18) + 5);
$('#btnHist').onclick = () => go('history');
// 1.41.0：首页右上角的 WiFi 按钮 —— 直接弹配网面板，不用先切到设置页
const wbh = $('#btnWifiHome');
if (wbh) wbh.onclick = () => openWifiSheet();
// 1.41.0：调台/暂停/音量收在「控制收音机」按钮里，点一下才展开。
//   展开状态存 localStorage —— 子女用顺手了下次进来还是开着的；
//   老人机上从来不点，就一直是收起的干净首屏。
bindCtl();
const vs = $('#vol');
vs.addEventListener('input', () => {
const v = +vs.value;
vs.style.setProperty('--p', pctOf(vs, v) + '%');
$('#volNum').textContent = v;
volSoon(v);
});
}
/* ---------------- 1.41.0 音量量程 ---------------- */
// 一个滑块里某个值占多少百分比（给 CSS 的 --p / --fill 用）
function pctOf(el, v){
const a = +el.min, b = +el.max;
if (!(b > a)) return 0;
return Math.max(0, Math.min(100, Math.round((v - a) / (b - a) * 100)));
}
// 固件 /api/state 会带 volmin / volmax（网络那一路）/ fvolmin / fvolmax（收音那一路）。
// 1.44.0 起两路量程不同（6..21 / 6..63），调用方必须传对应的那一对。
// 老固件没有这些字段时兜底成网络那一路的 6..21。
// 只在值真的变了才写 DOM —— 每 0.9 秒一次的轮询里改 attr 会让滑条抖。
function setVolRange(el, mn, mx){
if (!el) return;
const a = mn != null ? +mn : 6, b = mx != null ? +mx : 21;
if (+el.min !== a) el.min = a;
if (+el.max !== b) el.max = b;
return { mn: a, mx: b };
}
/* ---------------- 1.41.0 控制面板 ---------------- */
function ctlOpen(on){
const p = $('#ctlPanel'), b = $('#btnCtl');
if (!p || !b) return;
p.classList.toggle('hide', !on);
b.setAttribute('aria-expanded', on ? 'true' : 'false');
try{ localStorage.setItem('mr.ctl', on ? '1' : '0'); }catch (e){}
}
function bindCtl(){
const b = $('#btnCtl');
if (!b) return;
b.onclick = () => ctlOpen($('#ctlPanel').classList.contains('hide'));
let on = false;
try{ on = localStorage.getItem('mr.ctl') === '1'; }catch (e){}
ctlOpen(on);
}
document.addEventListener('DOMContentLoaded', () => {
bindCore();
bootApp();
});
;
/* ==== stations.js ==== */
/* ============================================================
stations.js —— 电台页：列表、搜索、拖拽排序、增删改、分类管理
============================================================ */
'use strict';
let _qLock = 0;
async function loadStations(){
try{
const r = await get('/api/stations');
S.list = r.list || [];
// 1.15.0：机器不再分组，/api/stations 返回**整张表** + 标签清单。
// S.tag = 当前筛的标签下标（-1 = 全部），只在本机生效，不再叫机器切组。
S.tags = r.tags || [];
if (S.tag == null) S.tag = -1;
if (S.tag >= S.tags.length) S.tag = -1;
renderStChips();
renderStList();
if (S.page === 'home') renderHomeChips();
}catch (e){
$('#stList').innerHTML = '<div class="empty">' + ic('cloud-off') + '读不到台表<br>检查一下连接</div>';
}
}
// core 里也调这个名字（页面切换后重拉）
async function refreshStations(){ await loadStations(); }
// 顶部那排标签：全部 + 有问题 + 每个标签。点一下**只在本机筛选**，机器上什么都不变。
// 1.17.0：「有问题」是固定的一项（不是标签，机器上没有这个标签），
//         只筛出「连不上 / 没声音」的台 —— 数据来自 /api/health。
function renderStChips(){
const box = $('#stChips');
const bad = hlCountBad();
box.innerHTML =
'<button class="chip' + (S.tag === -1 ? ' on' : '') + '" data-t="-1">全部<b>' + S.list.length + '</b></button>' +
'<button class="chip warn' + (S.tag === -2 ? ' on' : '') + (bad ? '' : ' zero') + '" data-t="-2" id="chipBad">' +
ic('activity', 'ic sm') + '有问题<b id="chipBadCnt">' + bad + '</b></button>' +
S.tags.map(t =>
'<button class="chip' + (t.i === S.tag ? ' on' : '') + '" data-t="' + t.i + '">' +
esc(t.n) + '<b>' + t.c + '</b></button>'
).join('') +
'<button class="chip ghost" id="chipMgr">' + ic('folder-plus', 'ic sm') + '标签</button>';
$$('#stChips .chip').forEach(b => {
b.onclick = () => {
if (b.id === 'chipMgr'){ openGroupMgr(); return; }
const t = +b.dataset.t;
if (t === S.tag) return;
S.tag = t;
renderStChips();
renderStList();
};
});
}
function filtered(){
const q = S.q.trim().toLowerCase();
let arr = S.list.map((s, i) => ({ s, i }));
if (S.tag === -2) arr = arr.filter(o => { const st = hlNow(o.i); return st === 2 || st === 3; });
else if (S.tag >= 0) arr = arr.filter(o => (o.s.t | 0) === S.tag);
if (!q) return arr;
return arr.filter(o =>
(o.s.n || '').toLowerCase().indexOf(q) >= 0 ||
(o.s.u || '').toLowerCase().indexOf(q) >= 0 ||
String(o.i + 1) === q
);
}
function renderStList(){
const box = $('#stList');
const rows = filtered();
const cur = S.scr ? S.scr.idx : -1;
const playing = S.scr && (S.scr.ps === 2 || S.scr.ps === 3);
const scope = S.tag === -2 ? '有问题的台 ' + rows.length + ' 个'
: S.tag < 0 ? '共 ' + S.list.length + ' 个台'
: '「' + ((S.tags[S.tag] || {}).n || '') + '」下 ' + rows.length + ' 个台';
$('#stHint').textContent = S.q ? '搜到 ' + rows.length + ' 个台'
: scope + ' · 按住左侧把手可拖动排序';
if (!rows.length){
box.innerHTML = '<div class="empty">' + ic(S.q ? 'search' : S.tag === -2 ? 'circle-check' : 'list-music') +
(S.q ? '没搜到这个台'
: S.tag === -2 ? '没查出有问题的台<br>都挺正常'
: '这个标签下还没有台<br>点右上角 ＋ 加一个') + '</div>';
return;
}
box.innerHTML = rows.map((o, k) => {
const s = o.s;
const isCur = o.i === cur;
return '<div class="row' + (isCur ? ' cur' : '') + '" data-i="' + o.i + '" style="--i:' + Math.min(k, 12) + '">' +
'<button class="grip" data-grip="' + o.i + '" aria-label="拖动排序">' + ic('grip-vertical', 'ic sm') + '</button>' +
'<span class="ix' + (isCur ? ' on' : '') + '">' + (o.i + 1) + '</span>' +
'<div class="nm">' +
'<div class="t">' + esc(s.n) + '</div>' +
'<div class="m">' +
(s.ep ? '<span class="tag">共 ' + s.ep + ' 集</span>' : '') +
(s.q ? '<span class="tag' + (s.ep ? ' gray' : '') + '">蜻蜓 ' + esc(s.u) + '</span>'
: '<span class="tag gray">流地址</span>') +
(isCur ? '<span>' + (s.ep && S.scr && S.scr.epn ? '正在播第 ' + ((S.scr.epi | 0) + 1) + ' 集' : '正在播') + '</span>' : '') +
'</div>' +
'</div>' +
'<button class="more" data-more="' + o.i + '" aria-label="更多">' +
ic('ellipsis', 'ic sm') + '</button>' +
'</div>';
}).join('');
syncRowHealth();       // 1.17.0：把「连不上 / 没声音」标到行上
$$('#stList .row').forEach(r => {
const i = +r.dataset.i;
r.querySelector('.nm').onclick = () => playStation(i, r);
r.querySelector('.more').onclick = (e) => { e.stopPropagation(); openStationMenu(i); };
const grip = r.querySelector('.grip');
grip.addEventListener('pointerdown', e => startDrag(e, i, r));
});
}
async function playStation(i, row){
row && row.classList.add('cur');
try{
await post('/api/play', { id: i });
const nm = (S.list[i] && S.list[i].n) || '';
toast('切到 ' + nm);
pollScreen();
}catch (e){ toast('切台失败', true); }
}
/* ---------------- 拖拽排序 ---------------- */
let drag = null;
function startDrag(e, i, row){
if (drag) return;
drag = { i, row, y0: e.clientY, dy: 0, h: row.offsetHeight, to: i };
row.classList.add('dragging');
row.style.animation = 'none';
document.addEventListener('pointermove', onDragMove, { passive: false });
document.addEventListener('pointerup', endDrag);
document.addEventListener('pointercancel', endDrag);
e.preventDefault();
}
function onDragMove(e){
if (!drag) return;
e.preventDefault();
drag.dy = e.clientY - drag.y0;
drag.row.style.transform = 'translateY(' + drag.dy + 'px)';
const n = $$('#stList .row').length;
let to = Math.round(drag.i + drag.dy / drag.h);
to = Math.max(0, Math.min(n - 1, to));
if (to !== drag.to){
drag.to = to;
$$('#stList .row').forEach((r, k) => {
r.classList.toggle('drop-before', k === to && to < drag.i);
r.classList.toggle('drop-after',  k === to && to > drag.i);
});
}
}
async function endDrag(){
if (!drag) return;
const d = drag;
drag = null;
document.removeEventListener('pointermove', onDragMove);
document.removeEventListener('pointerup', endDrag);
document.removeEventListener('pointercancel', endDrag);
d.row.style.transform = '';
d.row.classList.remove('dragging');
$$('#stList .row').forEach(r => r.classList.remove('drop-before', 'drop-after'));
if (d.to === d.i) return;
try{
await postOk('/api/move', { id: d.i, to: d.to });
toast('已移到第 ' + (d.to + 1) + ' 位');
await loadStations();
pollScreen();
}catch (e){ toast('排序没成功', true); }
}
/* ---------------- 单台菜单 ---------------- */
function openStationMenu(i){
const s = S.list[i];
if (!s) return;
openSheet(
'<h3>' + esc(s.n) + '</h3>' +
'<p class="desc">第 ' + (i + 1) + ' 个台 · ' +
(s.ep ? '有声书，共 ' + s.ep + ' 集（播完自动接下一集）' : (s.q ? '蜻蜓台号 ' + esc(s.u) : '自定义流地址')) + '</p>' +
'<div class="optlist">' +
'<button class="opt" data-a="play">' + ic('play') + '立刻播放</button>' +
(s.ep ? '<button class="opt" data-a="ep">' + ic('list-music') + '选集（共 ' + s.ep + ' 集）</button>' : '') +
(s.ep ? '<button class="opt" data-a="next">' + ic('list-music') + '下次从第几集开始</button>' : '') +
'<button class="opt" data-a="rename">' + ic('pencil') + '改名字 / 改地址</button>' +
'<button class="opt" data-a="move">' + ic('move-right') + '换到别的分类</button>' +
'<button class="opt" data-a="up">' + ic('arrow-up') + '往前放一位</button>' +
'<button class="opt" data-a="down">' + ic('arrow-down') + '往后放一位</button>' +
'<button class="opt" data-a="top">' + ic('sparkles') + '放到最前面</button>' +
'<button class="opt danger" data-a="del">' + ic('trash-2') + '删除这个台</button>' +
'</div>',
sheet => {
$$('.opt', sheet).forEach(b => b.onclick = () => {
const a = b.dataset.a;
closeSheet();
if (a === 'play') post('/api/play', { id: i }).then(pollScreen);
if (a === 'ep'){
// 1.46.33：**不要**再先播这一台了。
//   以前这里 post('/api/play') 再开弹层 —— 用户点「选集」，
//   机器先把正在听的节目切掉/重连，听起来就是「卡一下、停了」。
//   选集弹层现在能按台号读集表（/api/epis?id=），不需要先切台。
openEpisodes(i);
}
if (a === 'next') openEpNext(i);
if (a === 'rename') openEditStation(i);
if (a === 'move') openMoveGroup(i);
if (a === 'up') moveTo(i, i - 1);
if (a === 'down') moveTo(i, i + 1);
if (a === 'top') moveTo(i, 0);
if (a === 'del') askDelete(i);
});
}
);
}
async function moveTo(i, to){
if (to < 0 || to >= S.list.length) return;
try{
await postOk('/api/move', { id: i, to });
await loadStations();
toast('已挪到第 ' + (to + 1) + ' 位');
}catch (e){ toast('没挪成', true); }
}
function askDelete(i){
const s = S.list[i];
confirmSheet({
title: '删掉「' + (s ? s.n : '') + '」？',
desc: '删了就得重新添加。要是只想换个位置，用「往前放一位」。',
ok: '删掉', danger: true,
onOk: async () => {
try{
await postOk('/api/del', { id: i });
toast('已删除');
await loadStations();
pollScreen();
}catch (e){ toast('删除失败' + (e.code ? '（' + e.code + '）' : ''), true); }
}
});
}
/* ---------------- 1.46.33：下次从第几集开始 ----------------
用户要的是「先设好，等真的调到这个台才从那一集开始」——
所以这个弹层只写一个标记：机器不跳台、不起播、也不动他听到的进度。
设好以后，第一次调到这个台时用它一次，然后就作废（回到"接着上次听"）。 */
function openEpNext(i){
const s = S.list[i] || {};
const scr = (S.scr && S.scr.idx === i) ? S.scr : null;
const tot = (scr && scr.epn) || s.ep || 0;
const cur = (scr && scr.epi >= 0) ? scr.epi : 0;
const has = (scr && typeof scr.epnext === 'number' && scr.epnext >= 0) ? scr.epnext : -1;
const def = (has >= 0 ? has : cur) + 1;
openSheet(
'<h3>下次从第几集开始</h3>' +
'<p class="desc">给「' + esc(s.n) + '」定个起点。设好之后<b>机器不动</b>；' +
'等你下次把它调到这个台，才会从这一集开始播，播过一次就恢复"接着上次听"。</p>' +
'<div class="field"><label>从第几集开始（共 ' + Math.max(1, tot) + ' 集）</label>' +
'<input type="number" id="enVal" inputmode="numeric" min="1" max="' + Math.max(1, tot) + '" ' +
'value="' + def + '" style="width:100%;height:52px;padding:0 16px;border-radius:14px;' +
'border:1px solid var(--line);background:var(--card);color:var(--ink);font-size:17px"></div>' +
(has >= 0
? '<p class="desc" style="font-size:12.5px">现在已经设着：下次从第 ' + (has + 1) + ' 集起</p>' +
'<button class="btn ghost" id="enClr" style="width:100%;margin-bottom:8px">取消这个设置</button>'
: '') +
'<div class="acts"><button class="btn ghost" id="enNo">取消</button>' +
'<button class="btn pri" id="enYes">设好</button></div>',
sheet => {
$('#enNo', sheet).onclick = closeSheet;
const clr = $('#enClr', sheet);
if (clr) clr.onclick = async () => {
try{
await postOk('/api/epnext', { id: i, e: -1 });
closeSheet(); toast('已取消'); pollScreen();
}catch (e){ toast('没取消成', true); }
};
$('#enYes', sheet).onclick = async () => {
const v = Math.round(+$('#enVal', sheet).value);
if (!(v >= 1 && v <= tot)){ toast('填 1 到 ' + tot + ' 之间的数', true); return; }
try{
await postOk('/api/epnext', { id: i, e: v - 1 });   // 固件是 0 基
closeSheet();
toast('设好了：下次调到这个台，从第 ' + v + ' 集开始');
pollScreen();
}catch (e){ toast('没设上', true); }
};
}
);
}
/* ---------------- 改 / 加 ---------------- */
function openEditStation(i){
const s = S.list[i] || { n: '', u: '' };
openSheet(
'<h3>改这个台</h3>' +
'<p class="desc">名字随便起，老人看得懂就行。地址可以是蜻蜓台号（纯数字），也可以是 http 开头的流地址。</p>' +
'<div class="field"><label>名字</label><input type="text" id="eName" value="' + esc(s.n) + '" maxlength="38"></div>' +
'<div class="field"><label>台号或流地址</label><input type="text" id="eUrl" value="' + esc(s.u) + '" maxlength="190"></div>' +
'<div class="acts"><button class="btn ghost" id="eNo">取消</button>' +
'<button class="btn pri" id="eYes">保存</button></div>',
sheet => {
$('#eNo', sheet).onclick = closeSheet;
$('#eYes', sheet).onclick = async () => {
const n = $('#eName', sheet).value.trim();
const u = $('#eUrl', sheet).value.trim();
if (!/^\d{1,9}$/.test(u) && !/^https?:\/\//i.test(u)){ toast('地址要填台号或 http 开头的地址', true); return; }
try{
await postOk('/api/edit', { id: i, name: n, url: u });
closeSheet(); toast('已保存');
await loadStations(); pollScreen();
}catch (e){ toast('保存失败' + (e.code ? '（' + e.code + '）' : ''), true); }
};
}
);
}
function groupOptions(sel){
return S.tags.map(t =>
'<option value="' + t.i + '"' + (t.i === sel ? ' selected' : '') + '>' +
esc(t.n) + '（' + t.c + ' 台）</option>'
).join('');
}
function openAddStation(){
if (!S.tags.length){ toast('先等台表读完', true); return; }
openSheet(
'<h3>添加电台</h3>' +
'<p class="desc">填蜻蜓台号最省事：在蜻蜓 FM 里找到台，看网址最后的数字，比如 339 是北京新闻广播。<br>' +
'如果要加某个单集，直接把蜻蜓的节目页地址整条粘进来也行（www.qtfm.cn/channels/…/programs/…）。</p>' +
'<div class="field"><label>名字</label><input type="text" id="aName" placeholder="例如：中国之声" maxlength="38"></div>' +
'<div class="field"><label>台号或流地址</label><input type="text" id="aUrl" inputmode="numeric" placeholder="339 或 https://…" maxlength="190"></div>' +
'<div class="field"><label>打哪个标签</label>' +
'<select id="aGrp" style="width:100%;height:50px;padding:0 14px;border-radius:14px;border:1px solid var(--line);background:var(--card);color:var(--ink)">' +
groupOptions(S.tag >= 0 ? S.tag : 0) + '</select></div>' +
'<div class="acts"><button class="btn ghost" id="aNo">取消</button>' +
'<button class="btn pri" id="aYes">添加</button></div>',
sheet => {
$('#aNo', sheet).onclick = closeSheet;
$('#aYes', sheet).onclick = async () => {
const n = $('#aName', sheet).value.trim();
const u = $('#aUrl', sheet).value.trim();
const g = +$('#aGrp', sheet).value;
if (!/^\d{1,9}$/.test(u) && !/^https?:\/\//i.test(u)){ toast('地址要填台号或 http 开头的地址', true); return; }
try{
await postOk('/api/add', { name: n || '未命名', url: u, g });
closeSheet(); toast('已添加');
await loadStations(); pollScreen();
}catch (e){ toast('没加上' + (e.code === 'full' ? '：台表满了（500 台上限）' : '：地址可能不对'), true); }
};
}
);
}
function openMoveGroup(i){
const s = S.list[i] || {};
openSheet(
'<h3>给「' + esc(s.n) + '」换个标签</h3>' +
'<p class="desc">换标签只改分类标记，<b>不会改变它在旋钮上的位置</b>。位置要动就回列表拖拽。</p>' +
'<div class="optlist">' +
S.tags.map(t => '<button class="opt" data-g="' + t.i + '">' + ic('folder') +
esc(t.n) + '<span class="r">' + t.c + ' 台</span></button>').join('') +
'<button class="opt" data-g="-1">' + ic('plus') + '新建一个标签</button>' +
'</div>',
sheet => {
$$('.opt', sheet).forEach(b => b.onclick = async () => {
const g = +b.dataset.g;
closeSheet();
if (g === -1){ askNewGroup(name => doMoveGroup(i, -1, name)); return; }
doMoveGroup(i, g, '');
});
}
);
}
async function doMoveGroup(i, g, name){
try{
await postOk('/api/mvgroup', g >= 0 ? { id: i, g } : { id: i, gn: name });
toast('已换标签');
await loadStations(); pollScreen();
}catch (e){ toast('没换成', true); }
}
/* ---------------- 标签管理 ---------------- */
function openGroupMgr(){
const body = S.tags.map((t, k) =>
'<div class="row" data-g="' + t.i + '" style="min-height:54px">' +
'<div class="nm"><div class="t">' + esc(t.n) + '</div>' +
'<div class="m">' + t.c + ' 个台</div></div>' +
'<button class="iconbtn" data-op="up" style="width:34px;height:34px;border-radius:10px;box-shadow:none"' +
(k === 0 ? ' disabled' : '') + '>' + ic('arrow-up', 'ic sm') + '</button>' +
'<button class="iconbtn" data-op="down" style="width:34px;height:34px;border-radius:10px;box-shadow:none"' +
(k === S.tags.length - 1 ? ' disabled' : '') + '>' + ic('arrow-down', 'ic sm') + '</button>' +
'<button class="iconbtn" data-op="ren" style="width:34px;height:34px;border-radius:10px;box-shadow:none">' +
ic('pencil', 'ic sm') + '</button>' +
'<button class="iconbtn" data-op="del" style="width:34px;height:34px;border-radius:10px;box-shadow:none"' +
(k === 0 ? ' disabled title="第一个标签是兜底标签，删不掉"' : '') + '>' +
ic('trash-2', 'ic sm') + '</button>' +
'</div>'
).join('');
openSheet(
'<h3>标签</h3>' +
'<p class="desc">标签只用来在 App 里筛选和整理，<b>不影响机器上的播放顺序</b>（机器上就是一整串台，转旋钮从头到尾）。</p>' +
'<div class="list" style="box-shadow:none">' + body + '</div>' +
'<button class="btn ghost" id="gNew" style="width:100%">' + ic('folder-plus', 'ic sm') + '新建一个标签</button>',
sheet => {
$('#gNew', sheet).onclick = () => askNewGroup(name => doGroupOp('add', 0, name));
$$('.row[data-g]', sheet).forEach(row => {
const g = +row.dataset.g;
$$('[data-op]', row).forEach(b => b.onclick = async () => {
const op = b.dataset.op;
if (op === 'up' || op === 'down'){
const dir = op === 'up' ? -1 : 1;
try{ await postOk('/api/groupop', { op: 'move', g, dir }); await loadStations(); openGroupMgr(); }
catch (e){ toast('没挪成', true); }
}
if (op === 'ren'){
const cur = S.tags.find(x => x.i === g);
askInput('改标签名字', '名字', cur ? cur.n : '', name => doGroupOp('rename', g, name));
}
if (op === 'del'){
const cur = S.tags.find(x => x.i === g);
confirmSheet({
title: '删掉标签「' + (cur ? cur.n : '') + '」？',
desc: '里面 ' + (cur ? cur.c : 0) + ' 个台<b>不会删</b>，会退回到「' + ((S.tags[0] || {}).n || '广播') + '」。',
ok: '删掉', danger: true,
onOk: async () => {
try{ await postOk('/api/groupop', { op: 'del', g }); await loadStations(); toast('已删除'); }
catch (e){ toast('删除失败', true); }
}
});
}
});
});
}
);
}
async function doGroupOp(op, g, name){
try{
await postOk('/api/groupop', { op, g, n: name });
toast(op === 'add' ? '已新建标签' : op === 'rename' ? '已改名' : '已处理');
await loadStations();
}catch (e){ toast('没成功，名字可能重复了', true); }
}
function askNewGroup(cb){
askInput('新建标签', '比如：常听、本地、评书', '', cb);
}
function askInput(title, ph, val, cb){
openSheet(
'<h3>' + esc(title) + '</h3>' +
'<div class="field" style="margin-top:10px"><label>名字</label>' +
'<input type="text" id="ipVal" placeholder="' + esc(ph) + '" value="' + esc(val) + '" maxlength="12"></div>' +
'<div class="acts"><button class="btn ghost" id="ipNo">取消</button>' +
'<button class="btn pri" id="ipYes">好了</button></div>',
sheet => {
const inp = $('#ipVal', sheet);
setTimeout(() => inp.focus(), 120);
$('#ipNo', sheet).onclick = closeSheet;
$('#ipYes', sheet).onclick = () => {
const v = inp.value.trim();
if (!v){ toast('给个名字', true); return; }
closeSheet();
cb(v);
};
}
);
}
/* ---------------- 事件 ---------------- */
function bindStations(){
$('#btnAdd').onclick = openAddStation;
const q = $('#q');
q.addEventListener('input', () => {
S.q = q.value;
$('#qClear').classList.toggle('hide', !S.q);
clearTimeout(_qLock);
_qLock = setTimeout(renderStList, 120);
});
$('#qClear').onclick = () => {
q.value = ''; S.q = ''; $('#qClear').classList.add('hide'); renderStList();
};
}
INIT.push(bindStations);
;
/* ==== pages.js ==== */
/* ============================================================
pages.js —— 历史 / 体检 / 设置（含 WiFi 配网、配对、恢复出厂）
============================================================ */
'use strict';
/* ============================================================
一、收听记录
============================================================ */
async function loadHistory(){
const box = $('#histList');
if (!S.hist) box.innerHTML = '<div class="empty">' + ic('clock') + '正在读记录…</div>';
try{
S.hist = await get('/api/history');
renderHistory();
}catch (e){
box.innerHTML = '<div class="empty">' + ic('cloud-off') + '读不到记录</div>';
}
}
function renderHistory(){
const h = S.hist;
if (!h) return;
// 近 7 天（接口给的是「新→旧」，不足 7 天就补空）
const days = (h.days || []).slice(0, 7);
while (days.length < 7) days.push({ d: '', s: 0, p: 0 });
const week = days.reduce((a, b) => a + (b.s || 0), 0);
$('#stats').innerHTML =
'<div class="stat hero"><div class="k">今天</div><div class="v">' + fmtShort(h.today) + '</div></div>' +
'<div class="stat"><div class="k">近 7 天</div><div class="v">' + fmtShort(week) + '</div></div>' +
'<div class="stat"><div class="k">一共</div><div class="v">' + fmtShort(h.total) + '</div></div>';
$('#weekSum').textContent = days.filter(x => x.s > 0).length + ' 天有收听';
const max = Math.max(60, ...days.map(x => x.s || 0));
const WD = ['日','一','二','三','四','五','六'];
$('#bars').innerHTML = days.map((d, k) => {
const pct = Math.max(4, Math.round((d.s || 0) / max * 100));
let lb = '—';
if (d.d){
const dt = new Date(d.d + 'T00:00:00');
lb = k === 0 ? '今天' : '周' + WD[dt.getDay()];
}
return '<div class="b' + (k === 0 ? ' now' : '') + '" style="animation-delay:' + (k * 40) + 'ms">' +
'<div class="bar' + (k === 0 ? ' now' : '') + '"><i style="height:' + pct + '%"></i></div>' +
'<div class="lb">' + lb + '</div></div>';
}).join('');
const list = h.list || [];
$('#histHint').textContent = list.length ? '一共听过 ' + list.length + ' 个台' : '机器自动记账';
if (!list.length){
$('#histList').innerHTML = '<div class="empty">' + ic('history') +
'还没有记录<br>听一会儿，这里就会自己长出来</div>';
return;
}
$('#histList').innerHTML = list.map((it, k) =>
'<div class="row" data-k="' + k + '" style="--i:' + Math.min(k, 12) + '">' +
'<span class="ix">' + (k + 1) + '</span>' +
'<div class="nm"><div class="t">' + esc(it.n) + '</div>' +
'<div class="m"><span class="tag gray">' + esc(it.g || '未打标签') + '</span>' +
'<span>' + fmtRel(it.last) + '</span></div></div>' +
'<div class="rt"><b>' + fmtDur(it.s) + '</b>' +
'<span>听过 ' + it.p + ' 次</span></div>' +
'</div>'
).join('');
$$('#histList .row').forEach(r => {
r.onclick = () => replayHist(list[+r.dataset.k]);
});
}
// 再听一次：整张表就在本机，直接按台名找（1.15.0 起不用先切分类）
async function replayHist(it){
if (!it) return;
toast('正在找「' + it.n + '」…');
try{
if (!S.list || !S.list.length){ await loadStations(); }
const idx = S.list.findIndex(x => x.n === it.n);
if (idx < 0){ toast('这个台已经被删了', true); return; }
await post('/api/play', { id: idx });
toast('正在播 ' + it.n);
pollScreen();
}catch (e){ toast('没切过去', true); }
}
/* ============================================================
二、体检
============================================================ */
async function loadDiag(){
try{
S.diag = await get('/api/diag');
renderDiag();
renderHlDiag();          // 1.17.0：台的健康（进来就先画一次，别等下一轮轮询）
pollHealth(true);
const badge = $('#diagBadge');
const score = healthScore(S.diag).score;
badge.classList.toggle('hide', score >= 80);
}catch (e){
$('#chkList').innerHTML = '<div class="empty">' + ic('cloud-off') + '读不到机器状态</div>';
}
}
function healthScore(d){
const items = [];
const push = (name, icon, ok, val, sub) => items.push({ name, icon, ok, val, sub });
const wifiOk = d.wifi && d.wifi.ok;
const rssi = d.wifi ? d.wifi.rssi : -127;
const ps = d.audio ? d.audio.ps : 0;
const heap = d.heap || 0;
const fsPct = d.fsTotal ? Math.round(d.fsUsed / d.fsTotal * 100) : 0;
// 1 网络
push('网络连接', 'wifi', wifiOk, wifiOk ? '已连' : '断开',
wifiOk ? (d.wifi.ssid || '') + ' · 信道 ' + d.wifi.ch : '机器没连上路由器');
// 2 信号
const sigOk = rssi > -78;
push('信号强度', 'signal', wifiOk && sigOk, rssi > -127 ? rssi + ' dBm' : '—',
!wifiOk ? '连上网才能测' : rssi > -55 ? '很强' : rssi > -67 ? '够用' : rssi > -78 ? '偏弱，离路由器远' : '太弱了，容易断');
// 3 播放
const playOk = ps === 2;
push('播放状态', ps === 2 ? 'play' : 'pause', playOk,
ps === 2 ? '正在播' : ps === 1 ? '连接中' : ps === 3 ? '缓冲中' : ps === 4 ? '出错' : '停着',
ps === 2 ? (d.audio.kbps ? d.audio.kbps + ' kbps' : '') : '点「重开这一个台」试试');
// 4 存储
const fsOk = fsPct < 88;
push('存储空间', 'hard-drive', fsOk, fsPct + '%',
Math.round((d.fsTotal - d.fsUsed) / 1024) + ' KB 可用');
// 5 内存
const heapOk = heap > 45000;
push('运行内存', 'cpu', heapOk, Math.round(heap / 1024) + ' KB',
heap > 90000 ? '富余' : heapOk ? '够用' : '偏少，重启一下更稳');
// 6 时钟
const timeOk = d.time && d.time.ok;
push('时间同步', 'clock', timeOk, timeOk ? '正常' : '未同步',
timeOk ? '屏上时间准时' : '连上网会自动校准');
// 7 台表
const tabOk = (d.table.total || 0) > 0 && (d.table.count || 0) > 0;
push('台表', 'list-music', tabOk, (d.table.total || 0) + ' 台',
(d.table.tags || 0) + ' 个标签，共 ' + (d.table.count || 0) + ' 台');
// 8 上次关机
const rst = d.reset || '';
const rstOk = rst === '上电' || rst === '软件重启' || rst === '深睡唤醒';
push('上次关机', 'power', rstOk, rst || '—',
rstOk ? '正常' : '上次是非正常退出，留意一下');
let score = 100;
items.forEach(it => { if (!it.ok) score -= (it.name === '网络连接' || it.name === '播放状态') ? 18 : 10; });
score = Math.max(5, Math.min(100, score));
return { items, score };
}
function renderDiag(){
const d = S.diag;
if (!d) return;
const { items, score } = healthScore(d);
const bad = items.filter(x => !x.ok);
// 环
const C = 2 * Math.PI * 38;
const fg = $('#ringFg');
fg.setAttribute('stroke-dasharray', C.toFixed(1));
fg.setAttribute('stroke-dashoffset', (C * (1 - score / 100)).toFixed(1));
fg.setAttribute('stroke', score >= 80 ? '#4a8a63' : score >= 60 ? '#bb7546' : '#ab5147');
$('#ringVal').innerHTML = score + '<small>分</small>';
$('#healthTitle').textContent =
score >= 90 ? '一切正常' : score >= 75 ? '基本没问题' : score >= 55 ? '有点小毛病' : '需要处理一下';
$('#healthDesc').textContent =
bad.length === 0 ? '网络、声音、存储都正常，放心用。'
: bad.length === 1 ? '发现 1 个问题：' + bad[0].name + '。' + bad[0].sub
: '发现 ' + bad.length + ' 个问题，点下面「一键修复」试试。';
// 顶部提示
const alertBox = $('#diagAlert');
if (!bad.length){
alertBox.innerHTML = '';
} else {
const worst = bad[0];
const hard = !bad.every(x => ['播放状态', '信号强度'].indexOf(x.name) >= 0);
alertBox.innerHTML =
'<div class="alertcard' + (hard ? '' : ' warn') + '">' +
ic(hard ? 'triangle-alert' : 'circle-alert') +
'<div><div class="t">' + esc(worst.name) + (bad.length > 1 ? ' 等 ' + bad.length + ' 项' : '') + '</div>' +
'<div class="m">' + esc(worst.sub) + '</div></div>' +
'</div>';
}
// 检查项
$('#chkList').innerHTML = items.map(it =>
'<div class="chk ' + (it.ok ? 'ok' : 'warn') + '">' +
'<span class="ci">' + ic(it.icon, 'ic sm') + '</span>' +
'<span class="ct"><span class="t">' + it.name + '</span>' +
'<span class="m">' + esc(it.sub || '') + '</span></span>' +
'<span class="cv">' + esc(it.val) + '</span>' +
'</div>'
).join('');
$('#fixAllTx').textContent = bad.length ? '一键修复这 ' + bad.length + ' 个问题' : '全部正常，再检查一遍';
}
// 一键修复：按问题类型排出动作顺序，逐个执行
async function fixAll(){
const d = S.diag;
if (!d){ toast('还没读到状态', true); return; }
const { items } = healthScore(d);
const bad = items.map(x => x.name).filter(n => items.find(y => y.name === n && !y.ok));
if (!items.some(x => !x.ok)){ await loadDiag(); toast('都正常，不用修'); return; }
const steps = [];
if (items.find(x => x.name === '网络连接' && !x.ok)) steps.push('reconnect');
if (items.find(x => x.name === '播放状态' && !x.ok)) steps.push('replay');
if (items.find(x => x.name === '台表' && !x.ok)) steps.push('fix_table');
if (!steps.length) steps.push('replay');
$('#fixAllTx').textContent = '正在修…';
try{
for (const s of steps){
const j = await post('/api/fix', { action: s });
toast(j.msg || '已处理');
await new Promise(r => setTimeout(r, 900));
}
await loadDiag();
await pollScreen();
}catch (e){ toast('修复请求没成功', true); }
$('#fixAllTx').textContent = '一键修复';
}
async function runFix(action){
const needConfirm = ['factory', 'reset_stations', 'forget_wifi'].indexOf(action) >= 0;
const run = async (confirm) => {
try{
const j = await postOk('/api/fix', { action, confirm: confirm ? 1 : 0 });
toast(j.msg || '已处理');
if (j.reboot) rebootWait();
else { setTimeout(loadDiag, 800); setTimeout(pollScreen, 900); }
}catch (e){ toast('没成功', true); }
};
if (needConfirm){
confirmSheet({
title: '确定要这样吗？',
desc: '这一步做完就回不去了。', ok: '确定', danger: true,
onOk: () => run(true)
});
return;
}
run(false);
}
function rebootWait(){
const b = $('#boot');
b.style.display = '';
b.classList.remove('gone');
b.innerHTML =
'<div class="logo">' + ic('radio') + '</div>' +
'<div class="msg">正在重启收音机…</div>' +
'<div class="bar2"><i></i></div>' +
'<div style="font-size:12.5px;color:var(--ink3);max-width:250px;text-align:center;line-height:1.7">' +
'大概 15 秒，页面会自己回来</div>';
let n = 0;
const t = setInterval(async () => {
n++;
try{
const r = await fetch('/api/info', { cache: 'no-store' });
if (r.ok){ clearInterval(t); location.reload(); }
}catch (e){}
if (n > 40){ clearInterval(t); location.reload(); }
}, 1500);
}
// 1.46.35：点「重新连接网络」时，机器会**先回包、再断链**（700ms）。
//   但断链那一瞬间 TCP 仍可能正好被掐，App 这边偶尔还是会收到 fetch 失败。
//   那不是「没成功」—— 机器确实在重连。所以这里分两种：
//     · 带回业务错误码（err / ok:0）→ 真失败，直接说；
//     · 纯网络错（没有 code）      → 当已发出，等它自己回来。
async function doReconnect(){
toast('正在让机器重连 WiFi…');
try{
const j = await postOk('/api/fix', { action: 'reconnect' });
if (j && j.msg) toast(j.msg);
}catch (e){
if (e && e.code){                 // 机器明确回了个错误
toast('没成功（' + e.code + '）', true);
return;
}
// 没 code = 请求发出去之后链路断了，这正是预期
}
waitNetBack();
}
// 等机器把网络接回来（最多约 24 秒），回来就刷新设置页
function waitNetBack(){
const b = $('#boot');
b.style.display = '';
b.classList.remove('gone');
b.innerHTML =
'<div class="logo">' + ic('wifi') + '</div>' +
'<div class="msg">正在重新连 WiFi…</div>' +
'<div class="bar2"><i></i></div>' +
'<div style="font-size:12.5px;color:var(--ink3);max-width:250px;text-align:center;line-height:1.7">' +
'大概 10 秒。连上后这页会自己回来</div>';
let n = 0;
const t = setInterval(async () => {
n++;
try{
const r = await fetch('/api/info', { cache: 'no-store' });
if (r.ok){
clearInterval(t);
b.classList.add('gone');
toast('网络已经重新连上');
loadSettings();
pollScreen();
return;
}
}catch (e){}
if (n > 16){                      // 16 × 1.5s ≈ 24s
clearInterval(t);
b.innerHTML =
'<div class="logo">' + ic('wifi-off') + '</div>' +
'<div class="msg">还是没连上</div>' +
'<div style="font-size:12.5px;color:var(--ink3);max-width:270px;text-align:center;line-height:1.75">' +
'机器可能还在找网络，或者这个 WiFi 现在连不上。<br>' +
'过一会儿点「换一个 WiFi」重新配一次。</div>' +
'<button class="btn" id="btnNetRetry" style="margin-top:14px">重试一次</button>' +
'<button class="btn ghost" id="btnNetBack" style="margin-top:8px">先回去</button>';
const r1 = $('#btnNetRetry'); if (r1) r1.onclick = () => { b.classList.add('gone'); doReconnect(); };
const r2 = $('#btnNetBack');  if (r2) r2.onclick = () => { b.classList.add('gone'); location.reload(); };
}
}, 1500);
}
/* ============================================================
三、设置
============================================================ */
/* ---------------- 1.19.0 电量 ---------------- */
// /api/state 里带的字段：bok(有没有电池) / batt(%) / bmv(毫伏) / chg / blow
let _lowTold = false;
function renderBatt(st){
const row = $('#battRow');
if (!row) return;
const ok = !!(st && st.bok);
row.style.display = ok ? '' : 'none';        // 没接电量检测就整行不出现
if (!ok) return;
const pct = +st.batt, mv = +st.bmv || 0;
$('#battNow').textContent = (pct >= 0 ? pct + '%' : '—');
// 1.38.2：把校准系数也显示出来，用户一眼能看出"机器自己调过"。
//   系数 = 1.0000 说明从没校准过，读数只靠分压比推 —— 这时候最容易不准。
const cal = +(st.bcal || 1);
// 1.46.51：机器顶栏「满电 / 插着电」是有显示的（batteryFull / batteryOnExt），
//   App 以前只认 chg（电压上行），充满后就一片空白 —— 用户看着像「没同步」。
// 1.46.55：bfull 是「电压到 4.18 且稳住」，充电时电压被顶高，插上 20 分钟
//   就会为真 —— 但电量可能才 50%。所以「已充满」还必须电量接近满，
//   否则会看到「已充满 · 57%」这种自相矛盾的组合。
const chgTx = st.chg ? ' · 正在充电'
: ((st.bfull && pct >= 95) ? ' · 已充满'
: (st.bext ? ' · 接着电源'
: (st.blow ? ' · 快没电了，插上充电器' : '')));
$('#battSub').textContent = (mv / 1000).toFixed(2) + ' V' + chgTx +
(cal.toFixed(4) !== '1.0000' ? ' · 已校准 ×' + cal.toFixed(3) : '');
const calBtn = $('#btnBattCal');
if (calBtn) calBtn.style.display = '';
if (st.blow && !_lowTold){ _lowTold = true; toast('电量低，请充电'); }
}
/* ---------------- 1.38.2 电量校准 ----------------
为什么需要用户自己填：读数 = 引脚电压 × 分压比(1.71) × 校准系数。
1.71 是推出来的，不是量出来的 —— 差 3% 就是 100mV，在锂电平台区那是十几到三十个百分点。
唯一能"和真实一致"的办法：拿万用表量电池两端，把真实值填进来，机器自己缩放系数。
这里不要求用户输入"4.02"这种精确值，给他一个滑条 + 几个常用档位，
因为万用表读数本身就有 ±0.01V 的误差，填到 10mV 精度已经够了。 */
async function openBattCal(){
// 注意：/api/screen 里**没有**电量字段，电量只在 /api/state 里（平铺的 bok/batt/bmv）。
// 这里现取一次，别图省事从 S.scr 读 —— 那样会拿到 undefined，弹层里全是 0.00。
let st = {};
try{ st = await get('/api/state'); }catch (e){}
const cur = +st.bmv || 0;                    // 机器现在认为的电压
const raw = +st.braw || 0;                   // 引脚原始读数（不含补偿）
const cal = +(st.bcal || 1);
openSheet(
'<h3>校准电量</h3>' +
'<p class="desc">机器的电量是按电压算的，天生会有偏差。<br>' +
'拿万用表量一下电池两端（或者看充电器上的电压显示），填进去就行。</p>' +
'<div class="field"><label>机器现在认为</label>' +
'<div style="font-size:22px;font-weight:800">' + (cur / 1000).toFixed(2) + ' V</div>' +
'<div style="font-size:12px;color:var(--ink3);margin-top:2px">' +
'引脚原始 ' + raw + ' mV · 校准系数 ×' + cal.toFixed(3) + '</div></div>' +
'<div class="field"><label>万用表量到的是多少伏</label>' +
'<input type="number" id="bcMv" step="0.01" min="3" max="4.4" ' +
'value="' + (cur / 1000).toFixed(2) + '" placeholder="4.02"></div>' +
'<div class="acts"><button class="btn ghost" id="bcNo">取消</button>' +
'<button class="btn pri" id="bcYes">按这个校准</button></div>',
sheet => {
$('#bcNo', sheet).onclick = closeSheet;
$('#bcYes', sheet).onclick = async () => {
const v = parseFloat($('#bcMv', sheet).value);
if (!(v >= 2.5 && v <= 4.5)) { toast('填 2.5 ~ 4.5 之间的数', true); return; }
try{
await post('/api/battcal', { mv: Math.round(v * 1000) });
toast('已校准到 ' + v.toFixed(2) + ' V');
closeSheet();
setTimeout(loadSettings, 400);
}catch (e){ toast('没发出去，重试一下', true); }
};
}
);
}
/* ---------------- 1.19.0 我的机器 ---------------- */
// 数据来自 MQTT 的设备广播（CLOUD.S.devs）。云没连上时列表是空的，
// 但已经在控的那台仍然显示出来 —— 不然用户会以为"机器丢了"。
function renderDevs(list){
const box = $('#devList');
if (!box) return;
const hint = $('#devHint');
const arr = (list || CLOUD.S.devs || []).slice().sort((a, b) => {
const ca = a.dev === CLOUD.S.dev ? 0 : 1, cb = b.dev === CLOUD.S.dev ? 0 : 1;
if (ca !== cb) return ca - cb;
return (b.seen || 0) - (a.seen || 0);
});
if (!arr.length){
if (hint) hint.textContent = CLOUD.S.phase === 'online' ? '没找到' : '正在找…';
box.innerHTML = '<div class="empty" style="padding:14px 0 6px">' + ic('radio') +
'还没看到机器<br><span style="font-size:12px;color:var(--ink3)">机器连上家里 WiFi 后会自动出现在这里</span></div>';
return;
}
if (hint) hint.textContent = arr.length + ' 台';
box.innerHTML = arr.map(d => {
const cur = d.dev === CLOUD.S.dev;
const alive = CLOUD.devAlive(d);
// 1.46.7：把机器报上来的 4 位码也显示出来 —— 否则两台都叫 1234 时，
//   用户看着两个一模一样的名字不知道该点哪个。
return '<button class="devrow' + (cur ? ' cur' : '') + '" data-d="' + esc(d.dev) + '">' +
'<span class="dot ' + (alive ? 'ok' : '') + '"></span>' +
'<span class="nm">' + esc(d.n || ('MyRadio-' + d.dev)) + '</span>' +
(d.pin ? '<span class="pn">' + esc(d.pin) + '</span>' : '') +
'<span class="mt">' + (d.batt >= 0 ? d.batt + '%' : '') +
(cur ? ' · 正在控' : (alive ? '' : ' · 离线')) + '</span>' +
'</button>';
}).join('');
$$('.devrow', box).forEach(b => b.onclick = () => useDev(b.dataset.d));
}
/* 1.46.7 修：设备广播到了要自己刷「我的机器」。
原来 cloud.js 的注释写着「onDevs: pages.js 挂的」，可 pages.js 从来没挂过 ——
于是这份列表只在点「重新找一遍」时才动：打开设置页看到的是「还没看到机器」，
等 30 秒机器报到了也不出现，用户会以为机器丢了（1.19.0 起就有的老问题）。
做三件事：300ms 合并（一次报到会连着来两台）、内容没变就不动 DOM
（每次重建 innerHTML 会让正在点的按不到）、新机器一到立刻出现。 */
let devPaintT = 0, devSig = '\u0000';
function devSignature(list){
return (list || []).map(d =>
d.dev + ':' + (d.pin || '') + ':' + (d.batt >= 0 ? d.batt : '-') + ':' +
(CLOUD.devAlive(d) ? 1 : 0)).join('|');
}
CLOUD.S.onDevs = function(list){
clearTimeout(devPaintT);
devPaintT = setTimeout(function(){
const sig = devSignature(list);
if (sig === devSig) return;
devSig = sig;
renderDevs(list);
}, 300);
};
// 选一台机器来控：免配对的直接算码连上；关了免配对的要它那串遥控码
// 连中转服务器（预览器里不连 —— 那会真的往公网发 WebSocket）
function cloudStart(){
if (typeof window !== 'undefined' && window.MR_PREVIEW) return;
CLOUD.enable(true);
}
function useDev(dev){
const ok = CLOUD.pickDev(dev);
cloudStart();
if (!ok){
toast('这台关了免配对，需要它的遥控码', true);
// 1.39.0：「手动输入遥控码」已删，这里不再自动弹它
return;
}
toast('已切到 ' + (CLOUD.S.name || dev));
setTimeout(() => loadSettings().then(() => pollScreen()), 500);
}
/* ---------------- 1.46.7 连接码：显示 + 修改 ----------------
固件侧 /api/pair?op=pin&code=5678 在 1.46.5 就有了（改完还会在机器屏上显示一次，
省得用户忘记），只是一直没有界面。这里补上，因为说明书和界面都在说「输 1234」——
不能改的话，家里两台都叫 1234 就永远分不开。 */
function openPinEdit(){
// ⚠️ 这里**不能**先 await /api/pair：跨网那条通道一次请求最长 9 秒，
//   点了没反应、9 秒后才弹层，用户会以为按钮坏了。
//   loadSettings 早就把 S.pair 读回来了，直接用；万一没有就先弹（显示 —），
//   再在后台补一次，补到了顺手填进输入框。
const cur = (S.pair && S.pair.pin) ? S.pair.pin : '';
if (!cur) get('/api/pair').then(p => {
if (!p || !p.pin) return;
S.pair = p;
const sv = $('#svPin'); if (sv) sv.textContent = p.pin;
const el = document.getElementById('peIn');
if (el && !el.value) el.value = p.pin;
}).catch(() => {});
openSheet(
'<h3>连接码</h3>' +
'<p class="desc">手机输这 4 位数字就能找到这台机器 —— 同一个 WiFi 里是这样，' +
'人在外地用远程版也是输它。<br>' +
'<b>出厂是 1234。</b>家里有两台机器时，把其中一台改成别的，就不会分不清了。</p>' +
'<div class="field"><label>新的一串（4 个数字）</label>' +
'<input id="peIn" inputmode="numeric" maxlength="4" autocomplete="off" ' +
'value="' + esc(cur) + '" placeholder="1234" ' +
'style="font-size:24px;letter-spacing:8px;text-align:center;font-weight:800"></div>' +
'<p class="desc" style="margin-top:6px">改完机器屏幕上会显示一次，方便记下来。' +
'已经在用的手机会自动跟着换，不用重新连。</p>' +
'<div class="acts"><button class="btn ghost" id="peNo">取消</button>' +
'<button class="btn pri" id="peYes">改成这个</button></div>',
sheet => {
const inp = $('#peIn', sheet);
inp.oninput = () => {
const v = inp.value.replace(/\D/g, '').slice(0, 4);
if (v !== inp.value) inp.value = v;
};
$('#peNo', sheet).onclick = closeSheet;
$('#peYes', sheet).onclick = async () => {
const v = String(inp.value || '').replace(/\D/g, '');
if (v.length !== 4) { toast('要 4 个数字，比如 5678', true); return; }
try{
const r = await post('/api/pair', { op: 'pin', code: v });
if (!r || !r.ok) { toast('没改成，重试一下', true); return; }
if (S.pair) S.pair.pin = r.pin || v;
$('#svPin').textContent = r.pin || v;
closeSheet();
toast('连接码已改成 ' + (r.pin || v));
setTimeout(loadSettings, 300);
}catch (e){ toast('没改成，重试一下', true); }
};
}
);
}
/* ---------------- 1.46.7 跨网输 4 位码 ----------------
机器每 30 秒往 myradio/dev/<短码> 广播一次，payload 里带了自己的 4 位码。
所以这里「查」就是本地筛（见 cloud.js 的 pinLogin）：
唯一命中 → 直接切过去（和点列表一样）
多台同码 → 只把这几台留下，让用户在列表里点
零台     → 说清是「没上线」还是「固件太老不报码」 */
function devPinGo(){
const el = $('#devPin');
if (!el) return;
const v = String(el.value || '').replace(/\D/g, '').slice(0, 4);
if (v.length !== 4) { toast('要 4 个数字，比如 1234', true); return; }
const r = CLOUD.pinLogin(v);
if (r.n === 1){
if (r.needCode){
toast('这台的码对上了，但它关了免配对 —— 请用它的 12 位遥控码', true);
return;
}
CLOUD.enable(true);
toast('已连到 ' + (r.name || r.dev));
setTimeout(() => { loadSettings().then(() => pollScreen()); }, 400);
return;
}
if (r.n > 1){
renderDevs(r.list);                       // 只把同码的这几台留下
const h = $('#devHint'); if (h) h.textContent = r.n + ' 台同码，点一台';
toast('有 ' + r.n + ' 台都用 ' + v + '，点一台');
return;
}
toast(CLOUD.anyPin()
? '没找到用 ' + v + ' 的机器，确认码没打错'
: '没找到：这些机器固件太老，不报连接码', true);
}
function initDevPin(){
const el = $('#devPin'), go = $('#devPinGo');
if (!el) return;
go.onclick = devPinGo;
el.addEventListener('keydown', e => { if (e.key === 'Enter') devPinGo(); });
el.addEventListener('input', () => {
const v = el.value.replace(/\D/g, '').slice(0, 4);
if (v !== el.value) el.value = v;
if (v.length === 4) setTimeout(devPinGo, 120);
});
}
async function loadSettings(){
try{
const [d, p, st] = await Promise.all([get('/api/diag'), get('/api/pair'), get('/api/state')]);
S.diag = d; S.pair = p;
// 1.46.7：把当前 4 位连接码显示出来（改码弹层和这一行共用同一个值）
const pv = $('#svPin'); if (pv) pv.textContent = p.pin || '—';
$('#svFw').textContent = 'v' + (d.fw || '—');
$('#svIp').textContent = (d.wifi && d.wifi.ip) || '—';
$('#svUp').textContent = fmtUp(d.up);
$('#setHint').textContent = (d.wifi && d.wifi.ok ? d.wifi.ssid : '未连网');
const wf = d.wifi || {};
const off = !wf.ok;
$('#wifiNow').textContent = wf.ok ? (wf.rssi + ' dBm') : '未连接';
$('#wifiSub').textContent = wf.ok
? ('当前：' + (wf.ssid || ''))
: '收音机还没上网，点这里 3 步连上';
const wt = $('#wifiTitle');
if (wt) wt.textContent = wf.ok ? 'WiFi' : '给收音机配网';
const wc = $('#wifiCard');
if (wc) wc.classList.toggle('needset', off);
// 1.39.0：配对码这套 UI 已经删掉（app 只绑这一台、不需要码）。
//   这里保留成「有元素才更新」的写法 —— 万一以后又加回来，也不会因为
//   取到 null 而抛 TypeError 把整个设置页打空。
renderBatt(st);                  // 1.19.0：电量（没接检测就自动隐藏）
// 屏幕与声音：亮度上限 / 两路音量 / 熄屏延时（值来自收音机，改了会真的生效）
// 1.44.0：两路量程**不一样了**（原厂口径：网络 netvolume 6..21、收音 volume 6..63），
//   所以两条滑条各用各的 min/max。之前两条共用 volmin/volmax 是因为
//   那时两边都是 3..10 档，现在共用会把 FM 那条卡在 21。
setVolRange($('#rgVol'),   st.volmin,  st.volmax);
setVolRange($('#rgFmVol'), st.fvolmin, st.fvolmax);
setRange($('#rgVol'),   st.vol   != null ? st.vol   : 18);
setRange($('#rgFmVol'), st.fmvol != null ? st.fmvol : 35);
setRange($('#rgBr'),    st.bmax  != null ? st.bmax  : 100);
$$('#segOff button').forEach(b =>
b.classList.toggle('on', +b.dataset.v === +(st.offmin != null ? st.offmin : 3)));
// ---- 1.41.0 收音机那一路（SI4732）：波段 / 静噪 / 半步进 / 搜台 ----
renderFm(st);
// 已存 WiFi 的组数直接写在行上，不用点进去才知道
try{
const wl = await get('/api/wifi/list');
S.wifiJ = wl;
const n = (wl && wl.list) ? wl.list.length : 0;
const max = wifiSlotCount(wl);
const nEl = $('#wifiNow');
if (nEl) nEl.textContent = n ? (n + ' / ' + max + ' 组') : '—';
const sEl = $('#wifiSub');
if (sEl) sEl.textContent = n
? ('已存 ' + wl.list[0].ssid + (n > 1 ? ' 等 ' + n + ' 组' : '') + ' · 点这里管理')
: ('机器一个 WiFi 都没记 · 点这里填一个');
// 老页面上的「已存的 WiFi」那一行（新版已合并，取不到就算了）
const n0 = $('#savedN');
if (n0) n0.textContent = n + ' 组';
const s0 = $('#savedSub');
if (s0) s0.textContent = n ? ('第 1 组：' + wl.list[0].ssid + '（开机先连它）')
: '机器一个 WiFi 都没记';
}catch (e){}
}catch (e){}
// ---- 1.8.0 远程控制 ----
try{
const c = await get('/api/cloud');
S.cloud = c;
// 1.42.0：固件恒开，这里只读状态。on 字段老固件可能有 0，一律显示「已开启」
$('#svRHost').textContent = (c.host || '—') + (c.port ? ':' + c.port : '');
$('#svRSt').textContent = c.conn ? '已连上' : (c.on === 0 ? '连不上服务器' : '连接中…');
// 顺手把遥控码记到本机：以后在外地打开远程版，不用再抄一遍
if (c.short && c.code) CLOUD.save({ dev: c.short, code: c.code, host: c.host || CLOUD.S.host });
// 1.19.0：免配对开关（开着才广播、才认算出来的码）
// 1.39.0：「免配对」开关已删（app 默认就绑这一台，不需要开关）
}catch (e){}
// ⚠️ 1.41.0 修 P0：上面这一行 `}catch (e){}` 和下面的 `}` 原来是**丢的**。
//   1.39.0 删「免配对」那一块时把 loadSettings 的收尾一起删掉了，
//   于是 `try{` 一直开着，后面的 setRange / setLock / openWifiSheet…
//   全被吞进 loadSettings 体内，文件到最后少两个 `}` ⇒ 整份 pages.js 语法错误
//   ⇒ App 所有按钮失灵。改这里的任何东西，改完必须
//   `node --check tools/app/js/pages.js`。
}
// 收音机那一路的状态。state 里 sq/hs/fmstn 是 1.41.0 新加的只读字段；
// 老固件没有就从 localStorage 里取上次设过的值，界面上不假装知道真实值。
function renderFm(st){
const on = st.mode === 'fm';
const bandWord = { fm: '调频', am: '中波', lw: '长波', sw: '短波' }[st.band] || '';
// 单位同上：fmfreq = kHz/10，换 MHz 要 /100
const freq = on ? (st.fmfreq ? (st.fmfreq / 100).toFixed(1) + ' MHz'
: (st.khz ? st.khz + ' kHz' : '—')) : '—';
const now = $('#fmNow');
if (now) now.textContent = on ? '正在收音' : '网络模式';
const fq = $('#fmFreq');
if (fq) fq.textContent = freq;
const fs = $('#fmBandSub');
if (fs) fs.textContent = on
? (bandWord + ' · 信号 ' + (st.fmrssi != null ? st.fmrssi : '—'))
: '现在是网络电台；没网时机器会自动切到收音';
// 静噪：0 = 关
const sq = st.sq != null ? st.sq : (+(localStorage.getItem('mr.sq') || 0));
setRange($('#rgSq'), sq);
const sqNow = $('#sqNow');
if (sqNow) sqNow.textContent = sq > 0 ? ('门限 ' + sq) : '关';
// 半步进
const hs = st.hs != null ? (!!st.hs) : (localStorage.getItem('mr.hs') === '1');
const swHalf = $('#swHalf');
if (swHalf) swHalf.classList.toggle('on', hs);
// 搜台
const busy = !!st.fmscan;
const cnt = st.fmstn != null ? st.fmstn : null;
const cEl = $('#fmCount');
if (cEl) cEl.textContent = busy ? ('搜到 ' + (cnt == null ? 0 : cnt) + ' 个') : (cnt == null ? '—' : cnt + ' 个台');
const sub = $('#fmScanSub');
if (sub){
if (busy){
const p = st.fmprog != null ? st.fmprog : 0;
sub.textContent = '正在搜 ' + (p > 100 ? 100 : p) + '%…';
} else {
sub.textContent = '收音机自己扫一遍，十几秒';
}
}
const btn = $('#btnFmScan');
if (btn) btn.disabled = busy;
}
// 滑条：设值同时更新填充比例（--fill 给 track 的渐变用）
function setRange(el, v){
if (!el) return;
el.value = v;
el.style.setProperty('--fill', ((v - el.min) / (el.max - el.min) * 100) + '%');
}
// 1.39.0：配对码保护这套 UI 已删（app 只绑这一台、不需要码）。
// 函数留着是因为固件侧还有 /api/pair 接口，万一以后要恢复设置项可以直接接回来。
// 注意：这里**不再碰任何 DOM** —— 元素已经不在了，写 $('#swLock') 会拿到 null。
//
// ⚠️ 1.41.0 修 P0：原来这里写的是 `async` + 换行 + 三行注释 + `function setLock(on)`。
//    注释把 `async` 和 `function` 拆开了 ⇒ `async` 成了一个单独的表达式语句，
//    `setLock` 变成**普通函数**，里面的 `await` 直接 SyntaxError。
//    pages.js 整份解析失败 ⇒ App 全部按钮失灵（不只是这个设置项）。
//    教训：`async function` 中间绝不能断开，注释要写在前面。
async function setLock(on){
try{
const j = await post('/api/pair', { op: 'lock', on: on ? 1 : 0 });
toast(on ? '已开启配对码保护' : '已关闭配对码保护');
if (on && j.code) pollScreen();      // 让屏上显示码
}catch (e){ toast('设置没成功', true); }
}
// 1.41.0：把搜到的台列出来（原厂就是这里能看见「搜到哪几个」）。
//   点一行 = 直接调过去（/api/fmtune），不用靠上一台/下一台一个个试。
function openFmStationsSheet(){
openSheet(
'<h3>搜到的台</h3>' +
'<p class="desc">收音机自己扫出来的，按「波段 + 信号强弱」排。点一行就调过去。</p>' +
'<div id="fsBody"><div class="empty" style="padding:22px 0">' + ic('refresh-cw') + '正在读…</div></div>',
sheet => { paintFmStations($('#fsBody', sheet)); }
);
}
async function paintFmStations(box){
let j = null;
try{ j = await get('/api/fmstations'); }catch (e){}
const arr = (j && j.list) || [];
if (!arr.length){
box.innerHTML = '<div class="empty" style="padding:22px 0">' + ic('radio') +
'还没搜到台<br><span style="font-size:12px;color:var(--ink3)">点上面的「重新搜台」扫一遍</span></div>';
return;
}
const bandName = { FM: '调频', AM: '中波', LW: '长波', SW: '短波' };
box.innerHTML = '<div class="optlist">' + arr.map(x => {
// FM 存的是 kHz（98000），其他波段本来就是 kHz（999）；
// 万一以后有人按「Hz」送进来也不至于显示成 999000 kHz
const f = x.band === 'FM'
? (x.khz / 1000).toFixed(1) + ' MHz'
: (x.khz >= 100000 ? (x.khz / 1000) : x.khz) + ' kHz';
const bw = x.rssi >= 60 ? 4 : x.rssi >= 40 ? 3 : x.rssi >= 20 ? 2 : 1;
let bars = '';
for (let i = 0; i < 4; i++) bars += '<i class="' + (i < bw ? 'on' : '') + '"></i>';
return '<button class="opt" data-khz="' + x.khz + '">' +
ic('radio', 'ic sm') +
'<span style="flex:1;text-align:left">' + (bandName[x.band] || x.band) + '　<b>' + f + '</b></span>' +
'<span class="r">' + x.rssi + '</span></button>';
}).join('') + '</div>';
$$('[data-khz]', box).forEach(b => b.onclick = async () => {
if (S.scr && S.scr.mode !== 'fm'){
toast('机器现在是网络模式，先让它切到收音（断网会自动切）', true);
return;
}
try{
await post('/api/fmtune', { khz: +b.dataset.khz });
toast('已调到 ' + b.textContent.trim().split('\u3000')[1]);
closeSheet();
setTimeout(pollScreen, 400);
}catch (e){ toast('没调过去，重试一下', true); }
});
}
// 搜台是异步的：每 1.2 秒问一次 /api/state，搜完自己停。
// 上限 40 次（约 48 秒）—— 比固件的搜台耗时留足余量，卡住了也不会一直轮询。
function waitFmScan(){
let n = 0;
const tick = async () => {
n++;
try{
const st = await get('/api/state');
renderFm(st);
if (!st.fmscan || n > 40){
if (n <= 40) toast('搜完了，共 ' + (st.fmstn || 0) + ' 个台');
return;
}
}catch (e){}
if (n <= 40) setTimeout(tick, 1200);
};
setTimeout(tick, 1200);
}
// ============================================================
//  WiFi —— 1.46.51 起「换一个 WiFi」和「已存的 WiFi」合成一页
//
//  用户原话：「已存的 wifi 改为三组，显示已保存的，没有的空着输入名字密码可以给收音机
//  添加 wifi，跟换一个 wifi 两个合并…框后面放一个搜索符号，点击可以扫描到当前有的
//  wifi 点击自动在框里填入名字」。
//
//  所以这一页就是 **N 个格子**（N = 固件容量，1.46.51 起是 4）：
//    · 有东西的格子 → 显示名字 + 「正在用 / 第 N 组」+ [连这个] [忘掉]
//    · 空着的格子   → 名字框 + 🔍 + 密码框 + [保存这组]
//  「保存」只写进 /wifi.txt，**不动当前连接**（见固件 apiWifiAdd 的注释）——
//  填一组备用网络不该把正在听的台打断。
//
//  ⚠️ 扫描回来一定要「重开一页」：openSheet() 会 sheetHost.innerHTML=''，
//     原来抓到的 box 立刻游离，往它里面写什么都看不见（1.46.36 就栽在这）。
//     所以用 _wfDraft 记草稿，重开时回填。
// ============================================================
const _wfDraft = { name: {}, pass: {} };   // 每个格子正在填的内容（扫描/重开时保留）
// 一页里最多画几个格子：固件说几个就几个（老固件没有 max 字段 → 按 3 个）
function wifiSlotCount(j){
const m = +(j && j.max);
return (m >= 1 && m <= 8) ? m : 3;
}
function wifiSlotHtml(i, x, max){
const no = ['①','②','③','④','⑤','⑥','⑦','⑧'][i] || String(i + 1);
if (x){
// ---- 已存 ----
return '<div style="border:1px solid var(--line);border-radius:12px;padding:10px 12px;margin-bottom:8px">' +
'<div style="display:flex;align-items:center;gap:8px">' +
ic('wifi', 'ic sm') +
'<b style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:14.5px">' +
no + ' ' + esc(x.ssid) + '</b>' +
(x.on ? '<span style="font-size:12px;color:var(--ok);font-weight:700">正在用</span>'
: '<span style="font-size:12px;color:var(--ink3)">第 ' + (i + 1) + ' 组</span>') +
'</div>' +
'<div style="display:flex;gap:8px;margin-top:8px">' +
(x.on ? '' : '<button class="btn ghost" data-use="' + esc(x.ssid) + '" ' +
'style="padding:6px 12px;font-size:12.5px">连这个</button>') +
'<button class="btn ghost" data-forget="' + esc(x.ssid) + '" ' +
'style="padding:6px 12px;font-size:12.5px;color:var(--bad)">忘掉</button>' +
'</div>' +
'</div>';
}
// ---- 空着 ----
const nm = _wfDraft.name[i] || '', pw = _wfDraft.pass[i] || '';
return '<div style="border:1px dashed var(--line);border-radius:12px;padding:10px 12px;margin-bottom:8px">' +
'<div style="font-size:12.5px;color:var(--ink3);margin-bottom:6px">' +
no + ' 空着 —— 填上就存进机器，当备用网络</div>' +
'<div style="display:flex;gap:6px">' +
'<input class="wsName" data-i="' + i + '" value="' + esc(nm) + '" placeholder="WiFi 名字" ' +
'autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" ' +
'style="flex:1;padding:10px;border:1px solid var(--line);border-radius:9px;font-size:14px;background:var(--bg2,#fff);color:inherit">' +
'<button class="btn ghost" data-scan="' + i + '" title="扫描附近的 WiFi" ' +
'style="padding:0 12px;min-width:44px">' + ic('search', 'ic sm') + '</button>' +
'</div>' +
'<input class="wsPass" data-i="' + i + '" type="password" value="' + esc(pw) + '" ' +
'placeholder="密码（没有密码就留空）" autocomplete="off" ' +
'style="width:100%;margin-top:6px;padding:10px;border:1px solid var(--line);border-radius:9px;font-size:14px;background:var(--bg2,#fff);color:inherit">' +
'<button class="btn pri" data-save="' + i + '" ' +
'style="width:100%;margin-top:8px;padding:9px;font-size:13.5px">保存这组</button>' +
'</div>';
}
function openWifiSheet(){
const needSetup = !(S.diag && S.diag.wifi && S.diag.wifi.ok);
openSheet(
'<h3>' + (needSetup ? '给收音机连 WiFi' : 'WiFi') + '</h3>' +
'<p class="desc">机器最多记 <b id="wfMax">4</b> 组，开机按顺序挨个试，连上就停。' +
'空着的格子填上就存进机器当备用，<b>不会打断现在正在听的台</b>。</p>' +
(needSetup
? '<div class="steps">' +
'<div class="step"><b>1</b><span>手机连上热点 <b>我的收音机</b>（<b>没有密码</b>，连上会自动弹配网页）</span></div>' +
'<div class="step"><b>2</b><span>在下面某个空格子里填 WiFi 名字和密码，点保存</span></div>' +
'<div class="step"><b>3</b><span>连上后手机会自动回到原来的 WiFi，页面打不开就重连一下</span></div>' +
'</div>'
: '') +
'<div id="wfSlots"><div class="empty" style="padding:22px 0">' + ic('refresh-cw') + '正在读机器…</div></div>' +
'<button class="btn ghost" id="wfScanAll" style="width:100%;margin-top:6px">' +
ic('scan-line', 'ic sm') + '重新扫描附近网络</button>' +
'<p class="desc" style="margin-top:12px;font-size:12.5px">扫描是<b>收音机</b>在做，' +
'所以手机得先够得着它：连它的热点，或者跟它在同一个 WiFi 下。</p>',
sheet => {
$('#wfScanAll', sheet).onclick = () => pickWifiForSlot(null);
loadWifiSlots($('#wfSlots', sheet));
}
);
}
// 老名字留着：别的地方（以及旧回归）还在用
function openSavedWifiSheet(){ openWifiSheet(); }
async function loadWifiSlots(box){
let j = null;
try{ j = await get('/api/wifi/list'); }catch (e){}
const arr = (j && j.list) || [];
const max = wifiSlotCount(j);
const mx = $('#wfMax');
if (mx) mx.textContent = String(max);
// 按槽位序号排（固件回的是文件里的顺序，也就是开机尝试顺序）
const bySlot = {};
arr.forEach((x, k) => { bySlot[+x.i >= 0 ? +x.i : k] = x; });
let html = '';
for (let i = 0; i < max; i++) html += wifiSlotHtml(i, bySlot[i], max);
box.innerHTML = html;
// 名字/密码改动随时记进草稿（扫描回来要保留）
$$('.wsName', box).forEach(el => el.oninput = () => { _wfDraft.name[+el.dataset.i] = el.value; });
$$('.wsPass', box).forEach(el => el.oninput = () => { _wfDraft.pass[+el.dataset.i] = el.value; });
$$('[data-scan]', box).forEach(b => b.onclick = () => pickWifiForSlot(+b.dataset.scan));
$$('[data-save]', box).forEach(b => b.onclick = () => saveWifiSlot(+b.dataset.save, box));
$$('[data-use]', box).forEach(b => b.onclick = () => useSavedWifi(b.dataset.use));
$$('[data-forget]', box).forEach(b => b.onclick = () => forgetWifi(b.dataset.forget, arr.length));
// 设置页那两行也顺手更新（组数写在外面，不用点进来才知道）
const nEl = $('#wifiNow');
if (nEl && arr.length) nEl.textContent = arr.length + ' / ' + max + ' 组';
const sub = $('#wifiSub');
if (sub && arr.length) sub.textContent = '已存 ' + arr[0].ssid +
(arr.length > 1 ? ' 等 ' + arr.length + ' 组' : '') + ' · 点这里管理';
}
// 扫描后把名字填进第 idx 个格子；idx 为 null = 只是想看看，不填
async function pickWifiForSlot(idx){
openSheet(
'<h3>选一个 WiFi</h3>' +
'<p class="desc">' + (typeof idx === 'number'
? '点一个名字，会自动填进第 ' + (idx + 1) + ' 个格子'
: '点一个名字，填进第一个空格子') + '</p>' +
'<div id="pkBody"><div class="empty" style="padding:26px 0">' + ic('scan-line') +
'正在扫描…<br><span style="font-size:12px;color:var(--ink3)">扫描是收音机在做，约 5 秒</span></div></div>' +
'<button class="btn ghost" id="pkAgain" style="width:100%;margin-top:6px">' +
ic('refresh-cw', 'ic sm') + '重新扫描</button>' +
'<div class="acts"><button class="btn ghost" id="pkNo">取消</button>' +
'<button class="btn ghost" id="pkType" style="margin-left:auto">直接手填</button></div>',
sheet => {
const again = () => scanIntoPick($('#pkBody', sheet), idx);
$('#pkAgain', sheet).onclick = again;
$('#pkNo', sheet).onclick = closeSheet;
$('#pkType', sheet).onclick = () => { closeSheet(); setTimeout(openWifiSheet, 120); };
again();
}
);
}
async function scanIntoPick(box, idx){
box.innerHTML = '<div class="empty" style="padding:26px 0">' + ic('scan-line') +
'正在扫描…<br><span style="font-size:12px;color:var(--ink3)">约 5 秒</span></div>';
let arr = [];
let fail = '';
try{ arr = await get('/api/scan'); }catch (e){ fail = '连不上收音机'; }
const bad = (arr || []).some(x => x && x.err);
arr = (arr || []).filter(x => x && x.s).sort((a, b) => b.r - a.r);
if (!arr.length){
const why = fail || (bad ? '收音机刚才正忙着（在连网/在播放），稍等几秒再点「重新扫描」'
: '收音机周围没有可见的 WiFi');
box.innerHTML = '<div class="empty" style="padding:26px 0">' + ic('wifi-off') + why +
'<br><span style="font-size:12px;color:var(--ink3)">也可以点「直接手填」自己打名字</span></div>';
return;
}
box.innerHTML = '<div class="optlist">' + arr.map((x, i) =>
'<button class="opt" data-p="' + i + '">' + ic(x.e === 0 ? 'wifi' : 'lock', 'ic sm') +
'<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + esc(x.s) + '</span>' +
'<span class="r">' + x.r + ' dBm</span></button>'
).join('') + '</div>';
$$('[data-p]', box).forEach(b => b.onclick = () => {
const ssid = arr[+b.dataset.p].s;
let slot = idx;
if (typeof slot !== 'number'){                 // 没指定 → 填第一个空格子
slot = 0;
while (_wfDraft.name[slot]) slot++;
}
_wfDraft.name[slot] = ssid;
closeSheet();
setTimeout(openWifiSheet, 150);
});
}
// 保存一个格子（只写盘，不切换当前连接）
async function saveWifiSlot(i, box){
const nameEl = $('.wsName[data-i="' + i + '"]', box);
const passEl = $('.wsPass[data-i="' + i + '"]', box);
const name = (nameEl ? nameEl.value : '').trim();
const pass = passEl ? passEl.value : '';
if (!name){ toast('先填 WiFi 名字（可以点右边的放大镜扫一下）', true); return; }
if (name.length > 32){ toast('名字太长了，检查一下', true); return; }
try{
const j = await postOk('/api/wifi/add', { ssid: name, pass });
toast('已存进机器：' + name + (j && j.n ? '（现共 ' + j.n + ' 组）' : ''));
}catch (e){
if (e && e.code === 'full')       toast('机器只能记 ' + wifiSlotCount(S.wifiJ) + ' 组，先「忘掉」一组再加', true);
else if (e && e.code === 'write') toast('机器存储写不进去，没存上', true);
else if (e && e.code)             toast('没存上（' + e.code + '）', true);
else                              toast('没收到回执，重开一次看看实际存没存');
}
_wfDraft.name[i] = '';
_wfDraft.pass[i] = '';
setTimeout(openWifiSheet, 320);         // 重开 = 重新问机器
setTimeout(loadSettings, 700);
}
// 用已存的密码去连（不用重新输密码）
async function useSavedWifi(ssid){
confirmSheet({
title: '连到 ' + ssid + '？',
desc: '机器会用存好的密码去连。正在播的台会中断十几秒；连不上会自动退回现在这个网络。',
ok: '连这个',
onOk: async () => {
try{ await post('/api/wifi/use', { ssid }); }
catch (e){
if (e && e.code === 'notfound'){ toast('机器里找不到这一组了', true); setTimeout(openWifiSheet, 300); return; }
// 换网本来就会把自己的链路掐断，没回执不代表没发出去
}
waitWifiSwap(ssid);
}
});
}
// 换网后的等待条：1.6 秒问一次状态，连上/失败都给结论
function waitWifiSwap(ssid){
openSheet(
'<h3>正在连 ' + esc(ssid) + '…</h3>' +
'<div id="wsState" style="font-size:13.5px;color:var(--ink2);min-height:22px;margin:6px 0 4px">' +
'最长 25 秒。连不上会自动退回原来的网络，机器不会失联。</div>' +
'<div class="acts"><button class="btn ghost" id="wsBack">先回去</button></div>',
sheet => {
$('#wsBack', sheet).onclick = closeSheet;
let n = 0;
const t = setInterval(async () => {
n++;
try{
const s = await get('/api/wifi/status');
if (s && s.state === 2){
clearInterval(t);
const st = $('#wsState', sheet);
if (st) st.innerHTML = '<span style="color:var(--ok);font-weight:700">连上了 · ' + esc(s.ip || '') + '</span>';
toast('已换到 ' + ssid);
setTimeout(() => { closeSheet(); loadSettings(); pollScreen(); }, 900);
} else if (s && s.state === 3){
clearInterval(t);
const st = $('#wsState', sheet);
if (st) st.innerHTML = '<span style="color:var(--bad);font-weight:700">' +
esc(s.msg || '连不上') + '</span><div style="margin-top:6px;color:var(--ink3)">' +
'已经退回原来的网络，机器没失联。</div>';
} else {
const st = $('#wsState', sheet);
if (st) st.textContent = '正在连接… ' + n * 2 + 's';
}
}catch (e){}
if (n > 16) clearInterval(t);
}, 1600);
}
);
}
// 忘掉一组
function forgetWifi(ssid, total){
const only = total <= 1;
confirmSheet({
title: '忘掉 ' + ssid + '？',
desc: only
? '这是机器记的<b>最后一组</b> WiFi。删掉之后它就没网了，会自己开热点 —— '
+ '手机连上那个热点（没密码）就能重新配网。'
: '机器会删掉这组名字和密码。如果它正在用这一组，会断开后改连剩下的那组。',
ok: '忘掉', danger: true,
onOk: async () => {
try{
const j = await postOk('/api/wifi/forget', { ssid });
toast(j && j.left ? ('已忘掉 ' + ssid + '，机器里还剩 ' + j.left + ' 组')
: ('已忘掉 ' + ssid + '，机器里一组都不剩了'));
}catch (e){
if (e && e.code === 'notfound')   toast('机器里本来就没有这一组', true);
else if (e && e.code === 'write') toast('机器存储写不进去，没删掉', true);
else if (e && e.code)             toast('没删成（' + e.code + '）', true);
else                              toast('没收到回执，重新读一遍机器的真实状态');
}
// 不能往旧 box 里写（它已随弹层销毁），重开一次 = 重新问机器
setTimeout(() => { openWifiSheet(); setTimeout(loadSettings, 700); }, 320);
}
});
}
function bindPages(){
// ---- 屏幕与声音：全部走 /api，收音机真的会变 ----
const debounce = (fn, ms) => {
let t = 0;
return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
};
const sendSet = debounce((k, v, el) => {
post('/api/set', { k, v }).then(j => {
if (j && j.ok) toast({ bright: '亮度已设为 ' + v, offmin: '熄屏延时已保存' }[k] || '已设置');
if (el) setRange(el, el.value);           // 刷新填充
setTimeout(pollScreen, 500);
}).catch(() => toast('设置没成功', true));
}, 400);
const rgVol = $('#rgVol'), rgBr = $('#rgBr');
// 1.41.0：两路音量分开设（原厂 volume / netvolume）。
//   ch=net 改「网络电台」那路，ch=fm 改「收音机」那路；
//   目标那一路不是当前模式时，机器只记下来，切过去才生效（固件侧也是这么做的）。
//   ⚠️ 原来写的是 `debounce(fn,300)()` —— 每次 input 都新建一个包装再立刻调用，
//   防抖等于没有，拖一次滑条会连发几十个请求。改成先建实例、再调用。
const sendVol = debounce((ch) => {
const el = ch === 'fm' ? $('#rgFmVol') : $('#rgVol');
if (!el) return;
post('/api/vol', { v: +el.value, ch }).catch(() => {});
}, 260);
const rgFmVol = $('#rgFmVol');
if (rgVol) rgVol.oninput = () => { setRange(rgVol, rgVol.value); sendVol('net'); };
if (rgFmVol) rgFmVol.oninput = () => { setRange(rgFmVol, rgFmVol.value); sendVol('fm'); };
if (rgBr) rgBr.oninput = () => setRange(rgBr, rgBr.value);
if (rgBr) rgBr.onchange = () => sendSet('bright', +rgBr.value, rgBr);
$$('#segOff button').forEach(b => b.onclick = () => {
$$('#segOff button').forEach(x => x.classList.remove('on'));
b.classList.add('on');
post('/api/set', { k: 'offmin', v: +b.dataset.v })
.then(j => toast(j && j.ok ? (b.dataset.v === '0' ? '已改为从不熄屏' : '已改为 ' + b.dataset.v + ' 分钟熄屏') : '设置没成功', !(j && j.ok)))
.catch(() => toast('设置没成功', true));
});
$('#btnScreenOff').onclick = async () => {
try{
await post('/api/set', { k: 'screen', v: 0 });
toast('已熄屏，转一下旋钮就亮');
setTimeout(pollScreen, 600);
}catch (e){ toast('没成功', true); }
};
$('#btnReboot').onclick = () => confirmSheet({
title: '重启收音机？',
desc: '大概 15 秒恢复。台表、音量、亮度这些设置都会保留。',
ok: '重启',
onOk: () => runFix('reboot')
});
$('#btnClearHist').onclick = () => confirmSheet({
title: '清空收听记录？',
desc: '时长统计和最近听过的台都会清掉，收音机本身不受影响。',
ok: '清空', danger: true,
onOk: async () => {
try{ await post('/api/fix', { action: 'clear_history' }); S.hist = null; toast('已清空'); loadHistory(); }
catch (e){ toast('没清成', true); }
}
});
$('#btnRecheck').onclick = () => { toast('正在检查…'); loadDiag().then(() => toast('检查完了')); };
$('#btnFixAll').onclick = fixAll;
$$('.srow[data-fix]').forEach(b => b.onclick = () => runFix(b.dataset.fix));
// ---- 1.41.0 收音机那一路 ----
const rgSq = $('#rgSq');
const sendSq = debounce((v) => {
postOk('/api/squelch', { v }).then(() => {
const now = $('#sqNow');
if (now) now.textContent = v > 0 ? ('门限 ' + v) : '关';
try{ localStorage.setItem('mr.sq', v); }catch (e){}
}).catch(() => toast('静噪没设上', true));
}, 350);
if (rgSq) rgSq.oninput = () => {
setRange(rgSq, rgSq.value);
const now = $('#sqNow');
if (now) now.textContent = +rgSq.value > 0 ? ('门限 ' + rgSq.value) : '关';
sendSq(+rgSq.value);
};
const bHalf = $('#btnHalfStep');
if (bHalf) bHalf.onclick = async () => {
const sw = $('#swHalf');
const on = !sw.classList.contains('on');
try{
await post('/api/halfstep', { on: on ? 1 : 0 });
sw.classList.toggle('on', on);
try{ localStorage.setItem('mr.hs', on ? '1' : '0'); }catch (e){}
toast(on ? '半步进已开（0.05MHz 一格）' : '半步进已关');
}catch (e){ toast('没设上，重试一下', true); }
};
const bStations = $('#btnFmStations');
if (bStations) bStations.onclick = openFmStationsSheet;
const bScan = $('#btnFmScan');
if (bScan) bScan.onclick = () => confirmSheet({
title: '重新搜一遍台？',
desc: '收音机会把调频/中波/长波/短波都扫一遍，十几秒。搜的时候机器上转旋钮会卡一点，正在播的网络台不受影响。',
ok: '开始搜',
onOk: async () => {
try{
const j = await post('/api/fmscan');
if (j && j.err === 'notfm'){
toast('机器现在在听网络台，先在机器上切到收音模式', true);
return;
}
if (j && j.err === 'busy'){ toast('已经在搜了，等它搜完'); waitFmScan(); return; }
if (j && j.err === 'chip'){
toast('收音芯片还没准备好，过几秒再点一次', true);
return;
}
toast('正在搜台…');
waitFmScan();
}catch (e){ toast('没发出去', true); }
}
});
// 1.46.51：两个入口合成一个 —— 点哪一行都开同一页（4 个格子）
$('#btnWifi').onclick = openWifiSheet;
const bSaved = $('#btnSavedWifi');
if (bSaved) bSaved.onclick = openWifiSheet;      // 老页面还有这一行就用同一个
$('#btnReconn').onclick = () => confirmSheet({
title: '重新连接网络？',
desc: '会断开当前的 WiFi 再连一次，大概 10 秒。正在播的台会中断一下。',
ok: '重连',
onOk: doReconnect
});
// ---- 1.8.0 远程控制（跨网）----
// 1.42.0：**这里绝不能再发 on:0** —— 固件虽然会拒绝，但 UI 层面就不该给这条路。
//   点这一行只是重新读一次状态（人站在这行前，多半是想知道「现在到底连没连上」）。
$('#btnCloud').onclick = async () => {
try{
const c = await get('/api/cloud');
S.cloud = c;
$('#svRHost').textContent = (c.host || '—') + (c.port ? ':' + c.port : '');
$('#svRSt').textContent = c.conn ? '已连上' : '连接中…';
toast(c.conn ? '中转服务器已连上' : '正在连中转服务器…');
if (!c.conn) setTimeout(loadSettings, 3000); // 给它 3 秒再确认一眼
}catch (e){ toast('没读到状态', true); }
};
// ---- 1.46.7 连接码 ----
$('#btnPinEdit').onclick = openPinEdit;
// ---- 1.19.0 我的机器 ----
initDevPin();          // 1.46.7：跨网输 4 位码那一行
$('#btnDevRefresh').onclick = () => {
cloudStart();
renderDevs();
toast('正在找机器…');
setTimeout(renderDevs, 1500);
};
$('#btnResetStations').onclick = () => confirmSheet({
title: '恢复出厂台表？',
desc: '你自己加过、删过、改过顺序的台都会被清掉，换成出厂带的 341 个台。机器会重启。',
ok: '恢复', danger: true,
onOk: () => runFix('reset_stations')
});
$('#btnForgetWifi').onclick = () => confirmSheet({
title: '忘掉 WiFi？',
desc: '之后机器只会发自己的热点「我的收音机」（没有密码），连上它重新配网。家里的 WiFi 密码不会丢在别处，随时能再输一次。',
ok: '忘掉', danger: true,
onOk: () => runFix('forget_wifi')
});
$('#btnFactory').onclick = () => confirmSheet({
title: '恢复出厂设置？',
desc: '台表、收听记录、配对全部清空，等于刚买回来。这一步做完找不回来。',
ok: '全部清掉', danger: true,
onOk: () => runFix('factory')
});
}
INIT.push(bindPages);
;
/* ==== health.js ==== */
/* ============================================================
health.js —— 台的健康档案（1.17.0）
固件每播一个台就判一次（出没出声 / 连不连得上），结果存在机器上；
这个文件负责：
1. 每秒问一次 /api/health，把「正在试的这台」的实时电平画出来
2. 电台页多一个「有问题 N」的筛选
3. 每行右侧标出「连不上 / 没声音」
4. 体检页去看全表统计、挨个试一遍
数据格式（/api/health，固件 src/health.cpp 拼的）：
n    台数
s    每台一个字符：0 没测过 / 1 正常 / 2 连不上 / 3 没声音
f    每台累计失败次数（0-9）
c    { ok, bad, dim, unk } 各项台数
cur  { i 台号, st 本次结论 0/1/2/3, lv 电平 0..100, sec 已播秒数, why }
============================================================ */
'use strict';
const HL_NAME = ['没测过', '正常', '连不上', '没声音'];
let _hlAt = 0, _hlBusy = false, _hlTimer = 0;
const hlSleep = ms => new Promise(r => setTimeout(r, ms));
/* ---------------- 读档 ---------------- */
function hlOf(i){
const h = S.hl;
if (!h || typeof h.s !== 'string') return 0;
const c = h.s.charAt(i);
return (c >= '0' && c <= '3') ? (+c) : 0;
}
function hlFailOf(i){
const h = S.hl;
if (!h || typeof h.f !== 'string') return 0;
const c = h.f.charAt(i);
return (c >= '0' && c <= '9') ? (+c) : 0;
}
function hlCountBad(){
const c = S.hl && S.hl.c;
return c ? (c.bad | 0) + (c.dim | 0) : 0;
}
// 正在播的这台用「本次会话」的结论，别的台用档案里的
function hlNow(i){
const h = S.hl, c = h && h.cur;
if (c && c.i === i && (c.st | 0) > 0) return c.st | 0;
return hlOf(i);
}
/* ---------------- 轮询 ---------------- */
async function pollHealth(force){
if (_hlBusy || S.offline) return;
const gap = useCloud() ? 1800 : 900;
if (!force && Date.now() - _hlAt < gap - 80) return;
_hlAt = Date.now();
_hlBusy = true;
try{
const h = await get('/api/health');
if (h && typeof h.s === 'string'){
S.hl = h;
applyHealth();
}
}catch (e){
// 老固件没有这个接口：静默关掉，页面照常
if (e && /http 404/.test(String(e.message))) S.hlOff = true;
}finally{
_hlBusy = false;
}
}
/* ---------------- 界面刷新 ---------------- */
function applyHealth(){
const h = S.hl;
if (!h) return;
// 顶部「有问题」的数量
const n = hlCountBad();
const cnt = $('#chipBadCnt');
if (cnt) cnt.textContent = n;
const cb = $('#chipBad');
if (cb) cb.classList.toggle('zero', n === 0);
// 电台页每行的小标
if (S.page === 'stations') syncRowHealth();
// 主页：实时出声
if (S.page === 'home') renderSound();
// 体检页：统计
if (S.page === 'diag') renderHlDiag();
}
function renderSound(){
const bar = $('#lvBar'), tx = $('#sndTx');
if (!bar || !tx) return;
const c = S.hl && S.hl.cur;
const ps = S.scr ? (S.scr.ps | 0) : 0;
let lv = 0, s = '没在放', cls = '';
if (S.offline){ s = '连不上收音机'; cls = 'bad'; }
else if (ps === 0){ s = '没在放'; }
else if (!c || c.i !== (S.scr ? S.scr.idx : -1)){ s = '正在连…'; }
else {
lv = Math.max(0, Math.min(100, c.lv | 0));
if ((c.st | 0) === 1){ s = '在出声'; cls = 'ok'; }
else if ((c.st | 0) === 2){ s = '连不上'; cls = 'bad'; }
else if ((c.st | 0) === 3){ s = '没声音'; cls = 'dim'; }
else { s = lv > 2 ? '在出声' : '等一下…'; cls = lv > 2 ? 'ok' : ''; }
}
bar.style.width = lv + '%';
bar.className = cls;
tx.textContent = s;
tx.className = 'snd ' + cls;
}
// 只改「不一样的行」，不整表重画 —— 否则翻着列表突然回顶
function syncRowHealth(){
$$('#stList .row').forEach(r => {
const i = +r.dataset.i;
const box = r.querySelector('.m');
if (!box) return;
const st = hlNow(i);
let el = box.querySelector('.hl');
let cls = '', txt = '';
if (st === 2){
const f = Math.min(hlFailOf(i), 9);
cls = 'bad'; txt = f > 0 ? '连不上 ' + f + ' 次' : '连不上';
} else if (st === 3){
cls = 'dim'; txt = '没声音';
}
if (!cls){ if (el) el.remove(); return; }
if (!el){
el = document.createElement('span');
el.dataset.hl = i;
el.onclick = e => { e.stopPropagation(); openHealth(i); };
box.appendChild(el);
}
if (el.className !== 'hl ' + cls) el.className = 'hl ' + cls;
if (el.textContent !== txt) el.textContent = txt;
});
}
/* ---------------- 单台详情 ---------------- */
async function openHealth(i){
const s = S.list[i] || {};
let d = null;
try { d = await post('/api/health', { i }); } catch (e){}
const st = d ? (d.st | 0) : hlOf(i);
const fail = d ? (d.fail | 0) : hlFailOf(i);
const lastOk = d ? (d.lastOk | 0) : 0;
const lastChk = d ? (d.lastChk | 0) : 0;
const tip = st === 2 ? '机器连不上这个地址，播到它会自动跳过。'
: st === 3 ? '连上了、数据也在流，但解出来一直是静音 —— 多半是音频格式被标错了。'
: st === 1 ? '这台出过声，是好的。'
: '还没在这台机器上试过。';
openSheet(
'<h3>' + esc(d ? d.n : s.n) + '</h3>' +
'<p class="desc">' + tip + '</p>' +
'<div class="list" style="box-shadow:none">' +
'<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">这台的状态</div></div>' +
'<span class="hl ' + ['', 'ok', 'bad', 'dim'][st] + '" style="position:static;margin-left:auto">' +
HL_NAME[st] + '</span></div>' +
'<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">失败次数</div></div>' +
'<span class="sv" style="margin-left:auto">' + fail + ' 次</span></div>' +
'<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">上次正常出声</div></div>' +
'<span class="sv" style="margin-left:auto">' + (lastOk ? fmtRel(lastOk) : '没有过') + '</span></div>' +
'<div class="row" style="min-height:52px"><div class="nm"><div class="t" style="font-size:15px">上次检查</div></div>' +
'<span class="sv" style="margin-left:auto">' + (lastChk ? fmtRel(lastChk) : '没查过') + '</span></div>' +
'</div>' +
'<div class="acts">' +
'<button class="btn ghost" id="hNo">知道了</button>' +
'<button class="btn pri" id="hTry">试一下这台</button>' +
'</div>',
sheet => {
$('#hNo', sheet).onclick = closeSheet;
$('#hTry', sheet).onclick = async () => {
closeSheet();
try { await post('/api/play', { id: i }); toast('正在试「' + (s.n || '') + '」'); pollScreen(); }
catch (e){ toast('切台失败', true); }
};
}
);
}
/* ---------------- 挨个试一遍 ---------------- */
let SCAN = null;
function openScanSheet(){
const c = (S.hl && S.hl.c) || { ok: 0, bad: 0, dim: 0, unk: 0 };
const unkIds = [], badIds = [], allIds = [];
S.list.forEach((s, i) => {
allIds.push(i);
const st = hlOf(i);
if (st === 0) unkIds.push(i);
if (st === 2 || st === 3) badIds.push(i);
});
if (!S.list.length){ toast('台表还没读出来', true); return; }
const opt = (a, n, m, dis) =>
'<button class="opt" data-a="' + a + '"' + (dis ? ' disabled style="opacity:.45"' : '') + '>' +
'<span style="flex:1">' + n + '</span><span class="r">' + m + '</span></button>';
openSheet(
'<h3>挨个试一遍</h3>' +
'<p class="desc">机器会把台一个个开几秒，看它<b>到底有没有出声</b>。' +
'好的台 2 秒就过，坏的台要等 13 秒才判死，中间会听见断断续续的片段。</p>' +
'<div class="optlist">' +
opt('unk', '只试没测过的', unkIds.length + ' 个台', !unkIds.length) +
opt('bad', '重试有问题的', badIds.length + ' 个台', !badIds.length) +
opt('all', '全试一遍', allIds.length + ' 个台') +
'</div>',
sheet => {
$$('.opt', sheet).forEach(b => b.onclick = () => {
const a = b.dataset.a;
if (b.disabled) return;
closeSheet();
if (a === 'unk') startScan(unkIds);
if (a === 'bad') startScan(badIds);
if (a === 'all') startScan(allIds);
});
}
);
}
async function startScan(ids){
if (SCAN){ toast('正在试，先停一下', true); return; }
if (!ids.length){ toast('没有要试的台'); return; }
SCAN = { ids, k: 0, ok: 0, bad: 0, dim: 0, late: 0, stop: false,
back: S.scr ? (S.scr.idx | 0) : -1, t0: Date.now(), nm: '' };
openSheet(
'<h3>正在试台</h3>' +
'<p class="desc" id="scNow">—</p>' +
'<div class="prog"><i id="scBar"></i></div>' +
'<div class="scrow"><span id="scNum">0 / 0</span><span id="scTally"></span></div>' +
'<div class="acts one"><button class="btn ghost" id="scStop">停下来</button></div>',
sheet => {
$('#scStop', sheet).onclick = () => {
if (SCAN) SCAN.stop = true;
$('#scStop', sheet).textContent = '正在停…';
};
}
);
const S0 = SCAN;
for (let k = 0; k < S0.ids.length; k++){
if (S0.stop) break;
const id = S0.ids[k];
S0.k = k + 1;
S0.nm = (S.list[id] || {}).n || ('第 ' + (id + 1) + ' 台');
scanUi();
let v = 0;
try { await post('/api/play', { id }); } catch (e){}
const t0 = Date.now();
while (!S0.stop && Date.now() - t0 < 13000){
await hlSleep(400);
await pollHealth(true);
const c = S.hl && S.hl.cur;
if (c && (c.i | 0) === id && (c.st | 0) >= 1){ v = c.st | 0; break; }
if (Date.now() - t0 > 4200 && (S.hl && S.hl.cur && (S.hl.cur.i | 0) === id)) scanUi();
}
if (v === 1) S0.ok++;
else if (v === 2) S0.bad++;
else if (v === 3) S0.dim++;
else S0.late++;
scanUi();
}
const done = S0.k, total = S0.ids.length;
const txt = '试了 ' + done + '/' + total + ' 个：' +
S0.ok + ' 个正常' +
(S0.bad ? '，' + S0.bad + ' 个连不上' : '') +
(S0.dim ? '，' + S0.dim + ' 个没声音' : '') +
(S0.late ? '，' + S0.late + ' 个没测出来' : '');
SCAN = null;
closeSheet(true);
// 回到试之前那个台，别把人正在听的换走
if (S0.back >= 0 && S0.back < S.list.length) { try { await post('/api/play', { id: S0.back }); } catch (e){} }
await pollHealth(true);
if (S.page === 'stations') renderStList();
if (S.page === 'diag') renderHlDiag();
toast(txt);
}
function scanUi(){
if (!SCAN) return;
const now = $('#scNow');
if (now) now.textContent = '正在试「' + SCAN.nm + '」';
const bar = $('#scBar');
if (bar) bar.style.width = Math.round(SCAN.k * 100 / Math.max(1, SCAN.ids.length)) + '%';
const num = $('#scNum');
if (num) num.textContent = SCAN.k + ' / ' + SCAN.ids.length;
const tl = $('#scTally');
if (tl) tl.textContent = SCAN.ok + ' 正常 · ' + (SCAN.bad + SCAN.dim) + ' 有问题';
}
/* ---------------- 体检页那张卡 ---------------- */
function renderHlDiag(){
const h = S.hl;
if (!h || !h.c) return;
const c = h.c;
$('#hlOk').textContent = c.ok | 0;
$('#hlBad').textContent = c.bad | 0;
$('#hlDim').textContent = c.dim | 0;
$('#hlUnk').textContent = c.unk | 0;
const bad = hlCountBad();
$('#hlSum').textContent = bad ? bad + ' 个要留意' : '都没问题';
$('#hlSum').style.color = bad ? 'var(--bad)' : '';
const sub = $('#hlScanSub');
if (sub) sub.textContent = (c.unk | 0) ? '还有 ' + (c.unk | 0) + ' 个台没试过' : '全表都试过了';
}
/* ---------------- 事件 ---------------- */
function bindHealth(){
const b1 = $('#btnHlList');
if (b1) b1.onclick = () => {
S.tag = -2;
go('stations');
setTimeout(() => { renderStChips(); renderStList(); }, 30);
};
const b2 = $('#btnHlScan');
if (b2) b2.onclick = openScanSheet;
_hlTimer = setInterval(() => {
if (document.hidden || SCAN) return;
pollHealth();
}, 900);
pollHealth(true);
}
INIT.push(bindHealth);
</script>
</body>
</html>)HTMLX";
