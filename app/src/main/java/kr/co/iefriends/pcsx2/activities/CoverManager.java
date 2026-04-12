package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.content.DialogInterface;
import android.net.Uri;
import android.text.TextUtils;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.core.util.Pair;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;
import kr.co.iefriends.pcsx2.utils.DebugLog;

class CoverManager {
    private static final String PREF_COVERS_URL = "covers_url_template";
    static final String PREF_MANUAL_COVER_PREFIX = "manual_cover:";

    private final MainActivity host;
    private final Object coverPrefetchLock = new Object();
    private boolean coverPrefetchRunning;

    CoverManager(MainActivity host) {
        this.host = host;
    }

    // region Covers

    String getCoversUrlTemplate() {
        return host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).getString(PREF_COVERS_URL, "");
    }

    void setCoversUrlTemplate(String s) {
        host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).edit().putString(PREF_COVERS_URL, s == null ? "" : s).apply();
    }

    String getManualCoverUri(String gameKey) {
        try { return host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).getString(PREF_MANUAL_COVER_PREFIX + gameKey, null); } catch (Throwable ignored) { return null; }
    }

    void setManualCoverUri(String gameKey, String uri) {
        try {
            host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).edit().putString(PREF_MANUAL_COVER_PREFIX + gameKey, uri).apply();
            GamesAdapter.clearLocalCoverCache();
        } catch (Throwable ignored) {}
    }

    void removeManualCoverUri(String gameKey) {
        try {
            host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).edit().remove(PREF_MANUAL_COVER_PREFIX + gameKey).apply();
            GamesAdapter.clearLocalCoverCache();
        } catch (Throwable ignored) {}
    }

    void promptForCoversUrl() {
        View dialogView = LayoutInflater.from(host).inflate(R.layout.dialog_cover_template, null);
        TextInputLayout inputLayout = dialogView.findViewById(R.id.input_layout_cover_template);
        TextInputEditText input = dialogView.findViewById(R.id.input_cover_template);
        String previous = getCoversUrlTemplate();
        if (previous != null && input != null) {
            input.setText(previous);
            input.setSelection(previous.length());
        }

        MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(host)
                .setTitle(R.string.cover_template_dialog_title)
                .setView(dialogView)
                .setNegativeButton(android.R.string.cancel, (d, which) -> d.dismiss())
                .setPositiveButton(R.string.action_save, null);

        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(dlg -> {
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(v -> {
                String value = "";
                if (input != null && input.getText() != null) {
                    value = input.getText().toString().trim();
                }
                setCoversUrlTemplate(value);
                try { Toast.makeText(host, R.string.cover_template_saved_toast, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                if (host.gamesFolderUri != null) {
                    host.scanGamesFolder(host.gamesFolderUri);
                }
                dialog.dismiss();
                if (!TextUtils.isEmpty(value) && !TextUtils.equals(previous, value)) {
                    prefetchCoversAsync(value);
                }
            });
        });
        dialog.show();
        if (inputLayout != null) {
            inputLayout.requestFocus();
        }
    }

    void prefetchCoversAsync(String template) {
        if (TextUtils.isEmpty(template)) {
            return;
        }
        LinkedHashSet<Uri> roots = collectGameRootUris();
        GamesAdapter.clearLocalCoverCache();
        File cacheDir = getCoversCacheDir(host);
        if (cacheDir == null) {
            try { Toast.makeText(host, R.string.cover_prefetch_none, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            return;
        }
        if (roots.isEmpty()) {
            try { Toast.makeText(host, R.string.cover_prefetch_none, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            return;
        }
        if (!hasInternetConnection(host)) {
            try { Toast.makeText(host, R.string.cover_prefetch_no_connection, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            return;
        }
        synchronized (coverPrefetchLock) {
            if (coverPrefetchRunning) {
                try { Toast.makeText(host, R.string.cover_prefetch_running, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                return;
            }
            coverPrefetchRunning = true;
        }
        try { Toast.makeText(host, R.string.cover_prefetch_start, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            new Thread(() -> {
                int downloaded = 0;
                try {
                    for (Uri root : roots) {
                    downloaded += prefetchCoversForRoot(root, template, cacheDir);
                    }
                } finally {
                    synchronized (coverPrefetchLock) {
                        coverPrefetchRunning = false;
                    }
            }
            final int total = downloaded;
            host.runOnUiThread(() -> {
                try {
                    if (total > 0) {
                        Toast.makeText(host, host.getString(R.string.cover_prefetch_done, total), Toast.LENGTH_SHORT).show();
                    } else {
                        Toast.makeText(host, R.string.cover_prefetch_none, Toast.LENGTH_SHORT).show();
                    }
                } catch (Throwable ignored) {}
            });
        }, "CoverPrefetch").start();
    }

    private int prefetchCoversForRoot(Uri root, String template, File cacheDir) {
        if (root == null) {
            return 0;
        }
        if (cacheDir == null) {
            return 0;
        }
        List<GameEntry> entries = GameScanner.scanFolder(host, root);
        if (entries == null || entries.isEmpty()) {
            return 0;
        }
        resolveMetadataForEntries(entries);
        int downloaded = 0;
        Set<String> attempted = new HashSet<>();
        for (GameEntry entry : entries) {
            if (ensureCoverCachedForEntry(cacheDir, entry, template, attempted)) {
                downloaded++;
            }
        }
        return downloaded;
    }

    private void resolveMetadataForEntries(List<GameEntry> entries) {
        if (entries == null || entries.isEmpty()) {
            return;
        }
        android.content.ContentResolver cr = host.getContentResolver();
        for (GameEntry ge : entries) {
            if (ge == null || ge.uri == null) {
                continue;
            }
            try {
                if (MainActivity.isChdEntry(ge.uri, ge.title)) {
                    Pair<String, String> cached = MainActivity.getPersistedChdMetadata(host, ge.uri);
                    if (cached != null) {
                        if (TextUtils.isEmpty(ge.serial) && !TextUtils.isEmpty(cached.first)) {
                            ge.serial = cached.first;
                        }
                        if (TextUtils.isEmpty(ge.gameTitle) && !TextUtils.isEmpty(cached.second)) {
                            ge.gameTitle = cached.second;
                        }
                    }
                    continue;
                }
                boolean needsSerial = TextUtils.isEmpty(ge.serial);
                boolean needsTitle = TextUtils.isEmpty(ge.gameTitle);
                if (!needsSerial && !needsTitle) {
                    continue;
                }
                RedumpDB.Result rd = RedumpDB.lookupByFile(cr, ge.uri);
                if (rd != null) {
                    if (needsSerial && !TextUtils.isEmpty(rd.serial)) {
                        ge.serial = rd.serial;
                    }
                    if (needsTitle && !TextUtils.isEmpty(rd.name)) {
                        ge.gameTitle = rd.name;
                    }
                }
            } catch (Throwable ignored) {}
        }
    }

    static List<String> buildCoverCandidateUrls(GameEntry entry, String template) {
        if (entry == null || TextUtils.isEmpty(template)) {
            return java.util.Collections.emptyList();
        }
        String fileBase = entry.fileTitleNoExt();
        String hyphenized = hyphenizeAlphaDigits(fileBase);
        List<String> variants = makeTitleVariants(fileBase);
        java.util.LinkedHashSet<String> urls = new java.util.LinkedHashSet<>();
        if (template.contains("${filetitle}")) {
            for (String v : variants) {
                urls.add(template.replace("${filetitle}", safeUrlPart(v))
                        .replace("${serial}", "")
                        .replace("${title}", ""));
            }
        }
        if (template.contains("${serial}")) {
            if (!TextUtils.isEmpty(entry.serial)) {
                urls.add(template.replace("${serial}", safeUrlPart(entry.serial))
                        .replace("${filetitle}", "")
                        .replace("${title}", ""));
            }
            if (!TextUtils.isEmpty(hyphenized) && !hyphenized.equals(fileBase)) {
                urls.add(template.replace("${serial}", safeUrlPart(hyphenized))
                        .replace("${filetitle}", "")
                        .replace("${title}", ""));
            }
            for (String v : variants) {
                urls.add(template.replace("${serial}", safeUrlPart(v))
                        .replace("${filetitle}", "")
                        .replace("${title}", ""));
            }
        }
        if (template.contains("${title}")) {
            String resolvedTitle = !TextUtils.isEmpty(entry.gameTitle) ? entry.gameTitle : fileBase;
            java.util.LinkedHashSet<String> titleVariants = new java.util.LinkedHashSet<>(makeTitleVariants(resolvedTitle));
            if (!TextUtils.isEmpty(entry.gameTitle) && !TextUtils.isEmpty(fileBase) && !entry.gameTitle.equals(fileBase)) {
                titleVariants.addAll(makeTitleVariants(fileBase));
            }
            for (String v : titleVariants) {
                urls.add(template.replace("${title}", safeUrlPart(v))
                        .replace("${serial}", "")
                        .replace("${filetitle}", ""));
            }
        }
        return new ArrayList<>(urls);
    }

    private static String safeUrlPart(String s) {
        if (s == null) {
            return "";
        }
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (Exception e) {
            return s;
        }
    }

    private static String hyphenizeAlphaDigits(String s) {
        if (s == null) {
            return "";
        }
        try {
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("^([A-Za-z]+)[-_]?([0-9]{3,})$").matcher(s);
            if (m.find()) {
                return (m.group(1).toUpperCase(Locale.US) + "-" + m.group(2));
            }
        } catch (Exception ignored) {}
        return s;
    }

    private static List<String> makeTitleVariants(String base) {
        java.util.LinkedHashSet<String> set = new java.util.LinkedHashSet<>();
        if (base == null) base = "";
        String b0 = base.trim();
        if (!b0.isEmpty()) set.add(b0);
        String b1 = b0.replace('_', ' ').trim(); if (!b1.isEmpty()) set.add(b1);
        String b1Sanitized = b1
                .replaceAll("\\[[^\\]]*\\]", " ")
                .replaceAll("\\([^\\)]*\\)", " ")
                .replaceAll("\\{[^\\}]*\\}", " ")
                .replaceAll("\\s+", " ")
                .trim();
        if (!b1Sanitized.isEmpty()) set.add(b1Sanitized);
        String b2 = b1.replace(":", " - ").replaceAll("\\s+", " ").trim(); if (!b2.isEmpty()) set.add(b2);
        try {
            String b3 = b1.replaceAll("(?i)(?<=\\w) \\–|\\u2014| - (?=\\w)", ": ");
            b3 = b3.replace(" - ", ": ");
            b3 = b3.replaceAll("\\s+", " ").trim();
            if (!b3.isEmpty()) set.add(b3);
            String b4 = b1Sanitized.replace(" - ", ": ").replaceAll("\\s+", " ").trim();
            if (!b4.isEmpty()) set.add(b4);
        } catch (Throwable ignored) {}
        return new ArrayList<>(set);
    }

    private static String sanitizeCoverFileComponent(String input) {
        if (TextUtils.isEmpty(input)) {
            return "";
        }
        String normalized = input.trim();
        normalized = normalized.replaceAll("[\\\\/:*?\"<>|]", " ");
        normalized = normalized.replaceAll("[^A-Za-z0-9._-]", "_");
        normalized = normalized.replaceAll("_+", "_");
        normalized = normalized.replaceAll("^_+|_+$", "");
        return normalized;
    }

    static String computeCoverBaseName(GameEntry entry) {
        String candidate = entry != null ? entry.serial : null;
        if (TextUtils.isEmpty(candidate) && entry != null) {
            candidate = entry.gameTitle;
        }
        if (TextUtils.isEmpty(candidate) && entry != null) {
            candidate = entry.fileTitleNoExt();
        }
        String sanitized = sanitizeCoverFileComponent(candidate);
        if (TextUtils.isEmpty(sanitized) && entry != null) {
            String fallback = entry.title != null ? entry.title : "cover";
            sanitized = sanitizeCoverFileComponent("cover_" + Integer.toHexString(fallback.hashCode()));
        }
        if (TextUtils.isEmpty(sanitized)) {
            sanitized = "cover";
        }
        return sanitized;
    }

    static File findExistingCoverFile(File dir, String baseName) {
        if (dir == null || TextUtils.isEmpty(baseName)) {
            return null;
        }
        String prefix = baseName.toLowerCase(Locale.US);
        File[] files = dir.listFiles();
        if (files == null) {
            return null;
        }
        for (File child : files) {
            if (child == null || !child.isFile()) continue;
            String name = child.getName();
            if (name == null) continue;
            String lower = name.toLowerCase(Locale.US);
            if (lower.equals(prefix) || lower.startsWith(prefix + ".")) {
                return child;
            }
        }
        return null;
    }

    static String guessImageExtension(String url, String contentType) {
        if (contentType != null) {
            String type = contentType.toLowerCase(Locale.US);
            if (type.contains("png")) return ".png";
            if (type.contains("webp")) return ".webp";
            if (type.contains("gif")) return ".gif";
            if (type.contains("jpeg") || type.contains("jpg")) return ".jpg";
        }
        if (url != null) {
            String path = url;
            int query = path.indexOf('?');
            if (query >= 0) {
                path = path.substring(0, query);
            }
            int dot = path.lastIndexOf('.');
            if (dot >= 0 && dot > path.lastIndexOf('/')) {
                String ext = path.substring(dot).toLowerCase(Locale.US);
                if (ext.matches("\\.(jpg|jpeg|png|webp|gif)")) {
                    return ext.equals(".jpeg") ? ".jpg" : ext;
                }
            }
        }
        return ".jpg";
    }

    private boolean downloadCoverToDirectory(File coversDir, String url, String baseName) {
        HttpURLConnection connection = null;
        InputStream in = null;
        OutputStream out = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(4000);
            connection.setReadTimeout(6000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestMethod("GET");
            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                return false;
            }
            String extension = guessImageExtension(url, connection.getContentType());
            String fileName = baseName + extension;
            File existing = findExistingCoverFile(coversDir, baseName);
            if (existing != null && existing.length() > 0) {
                return false;
            }
            if (existing != null) {
                if (!existing.delete()) {
                    return false;
                }
            }
            File file = new File(coversDir, fileName);
            File parent = file.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                return false;
            }
            out = new FileOutputStream(file);
            in = connection.getInputStream();
            byte[] buffer = new byte[8192];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            out.flush();
            return true;
        } catch (Exception ignored) {
            return false;
        } finally {
            if (in != null) {
                try { in.close(); } catch (IOException ignored) {}
            }
            if (out != null) {
                try { out.close(); } catch (IOException ignored) {}
            }
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    private boolean ensureCoverCachedForEntry(File coversDir, GameEntry entry, String template, Set<String> attemptedUrls) {
        List<String> urls = buildCoverCandidateUrls(entry, template);
        if (urls.isEmpty()) {
            return false;
        }
        String baseName = computeCoverBaseName(entry);
        if (TextUtils.isEmpty(baseName)) {
            return false;
        }
        File existing = findExistingCoverFile(coversDir, baseName);
        if (existing != null && existing.length() > 0) {
            return false;
        }
        for (String url : urls) {
            if (TextUtils.isEmpty(url) || url.contains("${")) {
                continue;
            }
            if (attemptedUrls != null && !attemptedUrls.add(url)) {
                continue;
            }
            if (downloadCoverToDirectory(coversDir, url, baseName)) {
                File stored = findExistingCoverFile(coversDir, baseName);
                if (stored != null && stored.isFile()) {
                    GamesAdapter.registerCachedCover(entry, stored);
                }
                try { DebugLog.d("Covers", "Cached cover for " + baseName + " from " + url); } catch (Throwable ignored) {}
                return true;
            }
        }
        return false;
    }

    private LinkedHashSet<Uri> collectGameRootUris() {
        LinkedHashSet<Uri> roots = new LinkedHashSet<>();
        if (host.gamesFolderUri != null) {
            roots.add(host.gamesFolderUri);
        }
        android.content.SharedPreferences prefs = host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE);
        String savedPrimary = prefs.getString(MainActivity.PREF_GAMES_URI, null);
        if (host.gamesFolderUri == null && !TextUtils.isEmpty(savedPrimary)) {
            try { roots.add(Uri.parse(savedPrimary)); } catch (Exception ignored) {}
        }
        java.util.Set<String> secondary = prefs.getStringSet("secondary_game_dirs", null);
        if (secondary != null) {
            for (String uriString : secondary) {
                if (TextUtils.isEmpty(uriString)) continue;
                try { roots.add(Uri.parse(uriString)); } catch (Exception ignored) {}
            }
        }
        return roots;
    }

    static boolean hasInternetConnection(Context context) {
        if (context == null) {
            return false;
        }
        try {
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return false;
            if (android.os.Build.VERSION.SDK_INT >= 23) {
                android.net.Network nw = cm.getActiveNetwork();
                if (nw == null) return false;
                android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(nw);
                return nc != null && (nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                        || nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_CELLULAR)
                        || nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_ETHERNET));
            } else {
                android.net.NetworkInfo ni = cm.getActiveNetworkInfo();
                return ni != null && ni.isConnected();
            }
        } catch (Throwable ignored) {
            return false;
        }
    }

    static File getCoversCacheDir(Context ctx) {
        if (ctx == null) {
            return null;
        }
        Context appCtx = ctx.getApplicationContext();
        if (appCtx == null) {
            appCtx = ctx;
        }
        File base = DataDirectoryManager.getDataRoot(appCtx);
        if (base == null) {
            return null;
        }
        File dir = new File(base, "armsx2_covers");
        if (!dir.exists() && !dir.mkdirs()) {
            return null;
        }
        return dir;
    }

    // endregion Covers
}
