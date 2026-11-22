// 13 听书网 -> tools/有声书源-听书网.json（一部书一个台，抓「全部集数」）
//
// 为什么非要用真浏览器：
//   1) 站点对非浏览器客户端做 TLS/JS 指纹拦截（裸 HTTP 回 403 挑战页）；
//   2) 音频地址在 POST /api/mapi/play 里，要求页面内 51.la SDK 现算的 sc/sp 签名头
//      （外部构造一律 {"status":404,"参数错误"}），只能让页面自己的 JS 去请求，
//      我们从 CDP 的网络事件里把响应接住。
//
// 为什么必须钉死 IPv4（重要）：
//   手机热点给了 IPv6，而 ting13 的 IPv6 入口（Cloudflare）一律回 520；
//   IPv4 入口 200 正常。所以这里先把域名解析成 IPv4，再用 --host-resolver-rules
//   钉住，Chrome 就不会走 IPv6 了。
//
// 章节分页：/tingdirs/<token>/<token>.html 每页 60 条，页签写着「1~60」「61 ~ 120」…
//   默认页是 1~60，第 k 页用 ?page=k&sort=asc。全部页取完合并去重就是全本。
//
// 用法：
//   node tools/fetch_t13.js                # 全量（断点续跑，缓存 tools/.t13cache）
//   node tools/fetch_t13.js --only=42266   # 只抓一本
//   node tools/fetch_t13.js --plan         # 只列每本书的总集数，不抓音频
//   node tools/fetch_t13.js --force        # 忽略缓存重抓
const fs = require('fs');
const path = require('path');
const os = require('os');
const dns = require('dns');
const { spawn } = require('child_process');

const HERE = __dirname;
const ROOT = path.dirname(HERE);
const SRC = path.join(HERE, '听书网源.json');
const OUT = path.join(HERE, '有声书源-听书网.json');
const CACHE = path.join(HERE, '.t13cache');
const PORT = Number(process.env.CDP_PORT || 9444);
const CHROME = process.env.CHROME || 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe';
const HOST = (process.env.T13_HOST || 'https://www.ting13.cc').replace(/\/+$/, '');
const PROXY = process.env.T13_PROXY || '';
const EXTRA_HOSTS = ['mp3hs.ting13.top', 'image.itingshu.net', 'static.ting13.cc'];

const WAIT = Number(process.env.WAIT || 12000);    // 等接口响应的上限
const GAP = Number(process.env.GAP || 2200);       // 每集之间歇一下（站点限流很紧，别催）
const PAGE_GAP = Number(process.env.PAGE_GAP || 3500);   // 章节页之间多等，站点会限流

const args = process.argv.slice(2);
const only = (args.find(a => a.startsWith('--only=')) || '').split('=')[1];
const force = args.includes('--force');
const planOnly = args.includes('--plan');

const sleep = ms => new Promise(r => setTimeout(r, ms));

/* ---------------- IPv4 解析 ---------------- */

function v4(host) {
  return new Promise(res => {
    dns.lookup(host, { family: 4, all: true }, (err, addrs) => {
      if (err || !addrs || !addrs.length) return res('');
      res(addrs[0].address);
    });
  });
}

