package kr.co.iefriends.pcsx2.activities;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;
import kr.co.iefriends.pcsx2.utils.DebugLog;

class RedumpDB {
    static class Result {
        String serial;
        String name;
    }

    private static final Object LOCK = new Object();
    private static Map<String, Result> sMd5SizeToResult = null;

    private static String md5ToLower(String s) { return s != null ? s.trim().toLowerCase() : null; }

    private static String externalResourcesPath(Context ctx) {
        File base = DataDirectoryManager.getDataRoot(ctx);
        return new File(base, "resources").getAbsolutePath();
    }

    private static void ensureLoaded(Context ctx) {
        if (sMd5SizeToResult != null) return;
        synchronized (LOCK) {
            if (sMd5SizeToResult != null) return;
            sMd5SizeToResult = new HashMap<>(8192);
            File f = new File(externalResourcesPath(ctx), "RedumpDatabase.yaml");
            BufferedReader br = null;
            try {
                InputStream in;
                if (f.exists()) {
                    in = new FileInputStream(f);
                } else {
                    try {
                        in = ctx.getAssets().open("resources/RedumpDatabase.yaml");
                    } catch (Exception e) {
                        try { DebugLog.w("Redump", "Database not found (assets and external)"); } catch (Throwable ignored) {}
                        return;
                    }
                }
                br = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
                String line;
                List<String[]> pendingHashes = new ArrayList<>(); // each [md5,size]
                String curSerial = null;
                String curName = null;
                String pendingMd5 = null;
                String pendingSize = null;
                while ((line = br.readLine()) != null) {
                    String t = line.trim();
                    if (t.isEmpty() || t.startsWith("#")) continue;
                    if (t.startsWith("- hashes:")) {
                        if (curSerial != null && !pendingHashes.isEmpty()) {
                            for (String[] hs : pendingHashes) {
                                String md5 = md5ToLower(hs[0]);
                                String size = hs[1] != null ? hs[1].trim() : null;
                                if (md5 != null && size != null) {
                                    Result r = new Result();
                                    r.serial = curSerial;
                                    r.name = (curName != null ? curName : curSerial);
                                    sMd5SizeToResult.put(md5 + "|" + size, r);
                                }
                            }
                        }
                        pendingHashes.clear();
                        curSerial = null;
                        curName = null;
                        pendingMd5 = null;
                        pendingSize = null;
                        continue;
                    }
                    if (t.startsWith("- md5:")) {
                        int idx = t.indexOf(':');
                        if (idx >= 0) { pendingMd5 = t.substring(idx + 1).trim(); }
                        continue;
                    }
                    if (t.startsWith("md5:")) {
                        int idx = t.indexOf(':');
                        if (idx >= 0) { pendingMd5 = t.substring(idx + 1).trim(); }
                        continue;
                    }
                    if (t.startsWith("size:")) {
                        int idx = t.indexOf(':');
                        if (idx >= 0) { pendingSize = t.substring(idx + 1).trim(); }
                        if (pendingMd5 != null && pendingSize != null) {
                            pendingHashes.add(new String[]{pendingMd5, pendingSize});
                            pendingMd5 = null;
                            pendingSize = null;
                        }
                        continue;
                    }
                    if (t.startsWith("serial:")) {
                        int idx = t.indexOf(':');
                        curSerial = (idx >= 0 ? t.substring(idx + 1).trim() : null);
                        continue;
                    }
                    if (t.startsWith("name:")) {
                        int idx = t.indexOf(':');
                        curName = (idx >= 0 ? t.substring(idx + 1).trim() : null);
                        continue;
                    }
                }
                if (curSerial != null && !pendingHashes.isEmpty()) {
                    for (String[] hs : pendingHashes) {
                        String md5 = md5ToLower(hs[0]);
                        String size = hs[1] != null ? hs[1].trim() : null;
                        if (md5 != null && size != null) {
                            Result r = new Result();
                            r.serial = curSerial;
                            r.name = (curName != null ? curName : curSerial);
                            sMd5SizeToResult.put(md5 + "|" + size, r);
                        }
                    }
                }
                pendingHashes.clear();
                try { DebugLog.i("Redump", "Loaded hash map entries: " + sMd5SizeToResult.size()); } catch (Throwable ignored) {}
            } catch (Exception ex) {
                try { DebugLog.e("Redump", "Failed to load DB: " + ex.getMessage()); } catch (Throwable ignored) {}
            } finally {
                if (br != null) try { br.close(); } catch (Exception ignored) {}
            }
        }
    }

    static Result lookupByFile(ContentResolver cr, Uri file) {
        Context ctx = NativeApp.getContext();
        if (ctx == null) return null;
        ensureLoaded(ctx);
        if (sMd5SizeToResult == null || sMd5SizeToResult.isEmpty()) return null;
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            long total = 0;
            final int BUF = 1024 * 1024;
            byte[] buf = new byte[BUF];
            try (InputStream in = CsoUtils.openInputStream(cr, file)) {
                if (in == null) return null;
                while (true) {
                    int r = in.read(buf);
                    if (r <= 0) break;
                    md.update(buf, 0, r);
                    total += r;
                }
            }
            String md5 = new BigInteger(1, md.digest()).toString(16);
            while (md5.length() < 32) md5 = "0" + md5;
            String key = md5ToLower(md5) + "|" + Long.toString(total);
            return sMd5SizeToResult.get(key);
        } catch (Exception ignored) {
            return null;
        }
    }
}
