package com.myradio.app;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.Manifest;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.net.InetSocketAddress;
import java.net.Socket;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * MyRadio 手机端外壳。
 *
 * 设计前提：App 的本体是**收音机自己发的那份网页**（固件里的 webapp.h）。
 * 所以这里不做界面，只做三件事：
 *   1. 找机器（mDNS + 网段扫描 + 手输 IP）
 *   2. 把 WebView 指到 http://<机器IP>/，页面原样加载，功能跟浏览器里一模一样
 *   3. 够不着（不在同一个网）时，**自动**切到内置的 assets/remote.html —— 那份是纯 MQTT
 *      跨网遥控。1.42.0 起不再让用户点「离线遥控」按钮，App 自己判断（用户原话：
 *      「需要 App 就能操控收音机，不需要和收音机在一个网」）
 *
 * 零第三方依赖：Activity / WebView / NsdManager 全是系统 API。
 */
public class MainActivity extends Activity {
    private static final String TAG = "MR";
    private static final String HOME = "file:///android_asset/home.html";
    // 1.42.0：带 ?auto=1 —— 远程页那扇「选机器」的门只在线上一台时自动选掉，
    // 用户在 App 里不该为「点一下自家那台机器」再操作一次。
    private static final String REMOTE = "file:///android_asset/remote.html?auto=1";
    static final String APP_VER = "1.26.6";

    private WebView web;
    private SharedPreferences sp;
    private Discovery disc;
    private WifiManager.MulticastLock mcast;
    private String curUrl = HOME;
    private final Handler ui = new Handler(Looper.getMainLooper());
    /** 从首页主动点进来的才允许「连不上就回首页」，避免设备页自己跳转时被误踢 */
    private boolean expectDevice;
    private long deviceSince;
    /** 1.26.6：这次「进机器页」有没有真的加载成功（看门狗用） */
    private boolean pageLoaded;
    /** 1.26.6：已经退过一次，别在「机器页 ⇄ 远程页」之间来回弹 */
    private boolean fellBack;
    /** 1.26.6：进了机器页 6 秒还没加载完 —— 当它够不着，自己掐，不等 WebView 十几秒超时 */
    private final Runnable watchdog = new Runnable() {
        @Override public void run() {
            if (expectDevice && !pageLoaded) {
                Log.w(TAG, "device page timeout " + curUrl);
                fallback("这台机器现在够不着");
            }
        }
    };
    /** 配网（1.20.0）：App 自己连上收音机的常开热点，把家里的 WiFi 发进去 */
    private Provision prov;
    private static final int REQ_WIFI = 0x4D52;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        sp = getSharedPreferences("myradio", MODE_PRIVATE);
        prov = new Provision(this);

