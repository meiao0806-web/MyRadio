package com.myradio.app;

import android.content.Context;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 给收音机配网 —— **App 里填表，不自动跳去连热点**。
 *
 * 1.42.0 重写。上一版（1.20.x）点「一键配网」会用 WifiNetworkSpecifier 自己切到
 * MyRadio 热点，用户反馈很直接：「收音机不发热点，怎么会和手机在一个 WiFi 下」。
 * 那个设计把「切换网络」藏在了按钮后面，用户不知道手机被抢走了 WiFi，
 * 而且一旦机器热点真没起来，整个流程就卡在第一步。
 *
 * 现在拆成两件事，都由用户看得见的操作驱动：
 *
 *   probe()  → 「手机现在够不够得着机器」。够得着（连着 MyRadio 热点，或和它同一个
 *              局域网）就回 ready + 机器信息；够不着就回 need-net，界面提示
 *              「去系统设置把 WiFi 连到 MyRadio」，并给一个跳转按钮。
 *   send()   → 把用户填的 WiFi 名字 + 密码 POST 给机器。**家里 WiFi 和手机热点
 *              用的是同一个接口**（/api/wifi），所以「让机器连我手机的热点」
 *              就是在这里填手机热点的名字和密码。
 *
 * 为什么首次配网绕不开机器热点：机器没连上任何网络时，手机上没有任何 IP 能到达它，
 * 这是网络本身的限制（不是实现选择）。固件把这个热点刻意留着：名字「我的收音机」、
 * **没有密码**（1.46.8 起，点一下就上）、地址 192.168.4.1，连上会自动弹配网页。
 */
class Provision {
    // 1.46.8：热点改名「我的收音机」，而且**去掉了密码**（开放热点，点一下就连）。
    //   两个名字都认：新固件广播「我的收音机」，1.46.7 及以前广播 MyRadio ——
    //   App 可能比固件新（用户先装 App 再烧机器），只认一个就会出现
    //   「明明连着热点，App 却说够不着机器」这种最难查的问题。
    static final String AP_SSID = "我的收音机";   // 必须和 src/config.h 的 AP_SSID 一致
    static final String AP_SSID_OLD = "MyRadio";  // 老固件的名字，也要认
    static final String AP_PASS = "";             // 开放热点：没有密码
    static final String AP_BASE = "http://192.168.4.1";
    static final String AP_IP = "192.168.4.1";

    interface Cb { void ev(JSONObject o); }

    private final Context ctx;
    private final WifiManager wm;
    private final ExecutorService ex = Executors.newSingleThreadExecutor();
    private volatile String base = "";     // 当前可达的地址（"" = 够不着）
    private volatile String token = "";
    private volatile String via = "";      // "ap" = 机器热点；"lan" = 同一局域网

    Provision(Context c) {
        ctx = c.getApplicationContext();
        wm = (WifiManager) ctx.getSystemService(Context.WIFI_SERVICE);
    }

    // ------------------------------------------------------------ 对外

    /** 手机现在够不够得着机器 */
    void probe(final String lastIp, final Cb cb) {
        ex.execute(new Runnable() {
            @Override public void run() {
                JSONObject o = new JSONObject();
                try {
                    String hit = findBase(lastIp);
                    o.put("stage", hit.isEmpty() ? "need-net" : "ready");
                    o.put("apSsid", AP_SSID);
                    o.put("apPass", AP_PASS);
                    o.put("apIp", AP_IP);
                    o.put("homeSsid", currentSsid());     // 手机当前连的 WiFi（用来预填）
                    if (hit.isEmpty()) {
                        o.put("nets", scanPhone(1200));   // 顺手带一份手机扫到的
                        cb.ev(o);
                        return;
                    }
                    base = hit;
                    JSONObject info = json(http(hit + "/api/info", null, 2500));
                    if (info == null) {
                        base = ""; via = "";
                        o.put("stage", "need-net");
                        o.put("nets", scanPhone(1200));
                        cb.ev(o);
                        return;
                    }
                    JSONObject a = json(http(hit + "/api/auth", null, 3000));
                    token = a == null ? "" : a.optString("token", "");
                    if (token.length() == 0) {
                        o.put("stage", "locked");
                        cb.ev(o);
                        return;
                    }
                    o.put("name", info.optString("name"));
                    o.put("fw", info.optString("fw"));
                    o.put("devid", info.optString("devid"));
                    o.put("rcode", info.optString("rcode"));
                    o.put("dcode", info.optString("dcode"));
                    o.put("needSetup", info.optInt("needSetup", 0));
                    o.put("ip", info.optString("ip"));
                    o.put("via", via);
                    o.put("base", base);
                    o.put("nets", scanPhone(1500));
                    cb.ev(o);
                } catch (Throwable t) {
                    JSONObject f = stage("need-net");
                    try { f.put("msg", "探测出错：" + t); } catch (Throwable ignored) { }
                    cb.ev(f);
                }
            }
        });
    }