async function hostRules() {
  const hosts = [HOST.replace(/^https?:\/\//, '').replace(/^www\./, ''), ...EXTRA_HOSTS];
  const uniq = [...new Set(hosts)];
  const rules = [];
  for (const h of uniq) {
    const ip = await v4(h) || await v4('www.' + h);
    if (!ip) continue;
    rules.push(`MAP ${h} ${ip}`);
    rules.push(`MAP www.${h} ${ip}`);
  }
  return rules.join(',');
}

/* ---------------- CDP ---------------- */

async function reachable() {
  try { return (await fetch(`http://127.0.0.1:${PORT}/json/version`)).ok; } catch (e) { return false; }
}

async function ensureChrome(rules) {
  if (await reachable()) return null;
  if (!fs.existsSync(CHROME)) throw new Error('找不到 Chrome：' + CHROME + '（可用 CHROME=... 指定）');
  const prof = path.join(os.tmpdir(), 't13-cdp-' + Date.now());
  const argv = ['--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run',
    '--no-default-browser-check', `--remote-debugging-port=${PORT}`, `--user-data-dir=${prof}`,
    PROXY ? ('--proxy-server=' + PROXY) : '--no-proxy-server'];
  if (rules) argv.push('--host-resolver-rules=' + rules);
  argv.push('about:blank');
  const p = spawn(CHROME, argv, { stdio: 'ignore', detached: false });
  for (let k = 0; k < 40; k++) { await sleep(500); if (await reachable()) return p; }
  throw new Error('Chrome 没起来');
}

async function pageWs() {
  for (let k = 0; k < 40; k++) {
    try {
      const j = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const p = j.find(x => x.type === 'page');
      if (p && p.webSocketDebuggerUrl) return p.webSocketDebuggerUrl;
    } catch (e) { }
    await sleep(500);
  }
  throw new Error('CDP 页面没就绪');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const pending = new Map(); const subs = []; let seq = 1;
    ws.addEventListener('message', ev => {
      let m; try { m = JSON.parse(typeof ev.data === 'string' ? ev.data : String(ev.data)); } catch (e) { return; }
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id); pending.delete(m.id);
        m.error ? p.reject(new Error(JSON.stringify(m.error))) : p.resolve(m.result);
      } else if (m.method) subs.forEach(f => f(m));
    });
    ws.addEventListener('open', () => resolve({
      send(method, params) {
        const id = seq++;
        return new Promise((res, rej) => { pending.set(id, { resolve: res, reject: rej }); ws.send(JSON.stringify({ id, method, params })); });
      },
      on(f) { subs.push(f); },
      close() { try { ws.close(); } catch (e) { } }
    }));
    ws.addEventListener('error', e => reject(new Error('ws: ' + (e.message || ''))));
  });
}

/* ---------------- Node 侧解析 ---------------- */

/** 专辑页标题 → 书名（去掉「_演播:xxx」「_免费听书…」「有声小说」这些后缀） */
function bookTitle(html) {
  const m = (html || '').match(/<title>([\s\S]*?)<\/title>/);
  let t = m ? m[1] : '';
  t = t.replace(/_免费听书[\s\S]*$/, '').replace(/_演播[:：][\s\S]*$/, '');
  return t.replace(/有声小说$/, '').trim();
}

/** 专辑页里的「全部章节」链接（带随机 token） */
function dirsUrl(html) {
  const m = (html || '').match(/href="(\/tingdirs\/[^"?]+)/);
  return m ? HOST + m[1] : '';
}

/** 页签上的分页范围，如 [[1,60],[61,120],[121,187]] */
function pageRanges(html) {
  const out = [];
  const re = /class="chapter-item[^"]*"[^>]*>\s*<a[^>]*>\s*(\d+)\s*~\s*(\d+)\s*<\/a>/g;
  let m;
  while ((m = re.exec(html || '')) !== null) out.push([Number(m[1]), Number(m[2])]);
  return out;
}

/** 只取播放列表区块里的章节（避开侧栏的「最近更新」） */
function chaptersOf(html, id) {
  const blk = (html || '').match(/<div class="playlist"\s+id="playlist"[\s\S]*?<\/ul>/);
  const seg = blk ? blk[0] : (html || '');
  const re = new RegExp('<a[^>]*href=[\'"](?:https?://[^/]+)?(/play/' + id + '_[^\'"]+)[\'"][^>]*>([\\s\\S]*?)</a>', 'g');
  const out = [];
  const seen = new Set();
  let m;
  while ((m = re.exec(seg)) !== null) {
    const t = m[2].replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();
    if (!t || seen.has(m[1])) continue;
    seen.add(m[1]);
    out.push({ t, u: HOST + m[1] });
  }
  return out;
}

/** 从标题里抠集号：第12章 / 第0060集 / 0007_xxx / 01集 */
function epIndex(title) {
  const s = String(title || '');
  let m = s.match(/第\s*0*(\d{1,4})\s*[章集回话期]/);
  if (m) return Number(m[1]);
  m = s.match(/^\s*0*(\d{1,4})\s*[_、.．\s]/);
  if (m) return Number(m[1]);
  m = s.match(/^\s*0*(\d{1,4})$/);
  if (m) return Number(m[1]);
  return 0;
}

/* ---------------- 主流程 ---------------- */

