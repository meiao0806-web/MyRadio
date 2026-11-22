# -*- coding: utf-8 -*-
"""离线验证 assets/home.html 的「填表 + 发送」配网面板（1.42.0 起）。

做法：把 home.html 复制一份，注入假 MRApp 桥 + 断言脚本，
      headless Chrome --dump-dom，从 <pre id="out"> 里抠 OK/FAIL。

1.42.0 的关键前提（用户要求）：
  - **没有一键配网**：App 不再替手机切网，也不再让收音机发热点当首选；
    用户在一张表里填「WiFi 名字 + 密码」（家里路由器或手机热点都行），
    点「让收音机连上」→ provSend → 等结果。
  - **没有「离线遥控 / 不配网直接控制」**（provDirect 已删）。
  - 面板只负责「探测 → 填表 → 发送 → 报结果」，步骤条 s1/s2 已删。
"""
import io, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.join(ROOT, "android", "app", "src", "main", "assets", "home.html")
CHROME = r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe"
if not os.path.exists(CHROME):
    CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"

html = io.open(HOME, encoding="utf-8").read()

MOCK = """
<script>
window.__calls = [];
window.MRApp = {
  ver: function(){ return 'test'; },
  scan: function(){ __calls.push('scan'); },
  recent: function(){ return '[]'; },
  open: function(ip,n){ __calls.push('open:' + ip); },
  openRemote: function(){ __calls.push('openRemote'); },
  forget: function(ip){},
  // 1.42.0 桥：probe（探测手机够不够得着机器）/ send / rescan / cancel / finish。
  // provJoin / provDirect 已删 —— mock 里也**不给**，谁要是还去调就说明页面改回去了。
  provProbe: function(){ __calls.push('provProbe'); },
  provRescan: function(){ __calls.push('provRescan'); },
  provSend: function(s,p){ __calls.push('provSend:' + s + '/' + p); },
  provCancel: function(){ __calls.push('provCancel'); },
  provFinish: function(ip){ __calls.push('provFinish:' + ip); },
  openWifiSettings: function(){ __calls.push('openWifiSettings'); },
  curSsid: function(){ return 'ChinaNet-3F2A'; },
  provSaved: function(){ return ''; },
  // 1.46.8：热点改名「我的收音机」、去掉密码
    apInfo: function(){ return JSON.stringify({ssid:'我的收音机',pass:'',ip:'192.168.4.1',sdk:34}); }
};
</script>
"""

