package kr.co.iefriends.pcsx2.activities;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.text.TextUtils;

import androidx.core.util.Pair;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import kr.co.iefriends.pcsx2.utils.DebugLog;

class GameScanner {
    static final String[] EXTS = new String[]{".iso", ".img", ".bin", ".cso", ".zso", ".chd", ".gz"};

    static List<GameEntry> scanFolder(Context ctx, Uri treeUri) {
        List<GameEntry> out = new ArrayList<>();
        ContentResolver cr = ctx.getContentResolver();
        try {
            String rootId = android.provider.DocumentsContract.getTreeDocumentId(treeUri);
            scanChildren(cr, treeUri, rootId, out, 0, 3);
        } catch (Exception ignored) {}
        for (GameEntry e : out) {
            if (e == null || !MainActivity.isChdEntry(e.uri, e.title)) {
                continue;
            }
            Pair<String, String> metadata = MainActivity.getPersistedChdMetadata(ctx, e.uri);
            if (metadata == null) {
                continue;
            }
            if (TextUtils.isEmpty(e.serial) && !TextUtils.isEmpty(metadata.first)) {
                e.serial = metadata.first;
            }
            if (TextUtils.isEmpty(e.gameTitle) && !TextUtils.isEmpty(metadata.second)) {
                e.gameTitle = metadata.second;
            }
        }
        return out;
    }

    static List<String> debugList(Context ctx, Uri treeUri) {
        List<String> out = new ArrayList<>();
        try {
            ContentResolver cr = ctx.getContentResolver();
            String rootId = android.provider.DocumentsContract.getTreeDocumentId(treeUri);
            debugChildren(cr, treeUri, rootId, out, 0, 3, "/");
        } catch (Exception e) { out.add("Error: " + e.getMessage()); }
        return out;
    }

    private static void scanChildren(ContentResolver cr, Uri treeUri, String parentDocId,
                                     List<GameEntry> out, int depth, int maxDepth) {
        if (depth > maxDepth) return;
        Uri children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        try (android.database.Cursor c = cr.query(children, new String[]{
                android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE
        }, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String docId = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                if (mime != null && mime.equals(android.provider.DocumentsContract.Document.MIME_TYPE_DIR)) {
                    scanChildren(cr, treeUri, docId, out, depth + 1, maxDepth);
                    continue;
                }
                if (name == null) name = "Unknown";
                String lower = name.toLowerCase();
                boolean matchExt = false;
                for (String ext : EXTS) { if (lower.endsWith(ext)) { matchExt = true; break; } }
                boolean matchMime = false;
                if (mime != null) {
                    String lm = mime.toLowerCase();
                    if (lm.contains("iso9660") || lm.equals("application/x-iso9660-image")) matchMime = true;
                }
                boolean match = matchExt || matchMime;
                if (!match) continue;
                Uri doc = android.provider.DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                GameEntry e = new GameEntry(name, doc);
                String ft = e.fileTitleNoExt();
                String s = parseSerialFromString(ft);
                if (s != null) e.serial = s;
                String lowerName = name.toLowerCase();
                if (e.serial == null && (lowerName.endsWith(".iso") || lowerName.endsWith(".img") || lowerName.endsWith(".cso") || lowerName.endsWith(".zso"))) {
                    try {
                        String isoSerial = tryExtractIsoSerial(cr, doc);
                        if (isoSerial != null) e.serial = isoSerial;
                    } catch (Throwable t) {
                        try { DebugLog.d("ISO", "Serial parse failed: " + t.getMessage()); } catch (Throwable ignored) {}
                    }
                }
                if (e.serial == null && lowerName.endsWith(".bin")) {
                    try {
                        String quick = tryExtractBinSerialQuick(cr, doc);
                        if (quick != null) e.serial = quick;
                    } catch (Throwable t) {
                        try { DebugLog.d("BIN", "Quick serial scan failed: " + t.getMessage()); } catch (Throwable ignored) {}
                    }
                }
                out.add(e);
            }
        } catch (Exception ignored) {}
    }

    private static void debugChildren(ContentResolver cr, Uri treeUri, String parentDocId,
                                       List<String> out, int depth, int maxDepth, String pathPrefix) {
        if (depth > maxDepth) return;
        Uri children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        try (android.database.Cursor c = cr.query(children, new String[]{
                android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE
        }, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String docId = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                String display = pathPrefix + (name != null ? name : "<null>") + (mime != null && mime.equals(android.provider.DocumentsContract.Document.MIME_TYPE_DIR) ? "/" : "");
                boolean dir = mime != null && mime.equals(android.provider.DocumentsContract.Document.MIME_TYPE_DIR);
                boolean accept = false;
                if (!dir && name != null) {
                    String lower = name.toLowerCase();
                    boolean matchExt = false;
                    for (String ext : EXTS) { if (lower.endsWith(ext)) { matchExt = true; break; } }
                    boolean matchMime = false;
                    if (mime != null) {
                        String lm = mime.toLowerCase();
                        if (lm.contains("iso9660") || lm.equals("application/x-iso9660-image")) matchMime = true;
                    }
                    accept = matchExt || matchMime;
                }
                out.add("[" + (mime == null ? "null" : mime) + "] " + display + (dir ? "" : (accept ? "  -> accepted" : "  -> skipped")));
                if (mime != null && mime.equals(android.provider.DocumentsContract.Document.MIME_TYPE_DIR)) {
                    debugChildren(cr, treeUri, docId, out, depth + 1, maxDepth, display);
                }
            }
        } catch (Exception e) { out.add("Error listing: " + e.getMessage()); }
    }

    static String stripExt(String name) {
        int i = name.lastIndexOf('.');
        return (i > 0) ? name.substring(0, i) : name;
    }

