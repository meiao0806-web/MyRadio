package com.myradio.app;

import android.util.Log;

import java.io.InputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 按「4 位手机连接码」找机器（1.46.5）。
 *
 * 为什么要这条路：
 *   mDNS 在很多路由器上直接被丢组播；/24 扫描在 /16 网段或开了 AP 隔离的网里
 *   永远扫不到 —— 用户看到的就是「App 连不上收音机」，而机器其实好好的。
 *   现在机器自己会在 UDP 8137 上答话：手机喊一句「谁是 1234」，它回自己的 IP。
 *   **广播一问一答，通常 0.2 秒出结果**，不依赖任何系统发现服务。
 *
 * 两条腿（和固件里的两种问法一一对应）：
 *   ① UDP 广播 255.255.255.255:8137 + 每块网卡的子网广播，发 "MR1?<码>"
 *      回 "MR1|1|<名字>|<版本>|<IP>|<设备码>"；码不对回 "MR1|0"
 *   ② HTTP 逐地址 GET /api/find?code=<码>（广播被路由丢掉时的兜底）
 *
 * 都是明文、都在局域网内、都不带任何密钥 —— 只回机器叫什么、在哪。
 */
class PairFind {
    private static final String TAG = "MR";
    static final int UDP_PORT = 8137;

    interface Cb {
        void stage(String s);
        void found(String ip, String name, String fw, String how);
        void fail(String why);
    }

    /** byUdp/byHttp 用这个哨兵表示「机器答话了，但码不对」 */
    private static final String[] BAD = new String[]{"!bad", "", ""};

