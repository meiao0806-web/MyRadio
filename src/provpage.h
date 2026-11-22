// ============================================================
//  provpage.h —— 配网页（照原厂固件那套页面做的）
//
//  为什么要有这个：1.45.0 之前，手机连上收音机热点、打开 192.168.4.1，
//  拿到的是整个「远程控制」网页版（180KB，还带着 App 的配对/轮询）。
//  配网这个场景其实只需要一件事：填 WiFi 名字和密码，点保存。
//  老人用手机浏览器打开时，那个大网页既慢又不知道点哪儿。
//
//  所以：从热点（192.168.4.x）来的请求，或者机器根本没存过 WiFi 时，
//  首页直接给这一页；家里局域网访问仍然给完整的网页版。
//
//  版式照原厂 page1/page2：浅灰底、白卡片、蓝色折叠条、绿色保存按钮、
//  「✓ 配置保存成功」。字段名沿用原厂的 ssid/pass、ssid2/pass2、ssid3/pass3，
//  第 4 组是 1.46.51 加的（ssid4/pass4）—— 原厂只给 3 组，但固件里能存 4 组。
//
//  两个页面都是纯静态字符串，取值靠 /api/prov/slots 和 /api/wifi/status 补，
//  表单本身是普通 POST（没有 JS 也能提交）。
// ============================================================
#pragma once

