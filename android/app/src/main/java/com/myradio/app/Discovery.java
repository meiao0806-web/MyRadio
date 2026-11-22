package com.myradio.app;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * 在局域网里找烧了 MyRadio 固件的机器。
 *
 * 两条腿并行：
 *  1) mDNS —— 固件里做了 MDNS.begin("myradio") + addService("http","tcp",80)，
 *     正常情况下秒级就能看见。某些路由器丢组播、或手机系统把 NsdManager 玩坏，会哑火。
 *  2) /24 端口扫描 —— 拿本机 IP 前三段，并发探 80 端口，通了再 GET /api/info 认一下
 *     是不是 MyRadio（防止把路由器管理页当机器）。约 3 秒。
 *
 * 两个都只做「发现」，不缓存；缓存交给 SharedPreferences。
 */
class Discovery {
    private static final String TAG = "MR";

    interface Cb {
        void onFound(String ip, String name, String fw, String how);
        void onNote(String note);
        void onDone(int total);
    }

    private final Context ctx;
    private final Cb cb;
    private final Handler main = new Handler(Looper.getMainLooper());
    /** ip -> name，去重 + 顺序稳定，避免同一个机器被 mDNS 和扫描各报一次 */
    private final Set<String> seen = Collections.synchronizedSet(new LinkedHashSet<String>());

    private NsdManager nsd;
    private NsdManager.DiscoveryListener dl;
    private final ExecutorService resolver = Executors.newSingleThreadExecutor();
    private final ExecutorService pool = Executors.newFixedThreadPool(32);
    private volatile boolean running;
    private long lastStart;
    private int mdnsHits;

    Discovery(Context c, Cb cb) {
        this.ctx = c.getApplicationContext();
        this.cb = cb;
    }

    void start() {
        // 外壳 onResume 和首页脚本都会调 start，1.5 秒内重复的直接忽略，
        // 否则两边互相 stop，mDNS 那一路刚发现一半就被掐掉。
        long now = System.currentTimeMillis();
        if (running && now - lastStart < 1500) return;
        lastStart = now;

        stop();
        seen.clear();
        mdnsHits = 0;
        running = true;
        main.post(new Runnable() {
            @Override public void run() {
                note("正在找机器…");
                startMdns();
            }
        });
        new Thread(new Runnable() {
            @Override public void run() {
                sleep(900);                 // 先给 mDNS 一拍机会，别跟扫描抢带宽
                if (!running) return;
                scan();
                if (!running) return;
                final int n = seen.size();
                main.post(new Runnable() {
                    @Override public void run() {
                        if (n == 0 && mdnsHits == 0) {
                            note("一台也没找到 —— 确认手机连的是同一个 WiFi");
                        } else {
                            note("还在后台继续等它报到…");
                        }
                        cb.onDone(n);
                    }
                });
            }
        }, "mr-scan").start();
    }

    void stop() {
        running = false;
        final NsdManager.DiscoveryListener l = dl;
        final NsdManager m = nsd;
        dl = null;
        nsd = null;
        if (l != null && m != null) {
            main.post(new Runnable() {
                @Override public void run() {
                    try { m.stopServiceDiscovery(l); } catch (Throwable ignored) { }
                }
            });
        }
    }

    void shutdown() {
        stop();
        try { resolver.shutdownNow(); } catch (Throwable ignored) { }
        try { pool.shutdownNow(); } catch (Throwable ignored) { }
    }

    // ------------------------------------------------------------ mDNS

    private void startMdns() {
        try {
            if (nsd == null) nsd = (NsdManager) ctx.getSystemService(Context.NSD_SERVICE);
            if (nsd == null) return;
            dl = new NsdManager.DiscoveryListener() {
                @Override public void onStartDiscoveryFailed(String t, int e) {
                    Log.w(TAG, "mdns start failed " + t + " " + e);
                    note("这台机器的系统不给用组播，改成逐个网段找…");
                }
                @Override public void onStopDiscoveryFailed(String t, int e) { }
                @Override public void onDiscoveryStarted(String t) { Log.i(TAG, "mdns on " + t); }
                @Override public void onDiscoveryStopped(String t) { }
                @Override public void onServiceLost(NsdServiceInfo info) { }
                @Override public void onServiceFound(final NsdServiceInfo info) {
                    // ESP32 的实例名就是 hostname：myradio
                    String sn = info.getServiceName() == null ? "" : info.getServiceName().toLowerCase();
                    if (!sn.contains("myradio")) return;
                    postResolve(info);
                }
            };
            nsd.discoverServices("_http._tcp.", NsdManager.PROTOCOL_DNS_SD, dl);
        } catch (Throwable t) {
            Log.w(TAG, "mdns " + t);
        }
    }

