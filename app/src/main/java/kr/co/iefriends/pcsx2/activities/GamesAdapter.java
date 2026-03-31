package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.text.TextUtils;
import android.util.LruCache;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DebugLog;

class GamesAdapter extends RecyclerView.Adapter<GamesAdapter.VH> {
    interface OnClick { void onClick(GameEntry e); }

    static class VH extends RecyclerView.ViewHolder {
        final TextView tv;
        final android.widget.ImageView img;
        final TextView tvOverlay;
        VH(View v) {
            super(v);
            this.tv = v.findViewById(R.id.tv_title);
            this.img = v.findViewById(R.id.img_cover);
            this.tvOverlay = v.findViewById(R.id.tv_cover_fallback);
        }
    }

    private final List<GameEntry> data;
    private final List<GameEntry> filtered = new ArrayList<>();
    private final OnClick onClick;
    private boolean listMode = false;

    // Lightweight in-memory cache for cover bitmaps
    private static final LruCache<String, Bitmap> sCoverCache;
    private static final Set<String> sNegativeCache = Collections.synchronizedSet(new HashSet<>());
    private static final ExecutorService sExec = Executors.newFixedThreadPool(3);
    private static final Map<String, File> sLocalCoverFiles = Collections.synchronizedMap(new HashMap<>());
    private static final Set<String> sLocalCoverMissing = Collections.synchronizedSet(new HashSet<>());

    static {
        int maxMem = (int) (Runtime.getRuntime().maxMemory() / 1024);
        int cacheSize = Math.max(1024 * 8, Math.min(1024 * 64, maxMem / 16));
        sCoverCache = new LruCache<String, Bitmap>(cacheSize) {
            @Override protected int sizeOf(String key, Bitmap value) {
                return value.getByteCount() / 1024;
            }
        };
    }

    static void clearLocalCoverCache() {
        sLocalCoverFiles.clear();
        sLocalCoverMissing.clear();
    }

    static void registerCachedCover(GameEntry entry, File file) {
        if (entry == null || file == null || !file.exists()) {
            return;
        }
        String key = coverKey(entry);
        if (TextUtils.isEmpty(key)) {
            return;
        }
        sLocalCoverFiles.put(key, file);
        sLocalCoverMissing.remove(key);
    }

    GamesAdapter(List<GameEntry> d, OnClick oc) {
        data = d;
        filtered.addAll(d);
        onClick = oc;
        setHasStableIds(true);
    }

    void update(List<GameEntry> d) {
        clearLocalCoverCache();
        data.clear();
        data.addAll(d);
        applyFilter(currentFilter);
    }

    int getItemCountTotal() { return data.size(); }

    private String currentFilter = "";

    void setFilter(String q) { currentFilter = q == null ? "" : q.trim(); applyFilter(currentFilter); }

    private void applyFilter(String q) {
        filtered.clear();
        if (TextUtils.isEmpty(q)) {
            filtered.addAll(data);
        } else {
            String needle = q.toLowerCase();
            for (GameEntry e : data) {
                String t = e != null && e.title != null ? e.title.toLowerCase() : "";
                String s = e != null && e.serial != null ? e.serial.toLowerCase() : "";
                if (t.contains(needle) || s.contains(needle)) filtered.add(e);
            }
        }
        notifyDataSetChanged();
    }

    void setListMode(boolean list) { this.listMode = list; notifyDataSetChanged(); }

    @Override public int getItemViewType(int position) { return listMode ? 1 : 0; }