    /** 把用户填的 WiFi（家里路由器 or 手机热点，同一个接口）发给机器 */
    void send(final String ssid, final String pass, final Cb cb) {
        ex.execute(new Runnable() {
            @Override public void run() {
                try {
                    if (base.isEmpty()) {
                        cb.ev(fail("手机现在够不着机器，先在系统 WiFi 里连一下「" + AP_SSID + "」"));
                        return;
                    }
                    JSONObject o = stage("sending");
                    o.put("ssid", ssid);
                    cb.ev(o);

                    String body = "ssid=" + enc(ssid) + "&pass=" + enc(pass == null ? "" : pass);
                    if (http(base + "/api/wifi", body, 8000) == null) {
                        cb.ev(fail("机器没收到（连接断了）。再试一次。"));
                        return;
                    }
                    for (int i = 0; i < 32; i++) {
                        sleep(1000);
                        JSONObject st = json(http(base + "/api/wifi/status", null, 2500));
                        if (st == null) continue;
                        if (st.optInt("ok") == 1) {
                            JSONObject r = stage("ok");
                            r.put("ssid", st.optString("ssid", ssid));
                            r.put("ip", st.optString("ip"));
                            cb.ev(r);
                            return;
                        }
                        if (st.optInt("state") == 3) {
                            cb.ev(fail(st.optString("msg", "连不上这个 WiFi")));
                            return;
                        }
                    }
                    cb.ev(fail("30 秒没连上 —— 多半是密码不对，或者那是 5G 网络（收音机只认 2.4G）。"));
                } catch (Throwable t) {
                    cb.ev(fail("配网出错：" + t));
                }
            }
        });
    }

    /** 只重扫手机附近的 WiFi */
    void rescan(final Cb cb) {
        ex.execute(new Runnable() {
            @Override public void run() {
                JSONObject o = stage("nets");
                try { o.put("nets", scanPhone(4200)); } catch (Throwable ignored) { }
                cb.ev(o);
            }
        });
    }

    /** 已存的 WiFi（最多 3 组） */
    void savedList(final Cb cb) {
        ex.execute(new Runnable() {
            @Override public void run() {
                if (base.isEmpty()) { cb.ev(fail("手机现在够不着机器")); return; }
                JSONObject j = json(http(base + "/api/wifi/list", null, 3000));
                JSONObject o = stage("saved");
                try {
                    o.put("n", j == null ? 0 : j.optInt("n"));
                    o.put("list", j == null ? new JSONArray() : j.optJSONArray("list"));
                } catch (Throwable ignored) { }
                cb.ev(o);
            }
        });
    }

    void shutdown() {
        try { ex.shutdownNow(); } catch (Throwable ignored) { }
    }

    // ------------------------------------------------------------ 探测

    /** 依次试：机器热点 → 上次用的 IP → myradio.local。返回可达的 base（""=没有） */
    private String findBase(String lastIp) {
        if (reach(AP_IP, 900)) { via = "ap"; return AP_BASE; }
        if (lastIp != null && lastIp.length() > 0 && !lastIp.equals(AP_IP) && reach(lastIp, 900)) {
            via = "lan";
            return "http://" + lastIp;
        }
        if (reach("myradio.local", 1200)) { via = "lan"; return "http://myradio.local"; }
        via = "";
        return "";
    }