(async () => {
  const rules = await hostRules();
  console.log('IPv4 钉死：' + rules);
  const chrome = await ensureChrome(rules);
  const cdp = await connect(await pageWs());
  fs.mkdirSync(CACHE, { recursive: true });

  let apiBody = null;
  cdp.on(async m => {
    if (m.method !== 'Network.responseReceived') return;
    if (!/\/api\/mapi\/play/.test(m.params.response.url)) return;
    try {
      const b = await cdp.send('Network.getResponseBody', { requestId: m.params.requestId });
      apiBody = b.body || '';
    } catch (e) { }
  });

  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Network.enable');
  await cdp.send('Network.setBlockedURLs', {
    urls: ['*doubleclick.net*', '*googlesyndication*', '*adtrafficquality*',
      '*google.com/recaptcha*', '*criteo*', '*taboola*', '*outbrain*',
      '*hm.baidu.com*', '*zz.bdstatic.com*', '*cloudflareinsights*',
      '*.gif', '*.jpg', '*.png', '*.webp']
  });

  async function ev(expr) {
    const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error('页面异常：' + JSON.stringify(r.exceptionDetails).slice(0, 200));
    return r.result.value;
  }

  // 取一个页面的 HTML：真导航 + 读 outerHTML（页内 fetch 会被 WAF 挡）
  // 站点连着开几页就回「请求过于频繁」，所以失败一律按指数退避，不硬撞。
  async function navHtml(url, tries = 8) {
    let wait = 6000;
    for (let k = 0; k < tries; k++) {
      try { await cdp.send('Page.navigate', { url }); } catch (e) { }
      await sleep(2300);
      let title = '', html = '';
      try {
        title = await ev('document.title || ""');
        html = await ev('document.documentElement.outerHTML');
      } catch (e) { }
      const freq = /频繁|稍后|稍候|too many/i.test(title + (html || '').slice(0, 4000));
      const bad = freq || /520|Just a moment|Attention Required|Loading\.\.\./i.test(title) || (html || '').length < 4000;
      if (!bad) return html;
      console.log(`  [重试${freq ? '·限流' : ''}] ${String(title).slice(0, 36) || '空页面'}，等 ${Math.round(wait / 1000)}s`);
      await sleep(wait);
      wait = Math.min(Math.round(wait * 1.8), 90000);
    }
    return '';
  }

  const books = JSON.parse(fs.readFileSync(SRC, 'utf8')).filter(b => !only || b.id === only);
  if (!books.length) throw new Error('清单里没有匹配的书');

  const firstId = books[0].id;
  const firstAlb = await navHtml(`${HOST}/youshengxiaoshuo/${firstId}/`);
  if (!firstAlb) {
    console.log('站点打不开（连 IPv4 都不行），先把缓存留住');
    cdp.close();
    return;
  }
  console.log('已落到站点上\n');
  const plan = [];

  for (const bk of books) {
    const cf = path.join(CACHE, bk.id + '.json');
    let cache = fs.existsSync(cf) ? JSON.parse(fs.readFileSync(cf, 'utf8')) : { chapters: [], eps: [] };
    if (force) cache = { chapters: [], eps: [] };

    // ---- 章节表（分页取全量）----
    if (!cache.chapters.length) {
      const album = bk.id === firstId ? firstAlb : await navHtml(`${HOST}/youshengxiaoshuo/${bk.id}/`);
      if (!album) { console.log(`[${bk.name}] 专辑页取不到，跳过`); continue; }
      const dirUrl = dirsUrl(album);
      if (!dirUrl) { console.log(`[${bk.name}] 专辑页里没找到「全部章节」，跳过`); continue; }

      const p1 = await navHtml(dirUrl);
      if (!p1) { console.log(`[${bk.name}] 章节页取不到，跳过`); continue; }
      const ranges = pageRanges(p1);
      const total = ranges.length ? ranges[ranges.length - 1][1] : 0;
      const pages = ranges.length || 1;

      const map = new Map();       // url -> title
      const addAll = (html) => {
        for (const c of chaptersOf(html, bk.id)) if (!map.has(c.u)) map.set(c.u, c.t);
      };
      addAll(p1);
      for (let k = 2; k <= pages; k++) {
        const u = `${dirUrl}?page=${k}&sort=asc`;
        const html = await navHtml(u);
        if (!html) { console.log(`  第 ${k}/${pages} 页取不到`); continue; }
        addAll(html);
        process.stdout.write(`  章节 ${k}/${pages} 页，累计 ${map.size}/${total || '?'}\n`);
        await sleep(PAGE_GAP);
      }

      let list = [...map.entries()].map(([u, t]) => ({ u, t }));
      // 有集号就按集号排；没有就保持页面顺序（同书 URL id 递增＝集数递增）
      const withIdx = list.filter(x => epIndex(x.t) > 0).length;
      if (withIdx >= list.length * 0.8) {
        list.sort((a, b) => (epIndex(a.t) || 9e9) - (epIndex(b.t) || 9e9));
      } else {
        const num = u => Number((u.match(/_(\d+)\.html/) || [0, 1])[1]) || 0;
        list.sort((a, b) => num(a.u) - num(b.u));
      }
      cache.chapters = list;
      cache.name = bookTitle(album) || bk.name;
      cache.total = total;
      cache.eps = cache.eps || [];
      fs.writeFileSync(cf, JSON.stringify(cache, null, 1), 'utf8');
      console.log(`[${bk.name}] 章节 ${list.length} 条（标注共 ${total} 集）`);
    }

    plan.push({ name: bk.name, n: cache.chapters.length, total: cache.total || cache.chapters.length });

    if (planOnly) continue;

    // ---- 每集换直链（全量，不截断）----
    const want = cache.chapters;
    let done = 0;
    for (let i = 0; i < want.length; i++) {
      const ch = want[i];
      if (cache.eps[i] && cache.eps[i].url) { done++; continue; }
      let rec = { t: ch.t, u: ch.u, url: '', status: 0, name: '' };
      for (let attempt = 0; attempt < 5 && !rec.url; attempt++) {
        apiBody = null;
        try { await cdp.send('Page.navigate', { url: ch.u }); } catch (e) { }
        const t0 = Date.now();
        while (Date.now() - t0 < WAIT && !apiBody) await sleep(250);
        if (!apiBody) {
          try { await ev('(document.querySelector("#play,.loadimg,.jp-play")||{click(){}}).click()'); } catch (e) { }
          const t1 = Date.now();
          while (Date.now() - t1 < 4000 && !apiBody) await sleep(250);
        }
        if (!apiBody) {
          let title = '';
          try { title = await ev('document.title'); } catch (e) { }
          rec.status = -1;
          await sleep(/520|Just a moment|Loading/.test(title) ? 8000 : 3000);
          continue;
        }
        try {
          const j = JSON.parse(apiBody);
          rec.status = j.status;
          if (j.status === 200 && j.url) { rec.url = j.url; rec.name = j.name || ''; }
        } catch (e) { rec.status = -2; }
        if (!rec.url) await sleep(2000);
      }
      cache.eps[i] = rec;
      if (rec.url) done++;
      fs.writeFileSync(cf, JSON.stringify(cache, null, 1), 'utf8');
      if ((i + 1) % 10 === 0 || !rec.url) {
        console.log(`  ${rec.url ? 'OK  ' : 'FAIL'} ${bk.name} ${i + 1}/${want.length} 已经 ${done}`);
        if (!rec.url) console.log(`        ${rec.name || rec.t} status=${rec.status}`);
      }
      await sleep(GAP);
      if ((i + 1) % 30 === 0) await sleep(2500);
    }
    console.log(`[${bk.name}] 完成 ${cache.eps.filter(e => e && e.url).length}/${want.length} 集`);
  }

  if (planOnly) {
    console.log('\n书名                        章数   标注总集');
    let sum = 0;
    for (const p of plan) { sum += p.n; console.log(`${p.name.padEnd(26)} ${String(p.n).padStart(5)} ${String(p.total).padStart(8)}`); }
    console.log(`\n合计 ${plan.length} 部 / ${sum} 集`);
    cdp.close();
    if (chrome) { try { chrome.kill(); } catch (e) { } }
    return;
  }

  // 合成输出（支持 --only 只补一本）
  const all = JSON.parse(fs.readFileSync(SRC, 'utf8'));
  const final = [];
  for (const bk of all) {
    const cf = path.join(CACHE, bk.id + '.json');
    if (!fs.existsSync(cf)) continue;
    const c = JSON.parse(fs.readFileSync(cf, 'utf8'));
    const items = (c.eps || []).filter(e => e && e.url).map(e => ({ title: e.name || e.t, url: e.url }));
    if (items.length) final.push({ group: bk.group, name: bk.name, items });
  }
  if (final.length) {
    fs.writeFileSync(OUT, JSON.stringify(final, null, 1), 'utf8');
    const n = final.reduce((s, b) => s + b.items.length, 0);
    console.log(`\n写出 ${path.relative(ROOT, OUT)}：${final.length} 部书 / ${n} 集`);
  } else {
    console.log('\n没有解析出任何一集，未写文件');
  }

  cdp.close();
  if (chrome) { try { chrome.kill(); } catch (e) { } }
})().catch(e => {
  console.log('出错：' + (e && e.message ? e.message : e));
  process.exit(1);
});
