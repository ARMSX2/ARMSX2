package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.core.util.Pair;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.File;
import java.util.Locale;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;

class ChdConversionManager {

    static final String PREF_CHD_SERIAL_PREFIX = "chd_serial:";
    static final String PREF_CHD_TITLE_PREFIX = "chd_title:";

    // Pending state for the two-step conversion → save flow
    private String pendingChdCachePath;
    private String pendingChdDisplayName;
    private Uri pendingChdSourceUri;
    private String pendingChdSourceSerial;
    private String pendingChdSourceTitle;

    private final MainActivity host;

    ChdConversionManager(MainActivity host) {
        this.host = host;
    }

    // ---- Static helpers (also used by CoverManager / GameScanner) -----------------

    static String stripFileExtension(@Nullable String name) {
        if (TextUtils.isEmpty(name)) {
            return name;
        }
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static String makeChdMetadataKey(String prefix, Uri uri) {
        return prefix + uri.toString();
    }

    static boolean isChdEntry(@Nullable Uri uri, @Nullable String title) {
        String lowerTitle = title != null ? title.toLowerCase(Locale.US) : "";
        if (lowerTitle.endsWith(".chd")) {
            return true;
        }
        if (uri == null) {
            return false;
        }
        String last = uri.getLastPathSegment();
        if (last != null && last.toLowerCase(Locale.US).endsWith(".chd")) {
            return true;
        }
        String uriString = uri.toString().toLowerCase(Locale.US);
        return uriString.endsWith(".chd") || uriString.contains(".chd?");
    }

    @Nullable
    static Pair<String, String> getPersistedChdMetadata(@Nullable Context ctx, @Nullable Uri uri) {
        if (ctx == null || uri == null) {
            return null;
        }
        Context appCtx = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        android.content.SharedPreferences prefs = appCtx.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE);
        String serial = prefs.getString(makeChdMetadataKey(PREF_CHD_SERIAL_PREFIX, uri), null);
        String title = prefs.getString(makeChdMetadataKey(PREF_CHD_TITLE_PREFIX, uri), null);
        if (TextUtils.isEmpty(serial) && TextUtils.isEmpty(title)) {
            return null;
        }
        return new Pair<>(serial, title);
    }

    // ---- Instance methods ---------------------------------------------------------