    private boolean reach(String host, int timeout) {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(host, 80), timeout);
            return true;
        } catch (Throwable t) {
            return false;
        } finally {
            try { s.close(); } catch (Throwable ignored) { }
        }
    }

    /** 手机此刻连着哪个 WiFi（用来预填「家里的 WiFi 名字」） */
    @SuppressWarnings("deprecation")
    private String currentSsid() {
        try {
            WifiInfo wi = wm == null ? null : wm.getConnectionInfo();
            String s = wi == null ? null : wi.getSSID();
            if (s == null) return "";
            s = s.trim();
            if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) s = s.substring(1, s.length() - 1);
            if (s.equals("<unknown ssid>") || s.equals("0x")
                    || s.equals(AP_SSID) || s.equals(AP_SSID_OLD)) return "";
            return s;
        } catch (Throwable t) {
            return "";
        }
    }

    /** 手机自己扫附近 WiFi（拿不到就空数组，界面会退化成手输） */
    @SuppressWarnings("deprecation")
    private JSONArray scanPhone(long budgetMs) {
        Set<String> seen = new LinkedHashSet<String>();
        List<String[]> rows = new ArrayList<String[]>();   // {ssid, rssi, secured}
        long end = System.currentTimeMillis() + budgetMs;
        try {
            if (wm != null) {
                wm.startScan();
                while (System.currentTimeMillis() < end) {
                    List<android.net.wifi.ScanResult> rs = wm.getScanResults();
                    if (rs != null && !rs.isEmpty()) {
                        for (android.net.wifi.ScanResult r : rs) {
                            if (r == null || r.SSID == null) continue;
                            String s = r.SSID;
                            if (s.length() == 0 || seen.contains(s)) continue;
                            seen.add(s);
                            rows.add(new String[]{s, String.valueOf(r.level),
                                    String.valueOf(r.capabilities != null && r.capabilities.contains("WPA"))});
                        }
                        if (rows.size() >= 3) break;
                    }
                    Thread.sleep(400);
                }
            }
        } catch (Throwable ignored) { }

        Collections.sort(rows, new Comparator<String[]>() {
            @Override public int compare(String[] a, String[] b) {
                return Integer.parseInt(b[1]) - Integer.parseInt(a[1]);
            }
        });

        JSONArray arr = new JSONArray();
        for (int i = 0; i < rows.size() && i < 16; i++) {
            try {
                JSONObject o = new JSONObject();
                o.put("s", rows.get(i)[0]);
                o.put("r", Integer.parseInt(rows.get(i)[1]));
                o.put("sec", rows.get(i)[2].equals("true") ? 1 : 0);
                arr.put(o);
            } catch (Throwable ignored) { }
        }
        return arr;
    }

    // ------------------------------------------------------------ HTTP

    private String http(String url, String form, int timeout) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(Math.min(timeout, 6000));
            c.setReadTimeout(timeout);
            c.setUseCaches(false);
            c.setRequestProperty("Accept", "application/json");
            if (token.length() > 0) c.setRequestProperty("X-Token", token);
            if (form != null) {
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                OutputStream os = c.getOutputStream();
                os.write(form.getBytes("UTF-8"));
                os.close();
            }
            int code = c.getResponseCode();
            InputStream in = (code >= 200 && code < 400) ? c.getInputStream() : c.getErrorStream();
            if (in == null) return null;
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) > 0) {
                bo.write(buf, 0, n);
                if (bo.size() > 32000) break;
            }
            in.close();
            return new String(bo.toByteArray(), "UTF-8");
        } catch (Throwable t) {
            return null;
        } finally {
            if (c != null) { try { c.disconnect(); } catch (Throwable ignored) { } }
        }
    }

    private static String enc(String s) {
        try { return URLEncoder.encode(s == null ? "" : s, "UTF-8"); }
        catch (Throwable t) { return ""; }
    }

    private static JSONObject json(String s) {
        if (s == null) return null;
        try { return new JSONObject(s); } catch (Throwable t) { return null; }
    }

    private static JSONObject stage(String s) {
        JSONObject o = new JSONObject();
        try { o.put("stage", s); } catch (Throwable ignored) { }
        return o;
    }

    private static JSONObject fail(String msg) {
        JSONObject o = stage("fail");
        try { o.put("msg", msg); } catch (Throwable ignored) { }
        return o;
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ignored) { }
    }

    static int sdk() { return Build.VERSION.SDK_INT; }
}
