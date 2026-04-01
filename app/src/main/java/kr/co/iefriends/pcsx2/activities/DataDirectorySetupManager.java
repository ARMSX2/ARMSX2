package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.ProgressBar;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.File;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;
import kr.co.iefriends.pcsx2.utils.LogcatRecorder;

class DataDirectorySetupManager {
    private final MainActivity host;

    // State
    private boolean storagePromptShown = false;
    private boolean onboardingLaunched = false;
    private boolean postOnboardingChecksRun = false;
    private AlertDialog dataDirProgressDialog;

    DataDirectorySetupManager(MainActivity host) {
        this.host = host;
    }

    boolean isOnboardingComplete() {
        return host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                .getBoolean("onboarding_complete", false);
    }

    void setOnboardingComplete() {
        host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                .edit().putBoolean("onboarding_complete", true).apply();
    }

    void maybeStartOnboardingFlow() {
        if (postOnboardingChecksRun) {
            return;
        }
        if (isOnboardingComplete()) {
            runPostOnboardingPrompts();
            return;
        }
        if (onboardingLaunched) {
            return;
        }
        try {
            Intent onboardingIntent = new Intent(host, OnboardingActivity.class);
            host.launchOnboardingIntent(onboardingIntent);
            onboardingLaunched = true;
        } catch (Throwable t) {
            setOnboardingComplete();
            runPostOnboardingPrompts();
        }
    }

    void handleOnboardingResult(int resultCode) {
        onboardingLaunched = false;
        if (resultCode == android.app.Activity.RESULT_OK) {
            setOnboardingComplete();
            runPostOnboardingPrompts();
        } else if (!isOnboardingComplete()) {
            maybeStartOnboardingFlow();
        }
    }

    void runPostOnboardingPrompts() {
        if (postOnboardingChecksRun) {
            return;
        }
        postOnboardingChecksRun = true;
        host.ensureBiosPresent();
        maybeShowDataDirectoryPrompt();
    }

    void maybeShowDataDirectoryPrompt() {
        if (storagePromptShown) {
            return;
        }
        if (DataDirectoryManager.isPromptDone(host)) {
            return;
        }
        storagePromptShown = true;
        new MaterialAlertDialogBuilder(host)
                .setTitle("Storage location")
                .setMessage("Do you wish to change where the emulator stores its data?")
                .setNegativeButton("Cancel", (dialog, which) -> {
                    DataDirectoryManager.markPromptDone(host);
                    dialog.dismiss();
                })
                .setPositiveButton("Choose", (dialog, which) -> {
                    dialog.dismiss();
                    launchDataDirectoryPicker();
                })
                .setOnDismissListener(dialog -> storagePromptShown = true)
                .show();
    }

    void launchDataDirectoryPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        host.launchDataDirPickerIntent(intent);
    }

    void handleDataDirectorySelection(@NonNull Uri tree) {
        String resolvedPath = DataDirectoryManager.resolveTreeUriToPath(host, tree);
        if (resolvedPath == null || resolvedPath.trim().isEmpty()) {
            try {
                Toast.makeText(host, R.string.onboarding_storage_unusable, Toast.LENGTH_LONG).show();
            } catch (Throwable ignored) {}
            storagePromptShown = false;
            maybeShowDataDirectoryPrompt();
            return;
        }
        File targetDir = new File(resolvedPath);
        if (!targetDir.exists() && !targetDir.mkdirs()) {
            try {
                Toast.makeText(host, R.string.onboarding_storage_create_failed, Toast.LENGTH_LONG).show();
            } catch (Throwable ignored) {}
            storagePromptShown = false;
            maybeShowDataDirectoryPrompt();
            return;
        }
        if (!DataDirectoryManager.canUseDirectFileAccess(targetDir)) {
            host.showStorageAccessError(targetDir);
            storagePromptShown = false;
            maybeShowDataDirectoryPrompt();
            return;
        }
        File currentDir = DataDirectoryManager.getDataRoot(host.getApplicationContext());
        if (currentDir != null && currentDir.getAbsolutePath().equals(targetDir.getAbsolutePath())) {
            DataDirectoryManager.storeCustomDataRoot(host.getApplicationContext(), targetDir.getAbsolutePath(), tree.toString());
            NativeApp.setDataRootOverride(targetDir.getAbsolutePath());
            DataDirectoryManager.markPromptDone(host);
            storagePromptShown = true;
            try {
                Toast.makeText(host, R.string.onboarding_storage_already_using, Toast.LENGTH_SHORT).show();
            } catch (Throwable ignored) {}
            return;
        }
        beginDataDirectoryMigration(currentDir, targetDir, tree.toString());
    }

    private void beginDataDirectoryMigration(@NonNull File currentDir, @NonNull File targetDir, @NonNull String uriString) {
        showDataDirProgressDialog();
        NativeApp.pause();
        NativeApp.shutdown();
        new Thread(() -> {
            boolean success = DataDirectoryManager.migrateData(currentDir, targetDir);
            if (success) {
                DataDirectoryManager.storeCustomDataRoot(host.getApplicationContext(), targetDir.getAbsolutePath(), uriString);
                NativeApp.setDataRootOverride(targetDir.getAbsolutePath());
                NativeApp.reinitializeDataRoot(targetDir.getAbsolutePath());
                LogcatRecorder.handleDataRootChanged();
                DataDirectoryManager.copyAssetAll(host.getApplicationContext(), "resources");
            }
            host.runOnUiThread(() -> {
                dismissDataDirProgressDialog();
                if (success) {
                    DataDirectoryManager.markPromptDone(host);
                    storagePromptShown = true;
                    try {
                        Toast.makeText(host, R.string.onboarding_storage_moved, Toast.LENGTH_LONG).show();
                    } catch (Throwable ignored) {}
                } else {
                    try {
                        Toast.makeText(host, R.string.onboarding_storage_move_failed, Toast.LENGTH_LONG).show();
                    } catch (Throwable ignored) {}
                    storagePromptShown = false;
                    maybeShowDataDirectoryPrompt();
                }
            });
        }, "DataDirMigration").start();
    }

    private void showDataDirProgressDialog() {
        host.runOnUiThread(() -> {
            if (dataDirProgressDialog != null && dataDirProgressDialog.isShowing()) {
                return;
            }
            ProgressBar progressBar = new ProgressBar(host);
            int padding = host.dpToPx(24);
            progressBar.setPadding(padding, padding, padding, padding);
            dataDirProgressDialog = new MaterialAlertDialogBuilder(host)
                    .setTitle("Moving data")
                    .setMessage("Moving emulator data to the selected folder…")
                    .setView(progressBar)
                    .setCancelable(false)
                    .create();
            dataDirProgressDialog.show();
        });
    }

    private void dismissDataDirProgressDialog() {
        host.runOnUiThread(() -> {
            if (dataDirProgressDialog != null) {
                dataDirProgressDialog.dismiss();
                dataDirProgressDialog = null;
            }
        });
    }
}