    static void find(final String code, final Cb cb) {
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    String[] hit = byUdp(code, cb);
                    if (hit == BAD) { cb.fail("配对码不对 —— 看看收音机屏幕上那 4 位"); return; }
                    if (hit != null) { cb.found(hit[0], hit[1], hit[2], "udp"); return; }

                    cb.stage("广播没回应，逐个地址找…");
                    hit = byHttp(code, cb);
                    if (hit == BAD) { cb.fail("配对码不对 —— 看看收音机屏幕上那 4 位"); return; }
                    if (hit != null) { cb.found(hit[0], hit[1], hit[2], "http"); return; }

                    cb.fail("没找到这台机器：确认它开着、已经连上 WiFi、手机和它在同一个网");
                } catch (Throwable t) {
                    Log.w(TAG, "pin find " + t);
                    cb.fail("找机器时出错：" + t.getClass().getSimpleName());
                }
            }
        }, "mr-pin").start();
    }

    // ------------------------------------------------------------ ① 广播

    private static String[] byUdp(String code, Cb cb) {
        cb.stage("正在广播找 " + code + "…");
        DatagramSocket s = null;
        try {
            s = new DatagramSocket();
            s.setBroadcast(true);
            s.setSoTimeout(180);
            byte[] m = ("MR1?" + code).getBytes("UTF-8");
            List<InetAddress> where = broadcasts();
            for (int i = 0; i < where.size(); i++) {
                try { s.send(new DatagramPacket(m, m.length, where.get(i), UDP_PORT)); }
                catch (Throwable ignored) { }
            }
            byte[] buf = new byte[256];
            long end = System.currentTimeMillis() + 1400;
            while (System.currentTimeMillis() < end) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    s.receive(p);
                } catch (SocketTimeoutException te) {
                    continue;
                }
                String t = new String(p.getData(), 0, p.getLength(), "UTF-8").trim();
                if (!t.startsWith("MR1|")) continue;
                String[] f = t.split("\\|", -1);
                if (f.length < 2) continue;
                if ("1".equals(f[1])) {
                    String name = f.length > 2 && f[2].length() > 0 ? f[2] : "MyRadio";
                    String fw = f.length > 3 ? f[3] : "";
                    // 机器回的 IP 不一定可信：热点模式下它可能还没拿到 STA 地址（回 "-"）。
                    // 这时候以「这个包是从哪儿来的」为准 —— 那才是此刻真能连上的地址。
                    String ip = f.length > 4 ? f[4].trim() : "";
                    if (!isIPv4(ip)) ip = p.getAddress().getHostAddress();
                    return new String[]{ip, name, fw};
                }
                if ("0".equals(f[1])) return BAD;      // 它听见了，只是码不对
            }
            return null;
        } catch (Throwable t) {
            Log.w(TAG, "udp " + t);
            return null;
        } finally {
            if (s != null) try { s.close(); } catch (Throwable ignored) { }
        }
    }

    /** 255.255.255.255 + 每块网卡的子网广播地址（同网段里更可靠） */
    private static List<InetAddress> broadcasts() {
        Set<InetAddress> out = new LinkedHashSet<InetAddress>();
        try { out.add(InetAddress.getByName("255.255.255.255")); } catch (Throwable ignored) { }
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null) out.add(b);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "broadcasts " + t);
        }
        return new ArrayList<InetAddress>(out);
    }

    // ------------------------------------------------------------ ② 逐地址

    private static String[] byHttp(final String code, Cb cb) {
        List<String> prefixes = prefixes();
        if (prefixes.isEmpty()) return null;

        final AtomicBoolean stop = new AtomicBoolean(false);
        final String[] found = new String[3];
        int total = prefixes.size() * 254;
        final CountDownLatch done = new CountDownLatch(total);
        ExecutorService pool = Executors.newFixedThreadPool(48);
        for (final String p : prefixes) {
            for (int i = 1; i <= 254; i++) {
                final String ip = p + "." + i;
                pool.execute(new Runnable() {
                    @Override public void run() {
                        try {
                            if (stop.get()) return;
                            String[] r = probe(ip, code);
                            if (r == BAD) {
                                if (stop.compareAndSet(false, true)) {
                                    found[0] = "!bad"; found[1] = ""; found[2] = "";
                                }
                                return;
                            }
                            if (r != null && stop.compareAndSet(false, true)) {
                                found[0] = r[0]; found[1] = r[1]; found[2] = r[2];
                            }
                        } finally {
                            done.countDown();
                        }
                    }
                });
            }
        }
        try { done.await(9, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
        stop.set(true);
        pool.shutdownNow();
        if (found[0] == null) return null;
        if ("!bad".equals(found[0])) return BAD;
        return found;
    }

    private static String[] probe(String ip, String code) {
        String body = get("http://" + ip + "/api/find?code=" + code, 700);
        if (body == null) return null;
        if (body.indexOf("\"ok\":1") < 0) {
            return body.indexOf("\"bad\":1") >= 0 ? BAD : null;
        }
        String name = field(body, "name");
        String fw = field(body, "fw");
        String rip = field(body, "ip");
        if (!isIPv4(rip)) rip = ip;          // 同上：地址不可信就用「我刚问的是谁」
        return new String[]{rip, name == null || name.length() == 0 ? "MyRadio" : name,
                fw == null ? "" : fw};
    }

    /** 只认 4 段数字，别把 "-" / 空串 / IPv6 当成能连的地址 */
    private static boolean isIPv4(String s) {
        if (s == null) return false;
        String[] p = s.trim().split("\\.");
        if (p.length != 4) return false;
        for (int i = 0; i < 4; i++) {
            String x = p[i];
            if (x.length() == 0 || x.length() > 3) return false;
            for (int j = 0; j < x.length(); j++) if (x.charAt(j) < '0' || x.charAt(j) > '9') return false;
            if (Integer.parseInt(x) > 255) return false;
        }
        return true;
    }

    private static String get(String url, int timeout) {
        InputStream in = null;
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection(java.net.Proxy.NO_PROXY);
            c.setConnectTimeout(timeout);
            c.setReadTimeout(timeout);
            c.setUseCaches(false);
            c.setRequestProperty("Accept", "application/json");
            if (c.getResponseCode() != 200) return null;
            in = c.getInputStream();
            byte[] buf = new byte[1024];
            StringBuilder sb = new StringBuilder();
            int n;
            while ((n = in.read(buf)) > 0) {
                sb.append(new String(buf, 0, n, "UTF-8"));
                if (sb.length() > 4096) break;
            }
            return sb.toString();
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) { }
        }
    }

    private static String field(String js, String key) {
        String k = "\"" + key + "\":\"";
        int i = js.indexOf(k);
        if (i < 0) return "";
        int j = js.indexOf('"', i + k.length());
        if (j < 0) return "";
        return js.substring(i + k.length(), j);
    }

    /** 本机所有 site-local IPv4 的前三段 */
    private static List<String> prefixes() {
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
}