static const char PROV_HTML[] = R"PROVH(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="format-detection" content="telephone=no">
<title>给收音机连 WiFi</title>
<style>
*{box-sizing:border-box;font-family:Arial,Helvetica,sans-serif}
body{margin:0;padding:10px;background:#f5f5f5;color:#333;font-size:16px;-webkit-text-size-adjust:100%}
.container{background:#fff;margin:14px auto;padding:20px;max-width:600px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1)}
h2{text-align:center;margin:0 0 6px;color:#2c3e50;font-size:23px}
.sub{text-align:center;color:#7f8c8d;font-size:14px;margin:0 0 16px;line-height:1.5}
.status{background:#f7fbff;border:1px solid #dbe9f6;border-radius:6px;padding:10px 12px;font-size:14.5px;line-height:1.7;margin-bottom:16px}
.status b{color:#2c3e50}
.status .ok{color:#1e8e4e;font-weight:bold}
.collapsible{background:#3498db;color:#fff;padding:12px;width:100%;border:none;text-align:left;font-size:16px;border-radius:4px;display:flex;justify-content:space-between;align-items:center}
.collapsible:after{content:"-";font-weight:bold;font-size:20px}
.collapsible.closed:after{content:"+"}
.panel{padding:12px 10px;background:#f9f9f9;border-radius:0 0 4px 4px}
.panel.hide{display:none}
.group{border:1px solid #eee;border-radius:8px;padding:12px;margin-bottom:12px;background:#fff}
.group b{display:block;font-size:14.5px;color:#2c3e50;margin-bottom:8px}
label{display:block;margin:0 0 5px;font-size:14px;color:#555}
input{width:100%;padding:12px 10px;border:1px solid #ddd;border-radius:4px;font-size:16px;background:#fff}
input:focus{outline:none;border-color:#3498db}
.btn{display:block;width:100%;padding:15px;background:#2ecc71;color:#fff;border:none;border-radius:4px;font-size:17px;font-weight:bold;margin-top:6px;cursor:pointer}
.btn:active{opacity:.9}
.help{color:#7f8c8d;font-size:13.5px;margin:10px 0 0;line-height:1.7}
.help b{color:#e07b00}
</style>
</head>
<body>
<div class="container">
  <h2>给收音机连 WiFi</h2>
  <p class="sub">连上家里的 WiFi，收音机才能听网络电台，手机也能看到它的屏幕</p>

  <div class="status" id="st">正在读取状态…</div>

  <form method="post" action="/setup" id="f">
    <button type="button" class="collapsible" id="hd">WiFi 设置（能存 4 个）</button>
    <div class="panel" id="pn">

      <div class="group">
        <b>第 1 个 · 常用的那个</b>
        <label for="ssid">WiFi 名称</label>
        <input id="ssid" name="ssid" list="nets" placeholder="家里 WiFi 的名字" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
        <label for="pass" style="margin-top:10px">密码</label>
        <input id="pass" name="pass" type="text" placeholder="留空 = 不改原来的密码" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
      </div>

      <div class="group">
        <b>第 2 个 · 备用，可以不填</b>
        <label for="ssid2">WiFi 名称</label>
        <input id="ssid2" name="ssid2" list="nets" placeholder="（不用就留空）" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
        <label for="pass2" style="margin-top:10px">密码</label>
        <input id="pass2" name="pass2" type="text" placeholder="留空 = 不改原来的密码" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
      </div>

      <div class="group">
        <b>第 3 个 · 备用，可以不填</b>
        <label for="ssid3">WiFi 名称</label>
        <input id="ssid3" name="ssid3" list="nets" placeholder="（不用就留空）" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
        <label for="pass3" style="margin-top:10px">密码</label>
        <input id="pass3" name="pass3" type="text" placeholder="留空 = 不改原来的密码" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
      </div>

      <div class="group">
        <b>第 4 个 · 备用，可以不填</b>
        <label for="ssid4">WiFi 名称</label>
        <input id="ssid4" name="ssid4" list="nets" placeholder="（不用就留空）" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
        <label for="pass4" style="margin-top:10px">密码</label>
        <input id="pass4" name="pass4" type="text" placeholder="留空 = 不改原来的密码" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
      </div>

      <datalist id="nets"></datalist>
      <button type="submit" class="btn">保存并连接</button>

      <p class="help">
        密码留空就表示不改动原来存的密码。<br>
        路由器如果有 2.4G 和 5G 两个名字，<b>选不带 5G 的那个</b>，收音机只认 2.4G。<br>
        保存后热点会自己关掉，手机会回到原来的 WiFi。
      </p>
    </div>
  </form>
</div>

<script>
(function(){
  function $(id){ return document.getElementById(id); }
  function esc(s){
    return String(s == null ? '' : s).replace(/[&<>"']/g, function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];
    });
  }

  // 折叠条：默认展开（配网时用户就是要填这个）
  $('hd').onclick = function(){
    var p = $('pn'), c = this.classList.toggle('closed');
    p.classList.toggle('hide', c);
  };

  // 当前连没连上
  function loadStatus(){
    fetch('/api/wifi/status', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){
      var h;
      if (s && s.ok && s.ssid) {
        h = '当前：<span class="ok">已连上 ' + esc(s.ssid) + '</span><br>地址 <b>' + esc(s.ip || '') + '</b>';
      } else if (s && s.state === 1) {
        h = '当前：<b>正在连接…</b>';
      } else {
        h = '当前：<b>还没连上 WiFi</b>（下面填好点保存）';
      }
      $('st').innerHTML = h;
    }).catch(function(){ $('st').textContent = '读不到状态，不影响保存'; });
  }

  // 已经存过的名字填进输入框（密码不返回，留空即不改）
  function loadSlots(){
    fetch('/api/prov/slots', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(j){
      var f = $('f'), names = ['ssid', 'ssid2', 'ssid3', 'ssid4'];
      (j && j.list || []).forEach(function(x, i){
        var el = f.elements[names[i]];
        if (el && !el.value) el.value = x.s || '';
      });
    }).catch(function(){});
  }

  // 附近能扫到的名字，做成输入框的候选
  function loadNets(){
    fetch('/api/scan', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(arr){
      var d = $('nets');
      (arr || []).forEach(function(x){
        if (!x || !x.s) return;
        var o = document.createElement('option');
        o.value = x.s;
        d.appendChild(o);
      });
    }).catch(function(){});
  }

  loadStatus();
  loadSlots();
  loadNets();
})();
</script>
</body>
</html>)PROVH";

// ---- 保存成功后那一页（原厂 page2 的版式，但不重启：重启会把热点关掉，
//      万一密码错了用户就再也进不来了。这里改成盯着真实连接结果）----
static const char PROV_OK_HTML[] = R"PROVOK(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>保存成功</title>
<style>
*{box-sizing:border-box;font-family:Arial,Helvetica,sans-serif}
body{margin:0;padding:40px 16px;background:#f5f5f5;color:#333;text-align:center}
.container{background:#fff;margin:0 auto;padding:24px 20px;max-width:520px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1)}
.success{color:#4CAF50;font-size:24px;margin-bottom:16px;font-weight:bold}
.state{font-size:16px;line-height:1.8;min-height:56px}
.state .ok{color:#1e8e4e;font-weight:bold}
.state .bad{color:#e74c3c;font-weight:bold}
.tip{color:#7f8c8d;font-size:14px;margin-top:14px;line-height:1.7}
a.btn2{display:inline-block;margin-top:18px;padding:12px 22px;background:#3498db;color:#fff;text-decoration:none;border-radius:4px;font-size:16px}
</style>
</head>
<body>
<div class="container">
  <div class="success">&#10003; 配置已保存</div>
  <div class="state" id="s">正在让收音机连 WiFi…</div>
  <div class="tip" id="t">最多等 30 秒</div>
  <a class="btn2" href="/setup">返回修改</a>
</div>
<script>
(function(){
  function $(id){ return document.getElementById(id); }
  var n = 0;
  function tick(){
    n++;
    fetch('/api/wifi/status', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){
      if (s && s.ok && s.ssid) {
        $('s').innerHTML = '<span class="ok">已连上 ' + String(s.ssid).replace(/[&<>]/g, '') + '</span><br>' +
                           '地址 <b>' + String(s.ip || '') + '</b>';
        $('t').textContent = '收音机已经上网了。手机可以回到原来的 WiFi，热点马上会自己关掉。';
        return;                        // 停：连上了就不用再问了
      }
      if (s && s.state === 3) {
        $('s').innerHTML = '<span class="bad">没连上</span><br>' + String(s.msg || '检查一下密码').replace(/[&<>]/g, '');
        $('t').textContent = '密码错一个字都连不上。注意大小写，或者离路由器近一点再试。';
        return;
      }
      if (n > 24) {
        $('s').innerHTML = '<span class="bad">还是没连上</span>';
        $('t').textContent = '密码或信号可能有问题，点下面「返回修改」重填一次。';
        return;
      }
      $('s').textContent = '正在让收音机连 WiFi…（' + (n * 1.2).toFixed(0) + ' 秒）';
      setTimeout(tick, 1200);
    }).catch(function(){
      if (n > 24) { $('s').innerHTML = '<span class="bad">连不上收音机了</span>'; $('t').textContent = '手机可能已经回到原来的 WiFi，重连一下收音机的热点再看。'; return; }
      setTimeout(tick, 1200);
    });
  }
  setTimeout(tick, 800);
})();
</script>
</body>
</html>)PROVOK";