        Window w = getWindow();
        w.setStatusBarColor(0xFFE6E9EF);
        w.setNavigationBarColor(0xFFE6E9EF);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFFE6E9EF);

        web = new WebView(this);
        web.setLayoutParams(new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        web.setBackgroundColor(0xFFE6E9EF);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setAllowFileAccess(true);
        s.setAllowContentAccess(true);
        s.setLoadsImagesAutomatically(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setTextZoom(100);
        s.setCacheMode(WebSettings.LOAD_DEFAULT);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        // 页面是给 320x170 那块屏配的固定版式，用设备宽度别用 980 视口
        s.setUseWideViewPort(false);
        s.setLoadWithOverviewMode(false);
        String ua = s.getUserAgentString();
        if (ua != null && ua.indexOf("MyRadioApp") < 0) {
            s.setUserAgentString(ua + " MyRadioApp/" + APP_VER);
        }

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                if (u == null) return false;
                String sc = u.getScheme() == null ? "" : u.getScheme();
                if (sc.startsWith("http")) return false;    // 站内/局域网都在 App 里开
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, u));
                } catch (ActivityNotFoundException ignored) { }
                return true;
            }

            @Override
            public void onPageFinished(WebView v, String url) {
                curUrl = url == null ? curUrl : url;
                Log.i(TAG, "page " + curUrl);
                if (expectDevice && url != null && url.startsWith("http")) {
                    pageLoaded = true;
                    ui.removeCallbacks(watchdog);
                }
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                if (r == null || !r.isForMainFrame()) return;
                String url = r.getUrl() == null ? "" : r.getUrl().toString();
                if (!expectDevice || !url.startsWith("http")) return;
                Log.w(TAG, "page error " + url + " code=" + e.getErrorCode());
                // 1.26.6：**一律接管**，不再看「点进来几秒内」。
                //   用户 2026-11-05 报的那张「网页无法打开 · net::ERR_CONNECTION_TIMED_OUT」
                //   就是这么来的：老代码只在 deviceSince + 8000ms 内才做退路，而
                //   ERR_CONNECTION_TIMED_OUT 往往十几秒才出来 ⇒ 条件不成立 ⇒ 什么都不做
                //   ⇒ 人停在浏览器自带报错页，连个返回按钮都没有。
                fallback("这台机器现在够不着");
            }
        });

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(android.webkit.ConsoleMessage m) {
                Log.i(TAG, "js: " + m.message());
                return true;
            }
        });

        web.addJavascriptInterface(new Bridge(), "MRApp");
        root.addView(web);
        setContentView(root);

        acquireMulticast();

        disc = new Discovery(this, new Discovery.Cb() {
            @Override public void onFound(String ip, String name, String fw, String how) {
                js("window.__mrDev && window.__mrDev(" + JSONObject.quote(ip) + ","
                        + JSONObject.quote(name) + "," + JSONObject.quote(fw) + ","
                        + JSONObject.quote(how) + ")");
            }
            @Override public void onNote(String note) {
                js("window.__mrNote && window.__mrNote(" + JSONObject.quote(note) + ")");
            }
            @Override public void onDone(int n) {
                js("window.__mrDone && window.__mrDone(" + n + ")");
            }
        });

        // 1.42.0：开机先去试上次那台。
        //   连不上**不再退回首页**，而是直接进内置的跨网遥控（MQTT）——
        //   用户要的是「App 就能操控收音机，不需要和它一个网」，
        //   那就别让他先看到「找不到机器」再自己找按钮。
        //   从来没连过任何机器（lastIp 为空）时才留在首页，因为那时该先配网。
        // 1.26.6：开机不再「盲跳」上次那个地址。
        //   实测最常见的坑：上次是在机器热点里连的，lastIp 被存成了 192.168.4.1；
        //   回到家机器早连上路由器、热点关了 ⇒ 一开 App 就扑向 192.168.4.1，
        //   等十几秒超时，停在浏览器的「网页无法打开」上（用户发来的截图）。
        //   现在先 900ms 摸一下，够不着就直接进跨网遥控（MQTT），一步到位。
        String lastIp = sp.getString("lastIp", "");
        if (Provision.AP_IP.equals(lastIp)) lastIp = "";   // 老版本存进去的热点地址，作废
        if (b == null && lastIp.length() > 0) {
            openDevice(lastIp, sp.getString("lastName", "MyRadio"), false, false);
        } else {
            load(HOME);
        }
    }

    // ------------------------------------------------------------ 导航

    private void load(String url) {
        expectDevice = false;
        curUrl = url;
        web.loadUrl(url);
    }

    private void go(String url) {
        curUrl = url;
        web.loadUrl(url);
    }

    /**
     * 1.26.6：所有「进机器页」都走这里，两道闸：
     *   ① 先 900ms 摸一下 80 端口 —— 够不着就根本别进，省得停在浏览器那句话上；
     *   ② 进去了 6 秒还没加载完也当够不着 —— 有些网络里 SYN 被静默丢掉，
     *      WebView 要十几秒才报错，与其等它不如自己掐。
     *
     * @param stayOnFail true = 够不着就留在当前页（配网页那个「直接打开 192.168.4.1」用）
     */
    private void openDevice(final String ip, final String name,
                            final boolean doRemember, final boolean stayOnFail) {
        if (ip == null || ip.length() == 0) return;
        new Thread(new Runnable() {
            @Override public void run() {
                final boolean ok = reach(ip, 900);
                ui.post(new Runnable() {
                    @Override public void run() {
                        if (!ok) {
                            if (stayOnFail) {
                                toast("手机没连上「我的收音机」热点，先到系统 WiFi 里连它");
                                return;
                            }
                            fallback("这台机器现在够不着");
                            return;
                        }
                        if (doRemember) remember(ip, name == null || name.length() == 0 ? ip : name);
                        fellBack = false;
                        pageLoaded = false;
                        expectDevice = true;
                        deviceSince = System.currentTimeMillis();
                        go("http://" + ip + "/");
                        ui.removeCallbacks(watchdog);
                        ui.postDelayed(watchdog, 6000);
                    }
                });
            }
        }, "mr-reach").start();
    }

    /** 够不着这台机器：配过网就切跨网遥控（MQTT），没配过就回首页 */
    private void fallback(String why) {
        ui.removeCallbacks(watchdog);
        if (fellBack) return;
        fellBack = true;
        expectDevice = false;
        pageLoaded = false;
        if (sp.getString("lastIp", "").length() > 0 && !REMOTE.equals(curUrl)) {
            toast(why + "，改用远程控制");
            load(REMOTE);
        } else {
            toast(why + "，先在首页找机器");
            load(HOME);
        }
    }

    /** 900ms 内能不能连上 80 端口 */
    private static boolean reach(String host, int timeoutMs) {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(host, 80), timeoutMs);
            return true;
        } catch (Throwable t) {
            return false;
        } finally {
            try { s.close(); } catch (Throwable ignored) { }
        }
    }

    private void js(final String code) {
        ui.post(new Runnable() {
            @Override public void run() {
                try { web.evaluateJavascript(code, null); } catch (Throwable ignored) { }
            }
        });
    }

    private void toast(final String m) {
        ui.post(new Runnable() {
            @Override public void run() {
                Toast.makeText(MainActivity.this, m, Toast.LENGTH_SHORT).show();
            }
        });
    }

    @Override
    public void onBackPressed() {
        // 1.26.1：机器页 / 跨网遥控页就是 App 的主界面，返回键该退出 App。
        //   原来一律 load(HOME)，用户按一下就跳回「输 4 位码」那页 ——
        //   看着像被踢出登录（用户原话：「点系统返回键会回到刚进入的页面」）。
        if (HOME.equals(curUrl) && web.canGoBack()) {
            web.goBack();               // 只在首页内部（配网页 → 首页）才回退
            return;
        }
        super.onBackPressed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) web.onResume();
        // 从后台切回来时重扫一遍（Discovery 里有 1.5 秒冷却，和首页脚本的调用不会打架）
        if (disc != null && HOME.equals(curUrl)) disc.start();
    }

    @Override
    protected void onPause() {
        if (web != null) web.onPause();
        if (disc != null) disc.stop();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (disc != null) disc.shutdown();
        if (prov != null) prov.shutdown();
        releaseMulticast();
        if (web != null) {
            try {
                web.loadUrl("about:blank");
                web.destroy();
            } catch (Throwable ignored) { }
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------ 组播锁

    private void acquireMulticast() {
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm == null) return;
            mcast = wm.createMulticastLock("myradio");
            mcast.setReferenceCounted(false);
            mcast.acquire();
        } catch (Throwable t) {
            Log.w(TAG, "multicast " + t);
        }
    }

    private void releaseMulticast() {
        try {
            if (mcast != null && mcast.isHeld()) mcast.release();
        } catch (Throwable ignored) { }
        mcast = null;
    }

    // ------------------------------------------------------------ JS 桥

    class Bridge {
        @JavascriptInterface
        public String ver() { return APP_VER; }

        @JavascriptInterface
        public void scan() {
            ui.post(new Runnable() {
                @Override public void run() {
                    if (disc != null) disc.start();
                }
            });
        }

        /** 1.46.5：输 4 位手机连接码 —— 广播问一句，机器自己答它在哪 */
        @JavascriptInterface
        public void pinFind(final String code) {
            if (code == null) return;
            final String c = code.trim();
            if (c.length() != 4) return;
            PairFind.find(c, pairCb);
        }

        @JavascriptInterface
        public String recent() {
            JSONArray out = new JSONArray();
            try {
                JSONArray src = new JSONArray(sp.getString("recent", "[]"));
                for (int i = 0; i < src.length() && i < 8; i++) out.put(src.getJSONObject(i));
            } catch (Throwable ignored) { }
            return out.toString();
        }

        @JavascriptInterface
        public void open(final String ip, final String name) {
            if (ip == null || ip.length() == 0) return;
            ui.post(new Runnable() {
                @Override public void run() {
                    // 1.26.6：先探再进。列表里躺着的旧地址（换过 WiFi / 关了热点）
                    //   直接跳过去只会得到一张报错页。
                    openDevice(ip, name, true, false);
                }
            });
        }

        @JavascriptInterface
        public void home() {
            ui.post(new Runnable() {
                @Override public void run() { load(HOME); }
            });
        }

        @JavascriptInterface
        public void reload() {
            ui.post(new Runnable() {
                @Override public void run() { web.reload(); }
            });
        }

        @JavascriptInterface
        public void forget(final String ip) {
            ui.post(new Runnable() {
                @Override public void run() {
                    try {
                        JSONArray src = new JSONArray(sp.getString("recent", "[]"));
                        JSONArray out = new JSONArray();
                        for (int i = 0; i < src.length(); i++) {
                            JSONObject o = src.getJSONObject(i);
                            if (ip != null && ip.equals(o.optString("ip"))) continue;
                            out.put(o);
                        }
                        SharedPreferences.Editor e = sp.edit().putString("recent", out.toString());
                        if (ip != null && ip.equals(sp.getString("lastIp", ""))) e.remove("lastIp");
                        e.apply();
                    } catch (Throwable ignored) { }
                }
            });
        }

        @JavascriptInterface
        public void toastMsg(final String m) {
            if (m != null) toast(m);
        }

        @JavascriptInterface
        public String lastIp() {
            String v = sp.getString("lastIp", "");
            return Provision.AP_IP.equals(v) ? "" : v;   // 热点地址不算「这台机器的 IP」
        }

        // ---------------------------------------------------------- 配网（1.20.0）

        @JavascriptInterface
        public int sdk() { return Build.VERSION.SDK_INT; }

        @JavascriptInterface
        public String apInfo() {
            JSONObject o = new JSONObject();
            try {
                o.put("ssid", Provision.AP_SSID);
                o.put("pass", Provision.AP_PASS);
                o.put("ip", Provision.AP_IP);
                o.put("sdk", Build.VERSION.SDK_INT);
            } catch (Throwable ignored) { }
            return o.toString();
        }

        /** 1.42.0：不再「一键自动连热点」，改成探测「手机现在够不够得着机器」 */
        @JavascriptInterface
        public void provProbe() {
            ui.post(new Runnable() {
                @Override public void run() {
                    String p = wifiPerm();
                    if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                        requestPermissions(new String[]{p}, REQ_WIFI);   // 拿不到就当手输
                    }
                    prov.probe(sp.getString("lastIp", ""), provCb);
                }
            });
        }

        /** 跳到系统 WiFi 设置，让用户自己把手机连到 MyRadio 热点 */
        @JavascriptInterface
        public void openWifiSettings() {
            ui.post(new Runnable() {
                @Override public void run() {
                    try {
                        startActivity(new Intent(android.provider.Settings.ACTION_WIFI_SETTINGS)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                    } catch (Throwable t) {
                        toast("打不开系统 WiFi 设置，请手动打开设置 → WiFi");
                    }
                }
            });
        }

        /** 手机当前连的是哪个 WiFi（配网时预填用；热点模式下返回空） */
        @JavascriptInterface
        public String curSsid() {
            try {
                android.net.wifi.WifiManager wm =
                    (android.net.wifi.WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
                String v = wm == null ? null : wm.getConnectionInfo().getSSID();
                if (v == null) return "";
                v = v.trim();
                if (v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) {
                    v = v.substring(1, v.length() - 1);
                }
                return (v.equals("<unknown ssid>") || v.equals("0x")) ? "" : v;
            } catch (Throwable t) {
                return "";
            }
        }

        /** 已存的 WiFi 列表（最多 3 组） */
        @JavascriptInterface
        public void provSaved() { prov.savedList(provCb); }

        /** 直接进远程遥控（跨网）。不再是需要用户找的按钮，而是兜底入口 */
        @JavascriptInterface
        public void openRemote() {
            ui.post(new Runnable() {
                @Override public void run() { load(REMOTE); }
            });
        }

        @JavascriptInterface
        public void provSend(final String ssid, final String pass) {
            if (ssid == null || ssid.length() == 0) return;
            prov.send(ssid, pass, provCb);
        }

        /** 只想重扫手机附近的 WiFi（已经在收音机热点上时用） */
        @JavascriptInterface
        public void provRescan() {
            prov.rescan(provCb);
        }

        /** 1.42.0：不再自己切网络，所以「取消」没有网络要还原 —— 保留空实现兼容页面 */
        @JavascriptInterface
        public void provCancel() { }

        /** 不配网，直接把手机指的地址打开当遥控用（连着机器热点时就是 192.168.4.1） */
        @JavascriptInterface
        public void provDirect() {
            ui.post(new Runnable() {
                @Override public void run() {
                    // 1.26.6：热点没连上就别跳 —— 以前这里必然停在超时页
                    openDevice(Provision.AP_IP, "我的收音机", false, true);
                }
            });
        }

        /** 配网成功：等手机回到能上网的状态，再打开设备页 */
        @JavascriptInterface
        public void provFinish(final String ip) {
            ui.post(new Runnable() {
                @Override public void run() {
                    if (ip == null || ip.length() == 0) { load(HOME); return; }
                    toast("机器连上了，正在打开…");
                    // 别写死延时：有的手机从热点切回路由器要好几秒，
                    // 太早点开设备页会加载失败，然后被 onReceivedError 踹回首页。
                    new Thread(new Runnable() {
                        @Override public void run() {
                            waitOnline(14000);
                            ui.post(new Runnable() {
                                @Override public void run() {
                                    openDevice(ip, sp.getString("lastName", "MyRadio"), true, false);
                                }
                            });
                        }
                    }, "mr-waitonline").start();
                }
            });
        }

        /** 等手机真的回到一个能上网的网络（最多 ms 毫秒） */
        private void waitOnline(long ms) {
            long end = System.currentTimeMillis() + ms;
            while (System.currentTimeMillis() < end) {
                try {
                    android.net.ConnectivityManager cm =
                        (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                    android.net.Network n = cm == null ? null : cm.getActiveNetwork();
                    android.net.NetworkCapabilities c = n == null ? null : cm.getNetworkCapabilities(n);
                    if (c != null && c.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET)) {
                        Thread.sleep(600);      // 再给 DHCP/DNS 一点余量
                        return;
                    }
                } catch (Throwable ignored) { }
                try { Thread.sleep(400); } catch (InterruptedException e) { return; }
            }
        }
    }


    // ------------------------------------------------------------ 配网

    // 1.46.5：按 4 位码找机器的过程（广播 → 扫地址 → 结果）全部回给页面，
    //   页面就能显示「正在广播找…」→「广播没回应，逐个地址找…」→「找到 XXX」
    private final PairFind.Cb pairCb = new PairFind.Cb() {
        @Override public void stage(String s) {
            js("window.__mrPair && window.__mrPair({stage:" + JSONObject.quote(s) + "})");
        }
        @Override public void found(final String ip, final String name,
                                    final String fw, final String how) {
            js("window.__mrPair && window.__mrPair({ok:1,ip:" + JSONObject.quote(ip)
                    + ",name:" + JSONObject.quote(name)
                    + ",fw:" + JSONObject.quote(fw)
                    + ",how:" + JSONObject.quote(how) + "})");
            // 给页面 260ms 把「找到 XXX」画出来，再进机器页 —— 否则用户只看到闪一下
            ui.postDelayed(new Runnable() {
                @Override public void run() {
                    openDevice(ip, name, true, false);
                }
            }, 260);
        }
        @Override public void fail(String why) {
            js("window.__mrPair && window.__mrPair({err:" + JSONObject.quote(why) + "})");
        }
    };

    private final Provision.Cb provCb = new Provision.Cb() {
        @Override public void ev(JSONObject o) {
            js("window.__mrProv && window.__mrProv(" + o.toString() + ")");
        }
    };

    /** Android 13 起用「附近的 WiFi 设备」，更老的用定位权限（都是系统要求，只弹一次） */
    private String wifiPerm() {
        return Build.VERSION.SDK_INT >= 33
                ? "android.permission.NEARBY_WIFI_DEVICES"
                : Manifest.permission.ACCESS_FINE_LOCATION;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code != REQ_WIFI) { super.onRequestPermissionsResult(code, perms, res); return; }
        // 1.42.0：这个权限只用来「列出手机能扫到的 WiFi」。
        //   不给也能配网 —— 名字手打就行，所以失败时只提示、不中断。
        if (res == null || res.length == 0 || res[0] != PackageManager.PERMISSION_GRANTED) {
            js("window.__mrProv && window.__mrProv({stage:'noperm'})");
        }
    }

    private void remember(String ip, String name) {
        try {
            JSONArray src = new JSONArray(sp.getString("recent", "[]"));
            JSONArray out = new JSONArray();
            out.put(new JSONObject().put("ip", ip).put("name", name).put("at", System.currentTimeMillis()));
            for (int i = 0; i < src.length(); i++) {
                JSONObject o = src.getJSONObject(i);
                if (ip.equals(o.optString("ip"))) continue;
                out.put(o);
            }
            while (out.length() > 8) out.remove(out.length() - 1);
            sp.edit()
                    .putString("recent", out.toString())
                    // 1.26.6：热点地址（192.168.4.1）不能当「这台机器的 IP」存 ——
                    //   机器一连上家里的 WiFi 热点就关了，存下来只会让下次开机直奔超时页。
                    .putString("lastIp", Provision.AP_IP.equals(ip) ? "" : ip)
                    .putString("lastName", name)
                    .apply();
        } catch (Throwable t) {
            Log.w(TAG, "remember " + t);
        }
    }
}