    /** resolveService 不能并发，串到一个单线程队列里。 */
    private void postResolve(final NsdServiceInfo info) {
        if (!running) return;
        resolver.execute(new Runnable() {
            @Override public void run() {
                if (!running) return;
                try {
                    nsd.resolveService(info, new NsdManager.ResolveListener() {
                        @Override public void onResolveFailed(NsdServiceInfo i, int e) {
                            Log.w(TAG, "mdns resolve fail " + e);
                        }
                        @Override public void onServiceResolved(NsdServiceInfo i) {
                            String ip = hostOf(i);
                            if (ip == null || ip.isEmpty()) return;
                            mdnsHits++;
                            String nm = i.getServiceName();
                            report(ip, nm == null || nm.isEmpty() ? "MyRadio" : nm, "", "mdns");
                        }
                    });
                } catch (Throwable t) {
                    Log.w(TAG, "resolve " + t);
                }
            }
        });
    }

    @SuppressWarnings("deprecation")
    private static String hostOf(NsdServiceInfo i) {
        try {
            InetAddress a = i.getHost();
            if (a != null && a.getHostAddress() != null) return a.getHostAddress();
        } catch (Throwable ignored) { }
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                List<InetAddress> all = i.getHostAddresses();
                if (all != null && !all.isEmpty() && all.get(0) != null) {
                    return all.get(0).getHostAddress();
                }
            }
        } catch (Throwable ignored) { }
        return "";
    }

    // ------------------------------------------------------------ 端口扫描

    private void scan() {
        List<String> prefixes = localPrefixes();
        if (prefixes.isEmpty()) {
            note("没拿到本机局域网地址 —— 是不是没连 WiFi？");
            return;
        }
        final int total = prefixes.size() * 254;
        final CountDownLatch done = new CountDownLatch(total);
        for (final String p : prefixes) {
            for (int i = 1; i <= 254; i++) {
                final String ip = p + "." + i;
                pool.execute(new Runnable() {
                    @Override public void run() {
                        try {
                            if (running) probe(ip);
                        } finally {
                            done.countDown();
                        }
                    }
                });
            }
        }
        try { done.await(25, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
    }

    private void probe(String ip) {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(ip, 80), 350);
        } catch (Throwable t) {
            return;
        } finally {
            try { s.close(); } catch (Throwable ignored) { }
        }
        // 80 通了不代表是收音机（可能有路由器/打印机），读一下 /api/info 认人
        String body = get("http://" + ip + "/api/info", 900);
        if (body == null) return;
        if (body.indexOf("MyRadio") < 0 && body.indexOf("\"fw\"") < 0) return;
        String name = field(body, "name");
        if (name == null || name.isEmpty()) name = "MyRadio";
        if (name.indexOf("MyRadio") < 0) name = "MyRadio · " + ip;
        report(ip, name, field(body, "fw"), "scan");
    }

    private static String get(String url, int timeout) {
        InputStream in = null;
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(timeout);
            c.setReadTimeout(timeout);
            c.setUseCaches(false);
            c.setRequestProperty("Accept", "application/json");
            if (c.getResponseCode() != 200) return null;
            in = c.getInputStream();
            byte[] buf = new byte[4096];
            StringBuilder sb = new StringBuilder();
            int n;
            while ((n = in.read(buf)) > 0) {
                sb.append(new String(buf, 0, n, "UTF-8"));
                if (sb.length() > 8192) break;
            }
            return sb.toString();
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) { }
        }
    }

    /** 极简 JSON 取字符串字段，够用就行，不为这个引 org.json 的树。 */
    private static String field(String js, String key) {
        String k = "\"" + key + "\":\"";
        int i = js.indexOf(k);
        if (i < 0) return "";
        int j = js.indexOf('"', i + k.length());
        if (j < 0) return "";
        return js.substring(i + k.length(), j);
    }

    /** 本机所有 site-local IPv4 的前三段。 */
    private List<String> localPrefixes() {
        Set<String> out = new LinkedHashSet<String>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (!(a instanceof Inet4Address)) continue;
                    if (!a.isSiteLocalAddress()) continue;
                    String h = a.getHostAddress();
                    int d = h.lastIndexOf('.');
                    if (d > 0) out.add(h.substring(0, d));
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "ifaces " + t);
        }
        return new ArrayList<String>(out);
    }

    // ------------------------------------------------------------

    private void report(final String ip, final String name, final String fw, final String how) {
        if (!seen.add(ip)) return;
        main.post(new Runnable() {
            @Override public void run() {
                cb.onFound(ip, name, fw == null ? "" : fw, how);
            }
        });
    }

    private void note(final String s) {
        main.post(new Runnable() {
            @Override public void run() {
                cb.onNote(s);
            }
        });
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ignored) { }
    }
}
