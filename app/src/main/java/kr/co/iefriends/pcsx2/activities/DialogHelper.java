package kr.co.iefriends.pcsx2.activities;

import android.content.Intent;
import android.view.View;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;

class DialogHelper {
    private final MainActivity host;

    DialogHelper(MainActivity host) {
        this.host = host;
    }

    void showGameStateDialog() {
        CharSequence[] items = new CharSequence[]{
                host.getString(R.string.home_game_state_save_slot_1),
                host.getString(R.string.home_game_state_load_slot_1)
        };
        new MaterialAlertDialogBuilder(host)
                .setTitle(R.string.home_game_state_title)
                .setItems(items, (dialog, which) -> {
                    if (which == 0) {
                        pauseVmForStateOperation();
                        boolean ok = NativeApp.saveStateToSlot(1);
                        try {
                            Toast.makeText(host, ok ? R.string.home_game_state_saved : R.string.home_game_state_save_failed, Toast.LENGTH_SHORT).show();
                        } catch (Throwable ignored) {}
                        resumeVmAfterStateOperation();
                    } else if (which == 1) {
                        pauseVmForStateOperation();
                        boolean ok = NativeApp.loadStateFromSlot(1);
                        try {
                            Toast.makeText(host, ok ? R.string.home_game_state_loaded : R.string.home_game_state_load_failed, Toast.LENGTH_SHORT).show();
                        } catch (Throwable ignored) {}
                        resumeVmAfterStateOperation();
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    void showAboutDialog() {
        String versionName = "";
        try {
            versionName = host.getPackageManager().getPackageInfo(host.getPackageName(), 0).versionName;
        } catch (Exception ignored) {}
        String message = "ARMSX2 (" + versionName + ")\n" +
                "by ARMSX2 team\n\n" +
                "Core contributors:\n" +
                "- MoonPower — App developer\n" +
                "- jpolo — Management\n" +
                "- Medievalshell — Web developer\n" +
                "- set l — Web developer\n" +
                "- Alex — QA tester\n" +
                "- Yua — QA tester\n\n" +
                "Thanks to:\n" +
                "- pontos2024 (emulator base)\n" +
                "- PCSX2 v2.3.430 (core emulator)\n" +
                "- SDL (SDL3)\n" +
                "- Fffathur (icon design)\n" +
                "- vivimagic0 (icon design)";
        new MaterialAlertDialogBuilder(host)
                .setTitle("About")
                .setMessage(message)
                .setPositiveButton(android.R.string.ok, (d, w) -> d.dismiss())
                .show();
    }

    void showSettingsDialog() {
        View view = host.getLayoutInflater().inflate(R.layout.dialog_settings, null);
        AlertDialog dialog = new MaterialAlertDialogBuilder(host)
                .setView(view)
                .create();

        View btnClose = view.findViewById(R.id.btn_close);
        if (btnClose != null) btnClose.setOnClickListener(v -> dialog.dismiss());

        View rg = view.findViewById(R.id.rg_aspect);
        if (rg instanceof android.widget.RadioGroup) {
            ((android.widget.RadioGroup) rg).setOnCheckedChangeListener((group, checkedId) -> {
                int type = 1;
                if (checkedId == R.id.rb_ar_4_3) type = 2;
                else if (checkedId == R.id.rb_ar_16_9) type = 3;
                else type = 1;
                NativeApp.setAspectRatio(type);
            });
        }

        View swFps = view.findViewById(R.id.switch_osd_fps);
        if (swFps instanceof android.widget.Switch) {
            ((android.widget.Switch) swFps).setChecked(false);
            ((android.widget.Switch) swFps).setOnCheckedChangeListener((buttonView, isChecked) ->
                    NativeApp.setSetting("EmuCore/GS", "OsdShowFPS", "bool", isChecked ? "true" : "false"));
        }
        View swRes = view.findViewById(R.id.switch_osd_res);
        if (swRes != null) swRes.setVisibility(View.GONE);
        View swStats = view.findViewById(R.id.switch_osd_stats);
        if (swStats != null) swStats.setVisibility(View.GONE);

        View swHw = view.findViewById(R.id.switch_hw_readbacks);
        if (swHw instanceof android.widget.Switch) {
            ((android.widget.Switch) swHw).setChecked(true);
            ((android.widget.Switch) swHw).setOnCheckedChangeListener((buttonView, isChecked) ->
                    NativeApp.setSetting("EmuCore/GS", "HardwareReadbacks", "bool", isChecked ? "true" : "false"));
        }

        View btnImportMc = view.findViewById(R.id.btn_import_memcard);
        if (btnImportMc != null) {
            btnImportMc.setOnClickListener(v -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false);
                i.setType("application/octet-stream");
                String[] types = new String[]{"application/octet-stream", "application/x-binary"};
                i.putExtra(Intent.EXTRA_MIME_TYPES, types);
                host.startActivityForResult(Intent.createChooser(i, "Select memory card"), 9911);
            });
        }

        View btnAbout = view.findViewById(R.id.btn_about);
        if (btnAbout != null) {
            btnAbout.setOnClickListener(v -> {
                String vn = "";
                try { vn = host.getPackageManager().getPackageInfo(host.getPackageName(), 0).versionName; } catch (Exception ignored) {}
                String msg = "ARMSX2 (" + vn + ")\n" +
                        "by ARMSX2 team\n\n" +
                        "Core contributors:\n" +
                        "- MoonPower — App developer\n" +
                        "- jpolo — Management\n" +
                        "- Medievalshell — Web developer\n" +
                        "- set l — Web developer\n" +
                        "- Alex — QA tester\n" +
                        "- Yua — QA tester\n\n" +
                        "Thanks to:\n" +
                        "- pontos2024 (emulator base)\n" +
                        "- PCSX2 v2.3.430 (core emulator)\n" +
                        "- SDL (SDL3)\n" +
                        "- Fffathur (icon design)\n" +
                        "- vivimagic0 (icon design)";
                new MaterialAlertDialogBuilder(host)
                        .setTitle("About")
                        .setMessage(msg)
                        .setPositiveButton("OK", (d, w) -> d.dismiss())
                        .show();
            });
        }

        dialog.show();
    }

    private void pauseVmForStateOperation() {
        try {
            NativeApp.pause();
            android.os.SystemClock.sleep(50);
            NativeApp.resetKeyStatus();
        } catch (Throwable ignored) {}
    }

    private void resumeVmAfterStateOperation() {
        try {
            android.os.SystemClock.sleep(30);
            NativeApp.resume();
            host.isVmPaused = false;
            host.updatePauseButtonIcon();
        } catch (Throwable ignored) {}
    }
}