    void persistChdMetadata(@Nullable Uri uri, @Nullable String serial, @Nullable String title) {
        if (uri == null) {
            return;
        }
        android.content.SharedPreferences.Editor editor =
                host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).edit();
        String serialValue = TextUtils.isEmpty(serial) ? null : serial.trim();
        String titleValue = TextUtils.isEmpty(title) ? null : title.trim();
        String serialKey = makeChdMetadataKey(PREF_CHD_SERIAL_PREFIX, uri);
        String titleKey = makeChdMetadataKey(PREF_CHD_TITLE_PREFIX, uri);
        if (TextUtils.isEmpty(serialValue)) {
            editor.remove(serialKey);
        } else {
            editor.putString(serialKey, serialValue);
        }
        if (TextUtils.isEmpty(titleValue)) {
            editor.remove(titleKey);
        } else {
            editor.putString(titleKey, titleValue);
        }
        editor.apply();
    }

    String queryOpenableDisplayName(Uri uri) {
        try (android.database.Cursor c = host.getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (Exception ignored) {}
        return null;
    }

    void startPickIsoForChd() {
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            String[] mimeTypes = {
                "application/octet-stream",
                "application/x-iso9660-image",
                "application/x-cd-image",
                "application/x-raw-disk-image"
            };
            i.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
            host.launchIsoPickerIntent(i);
        } catch (Throwable t) {
            try { Toast.makeText(host, R.string.home_unable_open_file_picker, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
        }
    }

    void handlePickIsoResult(int resultCode, Intent data) {
        if (resultCode == android.app.Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            String name = queryOpenableDisplayName(uri);
            String low = name != null ? name.toLowerCase() : uri.toString().toLowerCase();
            if (!low.endsWith(".iso")) {
                try {
                    new MaterialAlertDialogBuilder(host)
                            .setTitle("Not an ISO")
                            .setMessage("Please select a .iso file.")
                            .setPositiveButton("OK", (d, w) -> d.dismiss())
                            .show();
                } catch (Throwable ignored) {}
                return;
            }
            performIsoToChd(uri, name);
        }
    }

    void handleSaveChdResult(int resultCode, Intent data) {
        if (pendingChdCachePath == null) {
            android.util.Log.w("ARMSX2_CHD", "Save handler invoked with no pending CHD path");
            return;
        }

        File chdFile = new File(pendingChdCachePath);
        String cachePath = pendingChdCachePath;
        pendingChdCachePath = null;
        String displayName = pendingChdDisplayName;
        pendingChdDisplayName = null;
        Uri sourceUri = pendingChdSourceUri;
        pendingChdSourceUri = null;
        String sourceSerial = pendingChdSourceSerial;
        pendingChdSourceSerial = null;
        String sourceTitle = pendingChdSourceTitle;
        pendingChdSourceTitle = null;

        if (!chdFile.exists()) {
            android.util.Log.e("ARMSX2_CHD", "Pending CHD file missing from cache: " + cachePath);
            showConversionResult(false, "Could not locate the converted CHD file. Please try converting again.");
            return;
        }

        if (resultCode == android.app.Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri destinationUri = data.getData();
            android.util.Log.d("ARMSX2_CHD", "User selected destination URI: " + destinationUri);
            boolean saved = saveChdToUri(chdFile, destinationUri);
            if (saved) {
                String destinationDisplayName = queryOpenableDisplayName(destinationUri);
                String persistedTitle = !TextUtils.isEmpty(sourceTitle) ? sourceTitle : stripFileExtension(destinationDisplayName);
                persistChdMetadata(destinationUri, sourceSerial, persistedTitle);
                host.carryCoverAssociationAfterChdSave(
                        sourceUri, destinationUri, displayName, destinationDisplayName, sourceSerial, persistedTitle);
                if (!chdFile.delete()) {
                    android.util.Log.w("ARMSX2_CHD", "Failed to delete cached CHD after saving: " + cachePath);
                } else {
                    android.util.Log.d("ARMSX2_CHD", "Deleted cached CHD after successful save");
                }
                showConversionResult(true, "CHD saved to the selected location.");
            } else {
                showConversionResult(false, "Failed to save CHD. The converted file is still available in the app cache:\n" + cachePath);
            }
        } else {
            android.util.Log.i("ARMSX2_CHD", "User cancelled CHD save dialog");
            showConversionResult(false, "Save cancelled. The converted CHD remains in the app cache:\n" + cachePath);
        }
    }

    private void performIsoToChd(Uri isoUri, String isoDisplayName) {
        if (!NativeApp.hasNativeTools) {
            String errorMsg = "ARMSX2 Native Tools library could not be called, it was probably not bundled with the app please rebuild the app with the library in place.";
            android.util.Log.e("ARMSX2_CHD", "Library not available: " + errorMsg);
            try {
                new MaterialAlertDialogBuilder(host)
                        .setTitle("Library Not Available")
                        .setMessage(errorMsg)
                        .setPositiveButton("OK", (d, w) -> d.dismiss())
                        .show();
            } catch (Throwable ignored) {}
            return;
        }

        new Thread(() -> {
            String inputPath = null;
            String outputPath = null;
            String resultMessage = null;
            boolean success = false;
            String sourceTitle = stripFileExtension(isoDisplayName);
            if (TextUtils.isEmpty(sourceTitle) && isoUri != null) {
                sourceTitle = stripFileExtension(isoUri.getLastPathSegment());
            }
            String sourceSerial = GameScanner.parseSerialFromString(sourceTitle);

            try {
                if (TextUtils.isEmpty(sourceSerial)) {
                    try {
                        sourceSerial = GameScanner.tryExtractIsoSerial(host.getContentResolver(), isoUri);
                    } catch (Throwable ignored) {}
                }

                android.util.Log.i("ARMSX2_CHD", "Starting ISO to CHD conversion for: " + isoDisplayName);
                android.util.Log.i("ARMSX2_CHD", "Input URI: " + isoUri.toString());

                inputPath = getFilePathFromUri(isoUri);
                if (inputPath == null) {
                    resultMessage = "Could not access the selected ISO file. Please ensure the file is accessible.";
                    android.util.Log.e("ARMSX2_CHD", "Failed to get file path from URI: " + isoUri.toString());
                    return;
                }
                android.util.Log.i("ARMSX2_CHD", "Input path resolved to: " + inputPath);

                outputPath = inputPath.replaceAll("\\.iso$", ".chd");
                android.util.Log.i("ARMSX2_CHD", "Expected output path: " + outputPath);

                android.util.Log.i("ARMSX2_CHD", "Calling native conversion...");
                try {
                    android.util.Log.d("ARMSX2_CHD", "Input path bytes: " + java.util.Arrays.toString(inputPath.getBytes("UTF-8")));
                    android.util.Log.d("ARMSX2_CHD", "Input path length: " + inputPath.length());
                    android.util.Log.d("ARMSX2_CHD", "Input path string: '" + inputPath + "'");
                } catch (java.io.UnsupportedEncodingException e) {
                    android.util.Log.e("ARMSX2_CHD", "Failed to encode path as UTF-8: " + e.getMessage());
                }
                int result = NativeApp.convertIsoToChd(inputPath);
                android.util.Log.i("ARMSX2_CHD", "Native conversion returned code: " + result);

                success = handleConversionResult(result, inputPath, outputPath);

                if (success) {
                    final String chdCachePath = outputPath;
                    final String chdDisplayName = isoDisplayName;
                    final String finalSourceSerial = sourceSerial;
                    final String finalSourceTitle = sourceTitle;
                    android.util.Log.i("ARMSX2_CHD", "Conversion succeeded. Prompting user to choose CHD save location.");
                    host.runOnUiThread(() -> promptForChdSave(
                            chdCachePath, chdDisplayName, isoUri, finalSourceSerial, finalSourceTitle));
                    resultMessage = null;
                } else {
                    resultMessage = getErrorMessage(result) + "\n\nInput: " + inputPath + "\nOutput: " + outputPath;
                    android.util.Log.e("ARMSX2_CHD", "Conversion failed with code " + result + ": " + getErrorMessage(result));
                }

            } catch (Throwable e) {
                resultMessage = "Conversion failed with exception: " + e.getMessage()
                        + "\n\nInput: " + inputPath + "\nOutput: " + outputPath;
                android.util.Log.e("ARMSX2_CHD", "Conversion exception: " + e.getClass().getSimpleName() + ": " + e.getMessage(), e);
            } finally {
                if (inputPath != null) {
                    File tempFile = new File(inputPath);
                    if (tempFile.exists() && tempFile.getParent().equals(host.getCacheDir().getAbsolutePath())) {
                        if (tempFile.delete()) {
                            android.util.Log.d("ARMSX2_CHD", "Cleaned up temporary file: " + inputPath);
                        } else {
                            android.util.Log.w("ARMSX2_CHD", "Failed to clean up temporary file: " + inputPath);
                        }
                    }
                }
                final String finalMessage = resultMessage;
                final boolean finalSuccess = success;
                host.runOnUiThread(() -> {
                    if (finalMessage != null) {
                        showConversionResult(finalSuccess, finalMessage);
                    }
                });
            }
        }, "IsoToChdConverter").start();
    }

    private String getFilePathFromUri(Uri uri) {
        android.util.Log.d("ARMSX2_CHD", "getFilePathFromUri called with: " + uri.toString());
        try {
            android.database.Cursor cursor = host.getContentResolver().query(uri, null, null, null, null);
            if (cursor != null) {
                try {
                    if (cursor.moveToFirst()) {
                        int displayNameIndex = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                        if (displayNameIndex >= 0) {
                            String displayName = cursor.getString(displayNameIndex);
                            File cacheDir = host.getCacheDir();
                            File tempFile = new File(cacheDir, displayName);
                            android.util.Log.d("ARMSX2_CHD", "Creating temporary file: " + tempFile.getAbsolutePath());
                            try (java.io.InputStream input = host.getContentResolver().openInputStream(uri);
                                 java.io.FileOutputStream output = new java.io.FileOutputStream(tempFile)) {
                                if (input != null) {
                                    byte[] buffer = new byte[8192];
                                    int bytesRead;
                                    long totalBytes = 0;
                                    while ((bytesRead = input.read(buffer)) != -1) {
                                        output.write(buffer, 0, bytesRead);
                                        totalBytes += bytesRead;
                                    }
                                    android.util.Log.d("ARMSX2_CHD", "Copied " + totalBytes + " bytes to cache");
                                    return tempFile.getAbsolutePath();
                                }
                            }
                        }
                    }
                } finally {
                    cursor.close();
                }
            }
        } catch (Throwable e) {
            android.util.Log.e("ARMSX2_CHD", "Exception in getFilePathFromUri: " + e.getMessage(), e);
        }
        android.util.Log.w("ARMSX2_CHD", "getFilePathFromUri returning null - failed to resolve path");
        return null;
    }

    void promptForChdSave(String chdCachePath, String displayName,
                          @Nullable Uri sourceUri,
                          @Nullable String sourceSerial,
                          @Nullable String sourceTitle) {
        File chdFile = new File(chdCachePath);
        if (!chdFile.exists()) {
            android.util.Log.e("ARMSX2_CHD", "CHD file missing in cache, cannot prompt for save: " + chdCachePath);
            showConversionResult(false, "Converted file could not be found. Please try converting again.");
            return;
        }

        pendingChdCachePath = chdCachePath;
        pendingChdDisplayName = displayName;
        pendingChdSourceUri = sourceUri;
        pendingChdSourceSerial = sourceSerial;
        pendingChdSourceTitle = sourceTitle;

        String baseName = displayName;
        if (baseName == null || baseName.trim().isEmpty()) {
            baseName = chdFile.getName();
        }
        String lower = baseName.toLowerCase(Locale.US);
        if (lower.endsWith(".iso")) {
            baseName = baseName.substring(0, baseName.length() - 4);
            lower = baseName.toLowerCase(Locale.US);
        }
        if (!lower.endsWith(".chd")) {
            baseName = baseName + ".chd";
        }

        android.util.Log.d("ARMSX2_CHD", "Prompting user to save CHD as: " + baseName);

        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        intent.putExtra(Intent.EXTRA_TITLE, baseName);

        host.launchSaveChdIntent(intent);
    }

    boolean saveChdToUri(File chdFile, Uri destinationUri) {
        android.util.Log.d("ARMSX2_CHD", "Saving CHD from cache to destination: " + destinationUri);
        try (java.io.FileInputStream input = new java.io.FileInputStream(chdFile);
             java.io.OutputStream output = host.getContentResolver().openOutputStream(destinationUri, "w")) {
            if (output == null) {
                android.util.Log.e("ARMSX2_CHD", "Content resolver returned null output stream for destination");
                return false;
            }
            byte[] buffer = new byte[8192];
            int bytesRead;
            long totalBytes = 0;
            while ((bytesRead = input.read(buffer)) != -1) {
                output.write(buffer, 0, bytesRead);
                totalBytes += bytesRead;
            }
            output.flush();
            android.util.Log.d("ARMSX2_CHD", "Wrote " + totalBytes + " bytes to destination URI");
            return true;
        } catch (Throwable e) {
            android.util.Log.e("ARMSX2_CHD", "Failed to copy CHD to destination: " + e.getMessage(), e);
            return false;
        }
    }

    private boolean handleConversionResult(int result, String inputPath, String outputPath) {
        return result == 0;
    }

    private String getErrorMessage(int errorCode) {
        switch (errorCode) {
            case -1: return "Error: Null pointer provided to conversion function";
            case -2: return "Error: Invalid UTF-8 encoding in file paths";
            case -3: return "Error: Input ISO file not found";
            case -4: return "Error: Input path is not a regular file";
            case -5: return "Error: Failed to create output CHD file";
            case -6: return "Error: I/O error during conversion";
            case -7: return "Error: Too many hunks for CHD format";
            case -8: return "Error: Numeric overflow during conversion";
            case -9: return "Error: Unexpected end of ISO data";
            case -100: return "Error: Internal conversion error";
            default: return "Error: Unknown conversion error (code: " + errorCode + ")";
        }
    }

    private void showConversionResult(boolean success, String message) {
        try {
            new MaterialAlertDialogBuilder(host)
                    .setTitle(success ? "Conversion Successful" : "Conversion Failed")
                    .setMessage(message)
                    .setPositiveButton("OK", (d, w) -> d.dismiss())
                    .show();
        } catch (Throwable ignored) {}
    }
}