DRIVER = """
<pre id="out" style="pointer-events:none;position:fixed;left:0;bottom:0;
  font:12px monospace;white-space:pre-wrap;max-height:46vh;overflow:auto"></pre>
<script>
(function(){
  var L = [], o = document.getElementById('out');
  function log(s){ L.push(s); o.textContent = L.join('\\n'); }
  function ok(name, cond, extra){ log((cond ? 'OK   ' : 'FAIL ') + name + (extra ? '  [' + extra + ']' : '')); }
  function $(id){ return document.getElementById(id); }
  function txt(id){ var e = $(id); return e ? e.textContent : ''; }
  function calls(){ return window.__calls || []; }
  function cnt(p){ return calls().filter(function(c){ return c.indexOf(p) === 0; }).length; }
  /** 真的看不见才算隐藏 —— 只看 el.hidden 会漏掉「class 把 display 抢回来」这种坑 */
  function invisible(id){
    var e = $(id);
    if (!e) return true;
    var cs = getComputedStyle(e);
    return cs.display === 'none' || cs.visibility === 'hidden' || e.offsetParent === null;
  }

  var steps = [];
  function step(fn, delay){ steps.push({f: fn, d: delay || 60}); }
  function run(i){
    if (i >= steps.length) { log('DONE'); return; }
    var s = steps[i];
    try { s.f(); } catch(e) { log('FAIL 步骤 ' + i + ' 抛错: ' + e); }
    setTimeout(function(){ run(i+1); }, s.d);
  }

  // ---------------- 首页 ----------------
  step(function(){
    ok('页面有「设置 WiFi」主按钮', !!$('btnSetup'));
    ok('按钮是主色大按钮', $('btnSetup').className.indexOf('main') >= 0, $('btnSetup').className);
    ok('手动输地址收进可折叠块', $('manualCard') && $('manualCard').tagName.toLowerCase() === 'details');
    ok('配网面板初始隐藏', $('setup').className.indexOf('on') < 0, $('setup').className);
    // 1.42.0：旧热点流程的步骤条 s1/s2、直接控制按钮都必须不存在
    ok('没有旧版步骤条 s1/s2', !$('s1') && !$('s2'));
    ok('没有「不配网直接控制」按钮', !$('btnSetupDirect'));
  });

  // ---------------- 打开面板：先探测 ----------------
  step(function(){ $('btnSetup').click(); });
  step(function(){
    ok('点开面板 → 调 provProbe（探测手机够不够得着机器）', cnt('provProbe') === 1, calls().join(','));
    ok('面板打开', $('setup').className.indexOf('on') >= 0);
    ok('提示「正在检查」', txt('setupBody').indexOf('正在检查') >= 0, txt('setupBody'));
    ok('退路按钮还没出来', invisible('setupActs'));
  });

  // ---------------- ready：预填家 WiFi，出表单 ----------------
  step(function(){
    window.__mrProv({stage:'ready', name:'MyRadio', fw:'MyRadio 1.42.0', via:'sta',
      base:'192.168.1.23', homeSsid:'ChinaNet-3F2A', nets:[]});
  });
  step(function(){
    ok('ready：状态条说机器就在这个网里', txt('viaTx').indexOf('机器就在这个网里') >= 0, txt('viaTx'));
    ok('ready：SSID 框已被预填成家里那个', $('ssid') && $('ssid').value === 'ChinaNet-3F2A',
       $('ssid') ? $('ssid').value : '无输入框');
    ok('ready：有密码框', !!$('pw') && $('pw').type === 'password');
    ok('ready：说明里讲了只认 2.4G', txt('setupBody').indexOf('2.4G') >= 0, txt('setupBody'));
    ok('ready：提示「已经帮你填好了」', txt('setupMsg').indexOf('帮你填好') >= 0, txt('setupMsg'));
    ok('ready：有「从手机附近选」入口', !!$('btnPick'));
    ok('ready：有「用我手机的热点」入口', !!$('btnPhoneHot'));
    ok('ready：默认不展开列表', document.querySelectorAll('#netList .wi').length === 0);
    ok('ready：SSID 可以自己改（是 input）', $('ssid').tagName.toLowerCase() === 'input');
    // 这一条踩过：.row2{display:flex} 把 [hidden] 的 display:none 覆盖了
    ok('ready：失败退路按钮真的看不见（不是只加了 hidden 属性）', invisible('setupActs'));
    var actBtns = document.querySelectorAll('#setupBody .row2 button');
    ok('ready：面板里只该有「让收音机连上」一个主按钮', actBtns.length === 1
        && actBtns[0].id === 'btnPwGo', actBtns.length + ' 个');
  });

  // ---------------- 手机热点：同一个表单 ----------------
  step(function(){ $('btnPhoneHot').click(); });
  step(function(){
    ok('点「用我手机的热点」：讲清楚去哪开热点', txt('setupMsg').indexOf('个人热点') >= 0, txt('setupMsg'));
    ok('热点入口不清掉已经填的名字', $('ssid').value === 'ChinaNet-3F2A', $('ssid').value);
  });

  // ---------------- 从手机附近选（App 侧扫，不找收音机）----------------
  step(function(){ $('btnPick').click(); });
  step(function(){
    ok('点「从手机附近选」→ 调 provRescan（不是让收音机扫）', cnt('provRescan') === 1, calls().join(','));
    ok('等列表时有转圈', document.querySelectorAll('#netList .spin').length === 1);
    window.__mrProv({stage:'nets', nets:[
      {s:'Xiaomi_AX3000',r:-72,sec:1},{s:'TP-LINK_5G',r:-48,sec:1},
      {s:'CMCC-8kQz',r:-86,sec:0},{s:'ChinaNet-3F2A',r:-61,sec:1}]});
  });
  step(function(){
    var items = document.querySelectorAll('#netList .wi');
    ok('nets：列出 4 个网络', items.length === 4, '实际 ' + items.length);
    ok('nets：按信号从强到弱', items[0].textContent.indexOf('TP-LINK_5G') >= 0, items[0].textContent);
    ok('nets：带 dBm', /-48dBm/.test(items[0].textContent), items[0].textContent);
    items[1].click();
  });
  step(function(){
    ok('点一个网络 → 填进 SSID 框', $('ssid').value === 'ChinaNet-3F2A', $('ssid').value);
    ok('点完收起列表', document.querySelectorAll('#netList .wi').length === 0);
    ok('按钮文案回到「从手机附近选一个」', txt('btnPick').indexOf('从手机附近选一个') >= 0, txt('btnPick'));
  });
  step(function(){
    $('ssid').value = '我改过的名字';
    $('btnPick').click();
  });
  step(function(){
    ok('再展开时用缓存，不重复扫（不多调 provRescan）', cnt('provRescan') === 1, calls().join(','));
    ok('再展开时列表直接出来', document.querySelectorAll('#netList .wi').length === 4);
    ok('展开列表不会清掉已经打的字', $('ssid').value === '我改过的名字', $('ssid').value);
    $('btnPick').click();
  });
  step(function(){
    ok('再点一次能收起', document.querySelectorAll('#netList .wi').length === 0);
    $('ssid').value = 'ChinaNet-3F2A';
  });

  // ---------------- 眼睛 / 空名字拦截 ----------------
  step(function(){ $('eye').click(); });
  step(function(){
    ok('点「显示」→ 密码明文', $('pw').type === 'text', $('pw').type);
    $('eye').click();
  });
  step(function(){
    ok('再点一次 → 密文', $('pw').type === 'password', $('pw').type);
    $('ssid').value = '';
    $('pw').value = 'x';
    $('btnPwGo').click();
  });
  step(function(){
    ok('SSID 空着不让发', cnt('provSend') === 0, calls().join(','));
    ok('SSID 空着会提示', txt('setupMsg').indexOf('还没填 WiFi 名字') >= 0, txt('setupMsg'));
  });

  // ---------------- 发送 ----------------
  step(function(){
    $('ssid').value = 'ChinaNet-3F2A';
    $('pw').value = 'hunter2';
    $('btnPwGo').click();
  });
  step(function(){
    ok('发送时调 provSend(ssid, pass)', cnt('provSend:ChinaNet-3F2A/hunter2') === 1, calls().join(','));
    ok('发送中：倒计时 30 秒', /30 秒/.test(txt('setupHint')), txt('setupHint'));
    ok('发送中：有转圈', document.querySelectorAll('#setupBody .spin').length === 1);
    ok('发送中：说明失败会回滚', txt('setupMsg').indexOf('退回') >= 0, txt('setupMsg'));
    ok('发送中：退路按钮收起', invisible('setupActs'));
  });

  // ---------------- 成功 ----------------
  step(function(){ window.__mrProv({stage:'ok', ssid:'ChinaNet-3F2A', ip:'192.168.1.47'}); });
  step(function(){
    ok('成功：显示新地址', txt('setupMsg').indexOf('192.168.1.47') >= 0, txt('setupMsg'));
    ok('成功：写「配网完成」', txt('setupBody').indexOf('配网完成') >= 0, txt('setupBody'));
    ok('成功：提一句远程控制不用开', txt('setupMsg').indexOf('远程控制') >= 0, txt('setupMsg'));
    ok('成功：停止倒计时', txt('setupHint').indexOf('配好了') >= 0, txt('setupHint'));
  }, 2400);   // 等 1.6s 后的 provFinish
  step(function(){
    ok('成功：1.6s 后调 provFinish 进遥控页', cnt('provFinish:192.168.1.47') === 1, calls().join(','));
  });

  // ---------------- 关掉 / 失败 / 重来 ----------------
  step(function(){ $('btnSetupX').click(); });
  step(function(){
    ok('点关掉 → 调 provCancel', cnt('provCancel') >= 1, calls().join(','));
    ok('点关掉 → 面板收起', $('setup').className.indexOf('on') < 0);
  });

  step(function(){ $('btnSetup').click(); });
  step(function(){ window.__mrProv({stage:'fail', msg:'连不上 ChinaNet-3F2A，密码或信号有问题'}); });
  step(function(){
    ok('失败：把原因写出来', txt('setupMsg').indexOf('密码或信号') >= 0, txt('setupMsg'));
    ok('失败：出现两个退路按钮', !invisible('setupActs'));
    ok('失败：倒计时停', txt('setupHint').indexOf('没成功') >= 0, txt('setupHint'));
  });

  step(function(){ $('btnSetupRetry').click(); });
  step(function(){
    ok('点「再检查一次」→ 重新 provProbe', cnt('provProbe') >= 2, calls().join(','));
    ok('点重来 → 提示在等机器应答', txt('setupHint').indexOf('等机器应答') >= 0, txt('setupHint'));
  });

  // ---------------- need-net：手机够不着机器（唯一要借热点的场景）----------------
  step(function(){ window.__mrProv({stage:'need-net', nets:[]}); });
  step(function(){
    ok('need-net：说清要手动连哪个热点', txt('setupMsg').indexOf('我的收音机') >= 0, txt('setupMsg'));
    // 1.46.8：热点没有密码了 —— 该说的是「没有密码」，不是把空密码拼成「（密码 ）」
ok('need-net：说明热点没有密码', txt('setupMsg').indexOf('没有密码') >= 0, txt('setupMsg'));
ok('need-net：不会拼出半截「（密码 ）」', txt('setupMsg').indexOf('（密码 ）') < 0, txt('setupMsg'));
    ok('need-net：说明这时不能上网', txt('setupMsg').indexOf('不能上网') >= 0, txt('setupMsg'));
    ok('need-net：退路按钮出来（去系统设置 / 再检查）', !invisible('setupActs'));
  });

  step(function(){ $('btnGoWifi').click(); });
  step(function(){
    ok('点「去系统设置连「我的收音机」」→ openWifiSettings', cnt('openWifiSettings') === 1, calls().join(','));
  });

  // ---------------- locked / noperm ----------------
  step(function(){ window.__mrProv({stage:'ready', name:'MyRadio', fw:'MyRadio 1.42.0', via:'sta'}); });
  step(function(){ window.__mrProv({stage:'locked'}); });
  step(function(){
    ok('locked：指路配对码', txt('setupMsg').indexOf('配对码') >= 0, txt('setupMsg'));
  });

  step(function(){ window.__mrProv({stage:'ready', name:'MyRadio', fw:'MyRadio 1.42.0', via:'sta'}); });
  step(function(){ window.__mrProv({stage:'noperm'}); });
  step(function(){
    ok('无权限：说清只影响列表、手输照常', txt('setupMsg').indexOf('手打 WiFi 名字') >= 0, txt('setupMsg'));
  });

  // ---------------- 回归：设备发现 / 手动地址 ----------------
  step(function(){
    window.__mrDev('192.168.4.1','MyRadio','MyRadio 1.42.0','scan');
    ok('设备列表能加一台', document.querySelectorAll('#devList .dev').length === 1,
       '实际 ' + document.querySelectorAll('#devList .dev').length);
  });
  step(function(){
    $('ip').value = '999.1.1.1'; $('btnGo').click();
    ok('非法 IP 被拦住', cnt('open:') === 0, calls().join(','));
  });
  step(function(){
    $('ip').value = '192.168.1.23'; $('btnGo').click();
    ok('合法 IP 能打开', cnt('open:192.168.1.23') === 1, calls().join(','));
  });

  setTimeout(function(){ run(0); }, 120);
})();
</script>
"""