    static String parseSerialFromString(String s) {
        if (s == null) return null;
        Pattern p = Pattern.compile(
            "(S[CL](?:ES|US|PS|CS)?[-_]?[0-9]{3,5}(?:\\.[0-9]{2})?)",
            Pattern.CASE_INSENSITIVE);
        Matcher m = p.matcher(s);
        if (m.find()) {
            String v = m.group(1).toUpperCase();
            v = v.replace('_', '-');
            v = v.replace(".", "");
            v = v.replaceAll("^([A-Z]+)([0-9])", "$1-$2");
            return v;
        }
        return null;
    }

    static String tryExtractIsoSerial(ContentResolver cr, Uri uri) throws IOException {
        final int SECTOR = 2048;
        byte[] pvd = readRange(cr, uri, 16L * SECTOR, SECTOR);
        if (pvd == null || pvd.length < SECTOR) return null;
        if (pvd[0] != 0x01 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
            return null;
        int rootLBA = u32le(pvd, 156 + 2);
        int rootSize = u32le(pvd, 156 + 10);
        if (rootLBA <= 0 || rootSize <= 0 || rootSize > 512 * 1024) rootSize = 64 * 1024;
        byte[] dir = readRange(cr, uri, (long) rootLBA * SECTOR, rootSize);
        if (dir == null) return null;
        int off = 0;
        while (off < dir.length) {
            int len = u8(dir, off);
            if (len == 0) {
                int next = ((off / SECTOR) + 1) * SECTOR;
                if (next <= off) break;
                off = next;
                continue;
            }
            if (off + len > dir.length) break;
            int lba = u32le(dir, off + 2);
            int size = u32le(dir, off + 10);
            int nameLen = u8(dir, off + 32);
            int namePos = off + 33;
            if (namePos + nameLen <= dir.length && nameLen > 0) {
                String name = new String(dir, namePos, nameLen, StandardCharsets.US_ASCII);
                if (!(nameLen == 1 && (dir[namePos] == 0 || dir[namePos] == 1))) {
                    String norm = name;
                    int semi = norm.indexOf(';');
                    if (semi >= 0) norm = norm.substring(0, semi);
                    if ("SYSTEM.CNF".equalsIgnoreCase(norm)) {
                        int readSize = Math.min(size, 4096);
                        byte[] cnf = readRange(cr, uri, (long) lba * SECTOR, readSize);
                        if (cnf != null) {
                            String txt = new String(cnf, StandardCharsets.US_ASCII);
                            Matcher m = Pattern.compile(
                                "BOOT\\d*\\s*=\\s*[^\\\\\\r\\n]*\\\\([A-Z0-9_\\.]+)",
                                Pattern.CASE_INSENSITIVE).matcher(txt);
                            if (m.find()) {
                                String bootElf = m.group(1);
                                String serial = parseSerialFromString(bootElf);
                                if (serial != null) return serial;
                            }
                        }
                        break;
                    }
                }
            }
            off += len;
        }
        return null;
    }

    static String tryExtractBinSerialQuick(ContentResolver cr, Uri uri) throws IOException {
        final int MAX = 8 * 1024 * 1024;
        byte[] buf;
        try (InputStream in = CsoUtils.openInputStream(cr, uri)) {
            if (in == null) return null;
            ByteArrayOutputStream bos = new ByteArrayOutputStream(Math.min(MAX, 1 << 20));
            byte[] tmp = new byte[64 * 1024];
            int total = 0;
            while (total < MAX) {
                int want = Math.min(tmp.length, MAX - total);
                int r = in.read(tmp, 0, want);
                if (r <= 0) break;
                bos.write(tmp, 0, r);
                total += r;
            }
            buf = bos.toByteArray();
        }
        if (buf == null || buf.length == 0) return null;
        String txt = new String(buf, StandardCharsets.US_ASCII);
        Matcher m = Pattern.compile(
            "BOOT\\d*\\s*=\\s*[^\\\\\\r\\n]*\\\\([A-Z0-9_\\.]+)",
            Pattern.CASE_INSENSITIVE).matcher(txt);
        if (m.find()) {
            String bootElf = m.group(1);
            String serial = parseSerialFromString(bootElf);
            if (serial != null) return serial;
        }
        return parseSerialFromString(txt);
    }

    private static int u8(byte[] a, int i) { return (i >= 0 && i < a.length) ? (a[i] & 0xFF) : 0; }

    private static int u32le(byte[] a, int i) {
        if (i + 3 >= a.length) return 0;
        return (a[i] & 0xFF) | ((a[i + 1] & 0xFF) << 8) | ((a[i + 2] & 0xFF) << 16) | ((a[i + 3] & 0xFF) << 24);
    }

    private static byte[] readRange(ContentResolver cr, Uri uri, long offset, int size) throws IOException {
        if (size <= 0) return null;
        if (size > 2 * 1024 * 1024) size = 2 * 1024 * 1024;
        byte[] csoBytes = CsoUtils.readRange(cr, uri, offset, size);
        if (csoBytes != null) {
            return csoBytes;
        }
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) return null;
            long toSkip = offset;
            byte[] skipBuf = new byte[8192];
            while (toSkip > 0) {
                long skipped = in.skip(toSkip);
                if (skipped <= 0) {
                    int r = in.read(skipBuf, 0, (int) Math.min(skipBuf.length, toSkip));
                    if (r <= 0) break;
                    toSkip -= r;
                } else {
                    toSkip -= skipped;
                }
            }
            byte[] buf = new byte[size];
            int off2 = 0;
            while (off2 < size) {
                int r = in.read(buf, off2, size - off2);
                if (r <= 0) break;
                off2 += r;
            }
            if (off2 == 0) return null;
            if (off2 < size) return java.util.Arrays.copyOf(buf, off2);
            return buf;
        }
    }
}
