package kr.co.iefriends.pcsx2.activities;

import android.app.GameManager;
import android.app.GameState;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.widget.Toast;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;

class BiosManager {
    private final MainActivity host;

    BiosManager(MainActivity host) {
        this.host = host;
    }

    boolean hasBios() {
        File base = DataDirectoryManager.getDataRoot(host.getApplicationContext());
        File biosDir = new File(base, "bios");
        if (!biosDir.exists()) return false;
        File[] files = biosDir.listFiles((dir, name) -> name != null && name.toLowerCase().endsWith(".bin"));
        return files != null && files.length > 0;
    }

    void ensureBiosPresent() {
        if (!hasBios()) {
            Toast.makeText(host, R.string.home_bios_missing_toast, Toast.LENGTH_LONG).show();
            new MaterialAlertDialogBuilder(host)
                .setMessage(R.string.home_bios_missing_message)
                .setCancelable(true)
                .setNegativeButton(R.string.home_close, (d, w) -> d.dismiss())
                .setPositiveButton(R.string.onboarding_bios_select, (d, w) -> host.openBiosPicker())
                .show();
        } else {
            // BIOS is present, signal we're gameplay-ready.
            if (Build.VERSION.SDK_INT >= 33) {
                try {
                    GameManager gm = (GameManager) host.getSystemService(Context.GAME_SERVICE);
                    if (gm != null) gm.setGameState(new GameState(false, GameState.MODE_GAMEPLAY_INTERRUPTIBLE));
                } catch (Throwable ignored) {}
            }
        }
    }

    void saveBiosFromUri(Uri uri) {
        Context ctx = host.getApplicationContext();
        File base = DataDirectoryManager.getDataRoot(ctx);
        File biosDir = new File(base, "bios");
        if (!biosDir.exists()) biosDir.mkdirs();

        String outName = "ps2_bios.bin";
        File outFile = new File(biosDir, outName);

        try (InputStream in = host.getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(outFile)) {
            if (in == null) throw new IOException("Unable to open BIOS Uri");
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            Toast.makeText(host, R.string.onboarding_bios_saved, Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(host, R.string.onboarding_bios_write_error, Toast.LENGTH_LONG).show();
        }
    }

    void importBiosFromUri(Uri uri) {
        Context ctx = host.getApplicationContext();
        File base = DataDirectoryManager.getDataRoot(ctx);
        File biosDir = new File(base, "bios");
        if (!biosDir.exists()) biosDir.mkdirs();

        String name = "ps2_bios.bin";
        try {
            if ("content".equalsIgnoreCase(uri.getScheme())) {
                try (android.database.Cursor c = host.getContentResolver().query(uri, new String[]{android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst()) {
                        String dn = c.getString(0);
                        if (dn != null && !dn.trim().isEmpty()) name = dn.trim();
                    }
                }
            } else {
                String p = uri.getPath();
                if (p != null) {
                    int idx = p.lastIndexOf('/');
                    if (idx >= 0 && idx + 1 < p.length()) name = p.substring(idx + 1);
                }
            }
        } catch (Throwable ignored) {}
        if (!name.toLowerCase().endsWith(".bin")) name = name + ".bin";

        // Avoid overwrite
        File outFile = new File(biosDir, name);
        int suffix = 1;
        while (outFile.exists()) {
            String baseName = name;
            String stem = baseName;
            String ext = "";
            int dot = baseName.lastIndexOf('.');
            if (dot > 0) { stem = baseName.substring(0, dot); ext = baseName.substring(dot); }
            outFile = new File(biosDir, stem + " (" + suffix + ")" + ext);
            suffix++;
        }

        try (InputStream in = host.getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(outFile)) {
            if (in == null) throw new IOException("Unable to open BIOS Uri");
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            Toast.makeText(host, host.getString(R.string.home_bios_imported, outFile.getName()), Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(host, R.string.home_bios_import_failed, Toast.LENGTH_LONG).show();
        }
    }

    void showBiosManagerDialog() {
        Context ctx = host.getApplicationContext();
        File base = DataDirectoryManager.getDataRoot(ctx);
        File biosDir = new File(base, "bios");
        if (!biosDir.exists()) biosDir.mkdirs();
        File[] files = biosDir.listFiles((dir, name) -> name != null && name.toLowerCase().endsWith(".bin"));
        java.util.List<File> biosList = new java.util.ArrayList<>();
        if (files != null) java.util.Collections.addAll(biosList, files);

        final String[] names = new String[biosList.size()];
        for (int i = 0; i < biosList.size(); i++) names[i] = biosList.get(i).getName();
        int checked = -1;
        try {
            String cur = NativeApp.getSetting("Filenames", "BIOS", "string");
            if (cur != null && !cur.isEmpty()) {
                for (int i = 0; i < biosList.size(); i++) {
                    if (new File(cur).getAbsolutePath().equals(biosList.get(i).getAbsolutePath())) {
                        checked = i;
                        break;
                    }
                }
            }
        } catch (Throwable ignored) {}

        MaterialAlertDialogBuilder b = new MaterialAlertDialogBuilder(host)
                .setTitle(R.string.home_bios_selection_title)
                .setSingleChoiceItems(names, checked, (d, which) -> {
                    try {
                        String path = biosList.get(which).getAbsolutePath();
                        NativeApp.setSetting("Filenames", "BIOS", "string", path);
                        Toast.makeText(host, host.getString(R.string.home_bios_current, biosList.get(which).getName()), Toast.LENGTH_SHORT).show();
                    } catch (Throwable ignored) {}
                })
                .setNegativeButton(R.string.home_close, (d, w) -> d.dismiss())
                .setPositiveButton(R.string.home_import, (d, w) -> host.openBiosImportForManager());
        b.show();
    }
}