    @NonNull @Override
    public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        int layout = (viewType == 1) ? R.layout.item_game_list : R.layout.item_game;
        View v = LayoutInflater.from(parent.getContext()).inflate(layout, parent, false);
        return new VH(v);
    }

    @Override public long getItemId(int position) {
        try {
            GameEntry e = data.get(position);
            String key = (e.uri != null ? e.uri.toString() : e.title) + "|" + (e.title != null ? e.title : "");
            return (long) key.hashCode();
        } catch (Throwable ignored) { return position; }
    }

    @Override public void onViewRecycled(@NonNull VH holder) {
        super.onViewRecycled(holder);
        try {
            holder.img.setTag(R.id.tag_request_key, null);
            holder.img.setImageDrawable(null);
        } catch (Throwable ignored) {}
    }

    @Override public void onBindViewHolder(@NonNull VH holder, int position) {
        GameEntry e = filtered.get(position);
        String tpl = ((MainActivity) holder.itemView.getContext()).getCoversUrlTemplate();
        boolean loaded = false;
        try { holder.img.setImageDrawable(null); } catch (Throwable ignored) {}
        try { holder.img.setBackgroundColor(Color.TRANSPARENT); } catch (Throwable ignored) {}
        if (holder.tvOverlay != null) holder.tvOverlay.setVisibility(View.GONE);
        try {
            String gameKey = MainActivity.gameKeyFromEntry(e);
            String manual = ((MainActivity) holder.itemView.getContext()).getManualCoverUri(gameKey);
            if (manual != null && !manual.isEmpty()) {
                Uri mu = Uri.parse(manual);
                try (InputStream is = holder.itemView.getContext().getContentResolver().openInputStream(mu)) {
                    if (is != null) {
                        Bitmap bmp = BitmapFactory.decodeStream(is);
                        if (bmp != null) {
                            holder.img.setImageBitmap(bmp);
                            loaded = true;
                        }
                    }
                } catch (Throwable ignored) {}
            }
        } catch (Throwable ignored) {}
        if (!loaded) {
            File cachedLocal = findCachedCoverFile(holder.itemView.getContext(), e);
            if (cachedLocal != null && cachedLocal.exists()) {
                String localKey = cachedLocal.getAbsolutePath();
                Bitmap cachedBmp = sCoverCache.get(localKey);
                if (cachedBmp != null) {
                    holder.img.setImageBitmap(cachedBmp);
                    loaded = true;
                } else {
                    Bitmap bmp = BitmapFactory.decodeFile(localKey);
                    if (bmp != null) {
                        holder.img.setImageBitmap(bmp);
                        sCoverCache.put(localKey, bmp);
                        loaded = true;
                    }
                }
            }
        }
        boolean online = CoverManager.hasInternetConnection(holder.itemView.getContext());
        if (!loaded && online && tpl != null && !tpl.isEmpty()) {
            List<String> urls = CoverManager.buildCoverCandidateUrls(e, tpl);
            String requestKey = (e.uri != null ? e.uri.toString() : e.title) + "|" + (e.serial != null ? e.serial : "") + "|" + (e.title != null ? e.title : "");
            holder.img.setTag(R.id.tag_request_key, requestKey);
            for (String u : urls) {
                if (u == null || u.isEmpty() || u.contains("${")) continue;
                Bitmap cached = sCoverCache.get(u);
                if (cached != null) {
                    loaded = true;
                    holder.img.setImageBitmap(cached);
                    break;
                }
            }
            if (!loaded && !urls.isEmpty())
                loadImageWithFallback(holder.img, holder.tvOverlay, holder.itemView.getContext(), e, urls, requestKey);
        }
        holder.img.setVisibility(View.VISIBLE);
        if (listMode) {
            holder.tv.setVisibility(View.VISIBLE);
            holder.tv.setText(e.gameTitle != null ? e.gameTitle : e.title);
            if (holder.tvOverlay != null) holder.tvOverlay.setVisibility(View.GONE);
        } else {
            if (loaded) {
                if (holder.tvOverlay != null) holder.tvOverlay.setVisibility(View.GONE);
                holder.tv.setVisibility(View.GONE);
            } else {
                holder.tv.setVisibility(View.GONE);
                if (holder.tvOverlay != null) {
                    holder.tvOverlay.setText(e.gameTitle != null ? e.gameTitle : e.title);
                    holder.tvOverlay.setVisibility(View.VISIBLE);
                    holder.tvOverlay.bringToFront();
                }
            }
        }
        holder.itemView.setOnClickListener(v -> onClick.onClick(e));
        holder.itemView.setOnKeyListener((v, keyCode, event) -> {
            if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
            RecyclerView rv = (RecyclerView) holder.itemView.getParent();
            RecyclerView.LayoutManager lm = rv.getLayoutManager();
            if (!(lm instanceof GridLayoutManager)) return false;
            int span = ((GridLayoutManager) lm).getSpanCount();
            int pos = holder.getAdapterPosition();
            if (pos == RecyclerView.NO_POSITION) return false;
            switch (keyCode) {
                case KeyEvent.KEYCODE_DPAD_LEFT:
                    if (pos % span > 0) {
                        int target = pos - 1;
                        rv.smoothScrollToPosition(target);
                        rv.post(() -> {
                            RecyclerView.ViewHolder tvh = rv.findViewHolderForAdapterPosition(target);
                            if (tvh != null) tvh.itemView.requestFocus();
                        });
                    }
                    return true;
                case KeyEvent.KEYCODE_DPAD_RIGHT:
                    if (pos + 1 < getItemCount()) {
                        int target = pos + 1;
                        rv.smoothScrollToPosition(target);
                        rv.post(() -> {
                            RecyclerView.ViewHolder tvh = rv.findViewHolderForAdapterPosition(target);
                            if (tvh != null) tvh.itemView.requestFocus();
                        });
                    }
                    return true;
                case KeyEvent.KEYCODE_DPAD_UP:
                    if (pos - span >= 0) {
                        int target = pos - span;
                        rv.smoothScrollToPosition(target);
                        rv.post(() -> {
                            RecyclerView.ViewHolder tvh = rv.findViewHolderForAdapterPosition(target);
                            if (tvh != null) tvh.itemView.requestFocus();
                        });
                    }
                    return true;
                case KeyEvent.KEYCODE_DPAD_DOWN:
                    if (pos + span < getItemCount()) {
                        int target = pos + span;
                        rv.smoothScrollToPosition(target);
                        rv.post(() -> {
                            RecyclerView.ViewHolder tvh = rv.findViewHolderForAdapterPosition(target);
                            if (tvh != null) tvh.itemView.requestFocus();
                        });
                    }
                    return true;
                case KeyEvent.KEYCODE_BUTTON_A:
                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_ENTER:
                    v.performClick();
                    return true;
            }
            return false;
        });
        holder.itemView.setOnLongClickListener(v -> {
            try { ((MainActivity) holder.itemView.getContext()).showGameOptionsPopup(v, e); } catch (Throwable ignored) {}
            return true;
        });
    }

    @Override public int getItemCount() { return filtered.size(); }

    private void loadImageWithFallback(android.widget.ImageView iv, TextView overlayView,
                                       Context ctx, GameEntry entry, List<String> urls, String requestKey) {
        try {
            sExec.execute(() -> {
                try {
                    Bitmap bmp = null;
                    String hitUrl = null;
                    byte[] downloadedBytes = null;
                    String downloadExtension = null;
                    for (String ustr : urls) {
                        if (ustr == null || ustr.isEmpty() || ustr.contains("${")) continue;
                        Object tag = iv.getTag(R.id.tag_request_key);
                        if (!(requestKey.equals(tag))) { break; }
                        if (sNegativeCache.contains(ustr)) continue;
                        Bitmap cached = sCoverCache.get(ustr);
                        if (cached != null) { bmp = cached; hitUrl = ustr; break; }
                        try {
                            HttpURLConnection c = (HttpURLConnection) new URL(ustr).openConnection();
                            c.setConnectTimeout(4000); c.setReadTimeout(6000);
                            c.setInstanceFollowRedirects(true);
                            c.setRequestMethod("GET");
                            int code = c.getResponseCode();
                            if (code == 200) {
                                try (InputStream is = c.getInputStream();
                                     java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream()) {
                                    byte[] buffer = new byte[8192];
                                    int read;
                                    while ((read = is.read(buffer)) != -1) {
                                        baos.write(buffer, 0, read);
                                    }
                                    byte[] data = baos.toByteArray();
                                    if (data.length > 0) {
                                        Bitmap candidate = BitmapFactory.decodeByteArray(data, 0, data.length);
                                        if (candidate != null) {
                                            bmp = candidate;
                                            downloadedBytes = data;
                                            downloadExtension = CoverManager.guessImageExtension(ustr, c.getContentType());
                                            hitUrl = ustr;
                                            break;
                                        }
                                    }
                                }
                            } else if (code == 404) {
                                sNegativeCache.add(ustr);
                                continue;
                            } else {
                                try { DebugLog.d("Covers", "HTTP " + code + " for " + ustr); } catch (Throwable ignored) {}
                            }
                        } catch (Exception ex) {
                            try { DebugLog.d("Covers", "Error loading cover: " + ex.getMessage()); } catch (Throwable ignored) {}
                        }
                    }
                    if (downloadedBytes != null && downloadedBytes.length > 0 && entry != null && ctx != null) {
                        try { storeCoverBytes(ctx, entry, downloadedBytes, downloadExtension); } catch (Throwable ignored) {}
                    }
                    final Bitmap fb = bmp;
                    final String fUrl = hitUrl;
                    iv.post(() -> {
                        Object tagNow = iv.getTag(R.id.tag_request_key);
                        if (requestKey.equals(tagNow) && fb != null) {
                            iv.setImageBitmap(fb);
                            if (fUrl != null) sCoverCache.put(fUrl, fb);
                            if (overlayView != null) overlayView.setVisibility(View.GONE);
                        }
                    });
                } catch (Throwable ignored) {}
            });
        } catch (Throwable ignored) {}
    }

    private File findCachedCoverFile(Context ctx, GameEntry entry) {
        if (ctx == null || entry == null || entry.uri == null) {
            return null;
        }
        String key = coverKey(entry);
        if (TextUtils.isEmpty(key)) {
            return null;
        }
        File cached = sLocalCoverFiles.get(key);
        if (cached != null && cached.exists()) {
            return cached;
        }
        if (sLocalCoverMissing.contains(key)) {
            return null;
        }
        File cacheDir = CoverManager.getCoversCacheDir(ctx);
        if (cacheDir == null) {
            sLocalCoverMissing.add(key);
            return null;
        }
        String baseName = CoverManager.computeCoverBaseName(entry);
        File coverFile = CoverManager.findExistingCoverFile(cacheDir, baseName);
        if (coverFile != null && coverFile.isFile() && coverFile.length() > 0) {
            sLocalCoverFiles.put(key, coverFile);
            sLocalCoverMissing.remove(key);
            return coverFile;
        }
        sLocalCoverMissing.add(key);
        return null;
    }

    private static void storeCoverBytes(Context ctx, GameEntry entry, byte[] data, String extension) {
        if (ctx == null || entry == null || data == null || data.length == 0) {
            return;
        }
        File cacheDir = CoverManager.getCoversCacheDir(ctx);
        if (cacheDir == null) {
            return;
        }
        String baseName = CoverManager.computeCoverBaseName(entry);
        if (TextUtils.isEmpty(baseName)) {
            return;
        }
        String ext = extension;
        if (TextUtils.isEmpty(ext)) {
            ext = ".jpg";
        }
        if (!ext.startsWith(".")) {
            ext = "." + ext;
        }
        File target = new File(cacheDir, baseName + ext);
        File parent = target.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return;
        }
        File temp = new File(cacheDir, baseName + "_tmp" + ext);
        try (FileOutputStream fos = new FileOutputStream(temp)) {
            fos.write(data);
            fos.flush();
        } catch (IOException ignored) {
            //noinspection ResultOfMethodCallIgnored
            temp.delete();
            return;
        }
        if (!temp.renameTo(target)) {
            //noinspection ResultOfMethodCallIgnored
            temp.delete();
            return;
        }
        GamesAdapter.registerCachedCover(entry, target);
        try { DebugLog.d("Covers", "Stored cover cache file: " + target.getAbsolutePath()); } catch (Throwable ignored) {}
    }

    @Nullable
    private static String coverKey(GameEntry entry) {
        if (entry == null) {
            return null;
        }
        if (entry.uri != null) {
            return entry.uri.toString();
        }
        String fallback = entry.title;
        if (TextUtils.isEmpty(fallback)) {
            fallback = entry.fileTitleNoExt();
        }
        return fallback;
    }
}
