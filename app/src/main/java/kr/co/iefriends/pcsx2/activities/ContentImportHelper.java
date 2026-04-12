package kr.co.iefriends.pcsx2.activities;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.widget.Toast;

import androidx.annotation.StringRes;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;
import kr.co.iefriends.pcsx2.utils.DebugLog;

class ContentImportHelper {
    private final MainActivity host;

    ContentImportHelper(MainActivity host) {
        this.host = host;
    }

    boolean importMemcardToSlot1(Uri uri) {
        try {
            File base = DataDirectoryManager.getDataRoot(host.getApplicationContext());
            File memDir = new File(base, "memcards");
            if (!memDir.exists() && !memDir.mkdirs()) return false;
            File out = new File(memDir, "Mcd001.ps2");
            try (InputStream in = host.getContentResolver().openInputStream(uri);
                 OutputStream os = new FileOutputStream(out)) {
                if (in == null) return false;
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                os.flush();
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    void importCheatFile(Uri uri) {
        if (uri == null) {
            return;
        }
        new Thread(() -> {
            boolean success = false;
            String targetName = null;
            String errorReason = null;
            File dataRoot = DataDirectoryManager.getDataRoot(host.getApplicationContext());
            if (dataRoot == null) {
                errorReason = "Cheat import unavailable: data directory not resolved.";
            } else {
                File cheatsDir = new File(dataRoot, "cheats");
                if (!cheatsDir.exists() && !cheatsDir.mkdirs()) {
                    errorReason = "Unable to create cheats directory: " + cheatsDir;
                    try { DebugLog.e("Cheats", errorReason); } catch (Throwable ignored) {}
                } else {
                    String displayName = getDisplayNameForUri(uri);
                    if (TextUtils.isEmpty(displayName)) {
                        displayName = "custom_cheats.pnach";
                    }
                    if (!displayName.toLowerCase(Locale.US).endsWith(".pnach")) {
                        displayName = displayName + ".pnach";
                    }
                    File destination = createUniqueFile(cheatsDir, displayName);
                    try (InputStream in = host.getContentResolver().openInputStream(uri);
                         OutputStream out = new FileOutputStream(destination)) {
                        if (in == null) {
                            throw new IOException("Cheat source stream unavailable.");
                        }
                        byte[] buffer = new byte[8192];
                        int read;
                        while ((read = in.read(buffer)) != -1) {
                            out.write(buffer, 0, read);
                        }
                        out.flush();
                        success = true;
                        targetName = destination.getName();
                    } catch (Exception e) {
                        errorReason = e.getMessage();
                        if (errorReason == null || errorReason.trim().isEmpty()) {
                            errorReason = e.getClass().getSimpleName();
                        }
                        try { DebugLog.e("Cheats", "Import failed: " + errorReason); } catch (Throwable ignored) {}
                    }
                }
            }

            boolean finalSuccess = success;
            String finalName = targetName;
            String finalError = errorReason;
            host.runOnUiThread(() -> {
                Toast.makeText(host,
                        finalSuccess
                                ? host.getString(R.string.drawer_toast_cheats_import_success, finalName)
                                : host.getString(R.string.drawer_toast_cheats_import_failed),
                        Toast.LENGTH_SHORT).show();
                if (finalSuccess) {
                    try {
                        NativeApp.setEnableCheats(true);
                    } catch (Throwable ignored) {}
                } else {
                    showDrawerImportFailureDialog(R.string.drawer_error_import_cheats_title, finalError);
                }
            });
        }).start();
    }

    void importTextureArchive(Uri uri) {
        if (uri == null) {
            return;
        }
        new Thread(() -> {
            boolean success = false;
            String errorReason = null;
            File dataRoot = DataDirectoryManager.getDataRoot(host.getApplicationContext());
            if (dataRoot == null) {
                errorReason = "Texture import unavailable: data directory not resolved.";
            } else {
                File texturesDir = new File(dataRoot, "textures");
                if (!texturesDir.exists() && !texturesDir.mkdirs()) {
                    errorReason = "Unable to create textures directory: " + texturesDir;
                    try { DebugLog.e("Textures", errorReason); } catch (Throwable ignored) {}
                } else {
                    try (InputStream inputStream = host.getContentResolver().openInputStream(uri)) {
                        if (inputStream == null) {
                            throw new IOException("Texture archive stream unavailable.");
                        }
                        try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(inputStream))) {
                            byte[] buffer = new byte[8192];
                            ZipEntry entry;
                            while ((entry = zis.getNextEntry()) != null) {
                                File outFile = new File(texturesDir, entry.getName());
                                if (!isFileInsideBase(texturesDir, outFile)) {
                                    zis.closeEntry();
                                    continue;
                                }
                                if (entry.isDirectory()) {
                                    if (!outFile.exists() && !outFile.mkdirs()) {
                                        throw new IOException("Failed to create directory " + outFile);
                                    }
                                } else {
                                    File parent = outFile.getParentFile();
                                    if (parent != null && !parent.exists() && !parent.mkdirs()) {
                                        throw new IOException("Failed to create parent " + parent);
                                    }
                                    try (OutputStream out = new BufferedOutputStream(new FileOutputStream(outFile))) {
                                        int count;
                                        while ((count = zis.read(buffer)) != -1) {
                                            out.write(buffer, 0, count);
                                        }
                                        out.flush();
                                    }
                                }
                                zis.closeEntry();
                            }
                            success = true;
                        }
                    } catch (Exception e) {
                        errorReason = e.getMessage();
                        if (errorReason == null || errorReason.trim().isEmpty()) {
                            errorReason = e.getClass().getSimpleName();
                        }
                        try { DebugLog.e("Textures", "Import failed: " + errorReason); } catch (Throwable ignored) {}
                    }
                }
            }

            boolean finalSuccess = success;
            String finalError = errorReason;
            host.runOnUiThread(() -> {
                Toast.makeText(host,
                        finalSuccess
                                ? host.getString(R.string.drawer_toast_textures_import_success)
                                : host.getString(R.string.drawer_toast_textures_import_failed),
                        Toast.LENGTH_SHORT).show();
                if (finalSuccess) {
                    try {
                        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", "true");
                        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", "true");
                    } catch (Throwable ignored) {}
                } else {
                    showDrawerImportFailureDialog(R.string.drawer_error_import_textures_title, finalError);
                }
            });
        }).start();
    }

    String getDisplayNameForUri(Uri uri) {
        if (uri == null) {
            return null;
        }
        try (Cursor cursor = host.getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    return cursor.getString(index);
                }
            }
        } catch (Exception ignored) {}
        return null;
    }

    File createUniqueFile(File directory, String name) {
        File candidate = new File(directory, name);
        if (!candidate.exists()) {
            return candidate;
        }
        String baseName = name;
        String extension = "";
        int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            baseName = name.substring(0, dot);
            extension = name.substring(dot);
        }
        int index = 1;
        while (candidate.exists()) {
            candidate = new File(directory, baseName + "_" + index + extension);
            index++;
        }
        return candidate;
    }

    boolean isFileInsideBase(File base, File target) {
        try {
            String basePath = base.getCanonicalPath();
            String targetPath = target.getCanonicalPath();
            return targetPath.startsWith(basePath + File.separator);
        } catch (IOException e) {
            return false;
        }
    }

    void persistUriPermission(Uri uri) {
        if (uri == null) {
            return;
        }
        try {
            host.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {}
    }

    void showDrawerImportFailureDialog(@StringRes int titleRes, String details) {
        if (host.isFinishing()) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && host.isDestroyed()) {
            return;
        }
        String message = (details != null && !details.trim().isEmpty())
                ? details.trim()
                : host.getString(R.string.drawer_error_import_unknown);
        new MaterialAlertDialogBuilder(host)
                .setTitle(titleRes)
                .setMessage(message)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }
}