out_html = html.replace("<script>\n(function () {", MOCK + "<script>\n(function () {", 1)
if MOCK not in out_html:
    sys.exit("!! 注入点没命中，home.html 的 script 开头变了")
out_html = out_html.replace("</body>", DRIVER + "\n</body>", 1)

tmp = os.path.join(tempfile.gettempdir(), "mr_prov_test.html")
io.open(tmp, "w", encoding="utf-8").write(out_html)

profile = os.path.join(tempfile.gettempdir(), "mr_chrome_prof")
cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
       "--user-data-dir=" + profile, "--window-size=420,900",
       "--allow-file-access-from-files", "--virtual-time-budget=20000",
       "--dump-dom", "file:///" + tmp.replace("\\", "/")]
r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
dom = r.stdout or ""

m = re.search(r'<pre id="out"[^>]*>(.*?)</pre>', dom, re.S)
if not m:
    print("!! 没抓到 #out，dump 前 2000 字：\n", dom[:2000])
    sys.exit(1)
body = (m.group(1).replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
                    .replace("&quot;", '"').replace("&#39;", "'"))
print(body)

fails = body.count("FAIL ")
oks = body.count("OK   ")
print("\n通过 %d / 失败 %d" % (oks, fails))
sys.exit(1 if fails else 0)
