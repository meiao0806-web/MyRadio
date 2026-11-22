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
