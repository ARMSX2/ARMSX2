package kr.co.iefriends.pcsx2.activities;

import android.content.DialogInterface;
import android.net.Uri;
import android.text.TextUtils;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Spinner;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.materialswitch.MaterialSwitch;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.GameSpecificSettingsManager;

class PerGameSettingsManager {
    private final MainActivity host;

    private boolean perGameOverridesActive = false;
    @Nullable private PerGameOverrideSnapshot lastOverrideSnapshot = null;
    @Nullable private String lastOverrideKey = null;

    PerGameSettingsManager(MainActivity host) {
        this.host = host;
    }

    void showDialog(GameEntry entry) {
        if (entry == null) return;
        String gameKey = MainActivity.gameKeyFromEntry(entry);
        View dialogView = LayoutInflater.from(host).inflate(R.layout.dialog_game_specific_settings, null);

        MaterialSwitch switchEnabled = dialogView.findViewById(R.id.per_game_switch_enabled);
        ViewGroup settingsGroup = dialogView.findViewById(R.id.per_game_settings_group);
        Spinner rendererSpinner = dialogView.findViewById(R.id.per_game_spinner_renderer);
        Spinner aspectSpinner = dialogView.findViewById(R.id.per_game_spinner_aspect_ratio);
        MaterialSwitch switchWidescreen = dialogView.findViewById(R.id.per_game_switch_widescreen);
        MaterialSwitch switchCheats = dialogView.findViewById(R.id.per_game_switch_enable_cheats);
        MaterialSwitch switchNoInterlacing = dialogView.findViewById(R.id.per_game_switch_no_interlacing);
        MaterialSwitch switchLoadTextures = dialogView.findViewById(R.id.per_game_switch_load_textures);
        MaterialSwitch switchAsyncTextures = dialogView.findViewById(R.id.per_game_switch_async_textures);
        MaterialSwitch switchPrecache = dialogView.findViewById(R.id.per_game_switch_precache_textures);
        MaterialSwitch switchShowFps = dialogView.findViewById(R.id.per_game_switch_show_fps);

        boolean globalCheats = host.readBoolSetting("EmuCore", "EnableCheats", false);
        boolean globalWidescreen = host.readBoolSetting("EmuCore", "EnableWideScreenPatches", false);
        boolean globalNoInterlacing = host.readBoolSetting("EmuCore", "EnableNoInterlacingPatches", false);
        boolean globalLoadTextures = host.readBoolSetting("EmuCore/GS", "LoadTextureReplacements", false);
        boolean globalAsyncTextures = host.readBoolSetting("EmuCore/GS", "LoadTextureReplacementsAsync", false);
        boolean globalPrecache = host.readBoolSetting("EmuCore/GS", "PrecacheTextureReplacements", false);
        boolean globalShowFps = host.readBoolSetting("EmuCore/GS", "OsdShowFPS", false);
        int globalRenderer = getCurrentRendererValue();
        String globalAspect = getCurrentAspectRatioValue();

        GameSpecificSettingsManager.GameSettings existing = GameSpecificSettingsManager.getSettings(host, gameKey);

        boolean initialEnabled = existing != null;
        boolean initialCheats = existing != null && existing.enableCheats != null ? existing.enableCheats : globalCheats;
        boolean initialWidescreen = existing != null && existing.widescreen != null ? existing.widescreen : globalWidescreen;
        boolean initialNoInterlacing = existing != null && existing.noInterlacing != null ? existing.noInterlacing : globalNoInterlacing;
        boolean initialLoadTextures = existing != null && existing.loadTextures != null ? existing.loadTextures : globalLoadTextures;
        boolean initialAsyncTextures = existing != null && existing.asyncTextures != null ? existing.asyncTextures : globalAsyncTextures;
        boolean initialPrecache = existing != null && existing.precacheTextures != null ? existing.precacheTextures : globalPrecache;
        boolean initialShowFps = existing != null && existing.showFps != null ? existing.showFps : globalShowFps;
        int initialRenderer = existing != null && existing.renderer != null ? existing.renderer : globalRenderer;
        String initialAspect = existing != null && !TextUtils.isEmpty(existing.aspectRatio) ? existing.aspectRatio : globalAspect;

        switchEnabled.setChecked(initialEnabled);
        switchCheats.setChecked(initialCheats);
        switchWidescreen.setChecked(initialWidescreen);
        switchNoInterlacing.setChecked(initialNoInterlacing);
        switchLoadTextures.setChecked(initialLoadTextures);
        switchAsyncTextures.setChecked(initialAsyncTextures);
        switchPrecache.setChecked(initialPrecache);
        switchShowFps.setChecked(initialShowFps);

        rendererSpinner.setSelection(rendererSpinnerPositionForValue(initialRenderer), false);

        String[] aspectOptions = host.getResources().getStringArray(R.array.aspect_ratios);
        int aspectIndex = 0;
        for (int i = 0; i < aspectOptions.length; i++) {
            if (TextUtils.equals(aspectOptions[i], initialAspect)) {
                aspectIndex = i;
                break;
            }
        }
        aspectSpinner.setSelection(aspectIndex, false);

        setGroupEnabled(settingsGroup, initialEnabled);

        switchEnabled.setOnCheckedChangeListener((button, isChecked) -> setGroupEnabled(settingsGroup, isChecked));

        MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(host)
                .setTitle(entry.gameTitle != null ? entry.gameTitle : entry.title)
                .setView(dialogView)
                .setNegativeButton(android.R.string.cancel, (d, w) -> d.dismiss())
                .setPositiveButton(R.string.action_save, null);

        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(dlg -> {
            android.widget.Button saveButton = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
            if (saveButton == null) {
                return;
            }
            saveButton.setOnClickListener(v -> {
                if (!switchEnabled.isChecked()) {
                    GameSpecificSettingsManager.removeSettings(host, gameKey);
                    try { Toast.makeText(host, R.string.per_game_settings_cleared_toast, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                    dialog.dismiss();
                    return;
                }

                GameSpecificSettingsManager.GameSettings toSave = new GameSpecificSettingsManager.GameSettings();

                boolean cheatsValue = switchCheats.isChecked();
                if (cheatsValue != globalCheats) toSave.enableCheats = cheatsValue;

                boolean widescreenValue = switchWidescreen.isChecked();
                if (widescreenValue != globalWidescreen) toSave.widescreen = widescreenValue;

                boolean noInterlacingValue = switchNoInterlacing.isChecked();
                if (noInterlacingValue != globalNoInterlacing) toSave.noInterlacing = noInterlacingValue;

                boolean loadTexturesValue = switchLoadTextures.isChecked();
                if (loadTexturesValue != globalLoadTextures) toSave.loadTextures = loadTexturesValue;

                boolean asyncTexturesValue = switchAsyncTextures.isChecked();
                if (asyncTexturesValue != globalAsyncTextures) toSave.asyncTextures = asyncTexturesValue;

                boolean precacheValue = switchPrecache.isChecked();
                if (precacheValue != globalPrecache) toSave.precacheTextures = precacheValue;

                boolean showFpsValue = switchShowFps.isChecked();
                if (showFpsValue != globalShowFps) toSave.showFps = showFpsValue;

                int rendererValue = rendererValueForSpinnerPosition(rendererSpinner.getSelectedItemPosition());
                if (rendererValue != globalRenderer) toSave.renderer = rendererValue;

                String aspectValue = aspectOptions[aspectSpinner.getSelectedItemPosition()];
                if (!TextUtils.equals(aspectValue, globalAspect)) toSave.aspectRatio = aspectValue;

                if (toSave.hasOverrides()) {
                    GameSpecificSettingsManager.saveSettings(host, gameKey, toSave);
                    try { Toast.makeText(host, R.string.per_game_settings_saved_toast, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                } else {
                    GameSpecificSettingsManager.removeSettings(host, gameKey);
                    try { Toast.makeText(host, R.string.per_game_settings_cleared_toast, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                }
                dialog.dismiss();
            });
        });

        dialog.show();
    }

    private void setGroupEnabled(@Nullable ViewGroup group, boolean enabled) {
        if (group == null) {
            return;
        }
        group.setEnabled(enabled);
        group.setAlpha(enabled ? 1f : 0.38f);
        for (int i = 0; i < group.getChildCount(); i++) {
            View child = group.getChildAt(i);
            child.setEnabled(enabled);
            if (child instanceof ViewGroup) {
                setGroupEnabled((ViewGroup) child, enabled);
            }
        }
    }

    private int rendererSpinnerPositionForValue(int value) {
        switch (value) {
            case 12: return 1;
            case 13: return 2;
            case 14: return 3;
            default: return 0;
        }
    }

    private int rendererValueForSpinnerPosition(int position) {
        switch (position) {
            case 1: return 12;
            case 2: return 13;
            case 3: return 14;
            default: return -1;
        }
    }

    private int getCurrentRendererValue() {
        int initialValue = -1;
        try {
            String renderer = NativeApp.getSetting("EmuCore/GS", "Renderer", "int");
            if (!TextUtils.isEmpty(renderer)) {
                initialValue = Integer.parseInt(renderer);
            }
        } catch (Exception ignored) {}
        return initialValue;
    }

    private String getCurrentAspectRatioValue() {
        String[] aspectOptions = host.getResources().getStringArray(R.array.aspect_ratios);
        String defaultValue = aspectOptions.length > 1 ? aspectOptions[1] : aspectOptions[0];
        try {
            String aspect = NativeApp.getSetting("EmuCore/GS", "AspectRatio", "string");
            if (!TextUtils.isEmpty(aspect)) {
                return aspect;
            }
        } catch (Exception ignored) {}
        return defaultValue;
    }

    void applyForEntry(@Nullable GameEntry entry) {
        if (entry == null) return;
        applyForKey(MainActivity.gameKeyFromEntry(entry));
    }

    void applyForUri(@Nullable Uri uri) {
        applyForKey(uri != null ? uri.toString() : null);
    }

    void applyForKey(@Nullable String gameKey) {
        restoreOverrides();
        if (TextUtils.isEmpty(gameKey)) {
            return;
        }
        GameSpecificSettingsManager.GameSettings settings = GameSpecificSettingsManager.getSettings(host, gameKey);
        if (settings == null || !settings.hasOverrides()) {
            return;
        }

        PerGameOverrideSnapshot snapshot = captureSnapshot();
        boolean applied = false;

        if (settings.enableCheats != null) {
            setNativeSetting("EmuCore", "EnableCheats", "bool", boolToString(settings.enableCheats));
            applied = true;
        }
        if (settings.widescreen != null) {
            setNativeSetting("EmuCore", "EnableWideScreenPatches", "bool", boolToString(settings.widescreen));
            applied = true;
        }
        if (settings.noInterlacing != null) {
            setNativeSetting("EmuCore", "EnableNoInterlacingPatches", "bool", boolToString(settings.noInterlacing));
            applied = true;
        }
        if (settings.loadTextures != null) {
            setNativeSetting("EmuCore/GS", "LoadTextureReplacements", "bool", boolToString(settings.loadTextures));
            applied = true;
        }
        if (settings.asyncTextures != null) {
            setNativeSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", boolToString(settings.asyncTextures));
            applied = true;
        }
        if (settings.precacheTextures != null) {
            setNativeSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", boolToString(settings.precacheTextures));
            applied = true;
        }
        if (settings.showFps != null) {
            setNativeSetting("EmuCore/GS", "OsdShowFPS", "bool", boolToString(settings.showFps));
            applied = true;
        }
        if (settings.renderer != null) {
            setNativeSetting("EmuCore/GS", "Renderer", "int", Integer.toString(settings.renderer));
            applied = true;
        }
        if (!TextUtils.isEmpty(settings.aspectRatio)) {
            setNativeSetting("EmuCore/GS", "AspectRatio", "string", settings.aspectRatio);
            applied = true;
        }

        if (applied) {
            perGameOverridesActive = true;
            lastOverrideSnapshot = snapshot;
            lastOverrideKey = gameKey;
        }
    }

    void restoreOverrides() {
        if (!perGameOverridesActive) {
            lastOverrideSnapshot = null;
            lastOverrideKey = null;
            return;
        }
        PerGameOverrideSnapshot snapshot = lastOverrideSnapshot;
        perGameOverridesActive = false;
        lastOverrideSnapshot = null;
        lastOverrideKey = null;
        if (snapshot == null) {
            return;
        }

        setNativeSetting("EmuCore", "EnableCheats", "bool", snapshot.enableCheats);
        setNativeSetting("EmuCore", "EnableWideScreenPatches", "bool", snapshot.widescreen);
        setNativeSetting("EmuCore", "EnableNoInterlacingPatches", "bool", snapshot.noInterlacing);
        setNativeSetting("EmuCore/GS", "LoadTextureReplacements", "bool", snapshot.loadTextures);
        setNativeSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", snapshot.asyncTextures);
        setNativeSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", snapshot.precacheTextures);
        setNativeSetting("EmuCore/GS", "OsdShowFPS", "bool", snapshot.showFps);
        setNativeSetting("EmuCore/GS", "Renderer", "int", snapshot.renderer);
        setNativeSetting("EmuCore/GS", "AspectRatio", "string", snapshot.aspectRatio);
    }

    private PerGameOverrideSnapshot captureSnapshot() {
        String cheats = safeGetSetting("EmuCore", "EnableCheats", "bool");
        if (cheats == null) cheats = boolToString(host.readBoolSetting("EmuCore", "EnableCheats", false));

        String widescreen = safeGetSetting("EmuCore", "EnableWideScreenPatches", "bool");
        if (widescreen == null) widescreen = boolToString(host.readBoolSetting("EmuCore", "EnableWideScreenPatches", false));

        String noInterlacing = safeGetSetting("EmuCore", "EnableNoInterlacingPatches", "bool");
        if (noInterlacing == null) noInterlacing = boolToString(host.readBoolSetting("EmuCore", "EnableNoInterlacingPatches", false));

        String loadTextures = safeGetSetting("EmuCore/GS", "LoadTextureReplacements", "bool");
        if (loadTextures == null) loadTextures = boolToString(host.readBoolSetting("EmuCore/GS", "LoadTextureReplacements", false));

        String asyncTextures = safeGetSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool");
        if (asyncTextures == null) asyncTextures = boolToString(host.readBoolSetting("EmuCore/GS", "LoadTextureReplacementsAsync", false));

        String precache = safeGetSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool");
        if (precache == null) precache = boolToString(host.readBoolSetting("EmuCore/GS", "PrecacheTextureReplacements", false));

        String showFps = safeGetSetting("EmuCore/GS", "OsdShowFPS", "bool");
        if (showFps == null) showFps = boolToString(host.readBoolSetting("EmuCore/GS", "OsdShowFPS", false));

        String renderer = safeGetSetting("EmuCore/GS", "Renderer", "int");
        if (renderer == null) renderer = Integer.toString(getCurrentRendererValue());

        String aspect = safeGetSetting("EmuCore/GS", "AspectRatio", "string");
        if (aspect == null) aspect = getCurrentAspectRatioValue();

        return new PerGameOverrideSnapshot(cheats, widescreen, noInterlacing, loadTextures, asyncTextures, precache, showFps, renderer, aspect);
    }

    private static String safeGetSetting(String section, String key, String type) {
        try {
            return NativeApp.getSetting(section, key, type);
        } catch (Exception ignored) {
            return null;
        }
    }

    private static void setNativeSetting(String section, String key, String type, @Nullable String value) {
        if (value == null) return;
        try {
            NativeApp.setSetting(section, key, type, value);
        } catch (Exception ignored) {}
    }

    static String boolToString(boolean value) {
        return value ? "true" : "false";
    }

    static final class PerGameOverrideSnapshot {
        @Nullable final String enableCheats;
        @Nullable final String widescreen;
        @Nullable final String noInterlacing;
        @Nullable final String loadTextures;
        @Nullable final String asyncTextures;
        @Nullable final String precacheTextures;
        @Nullable final String showFps;
        @Nullable final String renderer;
        @Nullable final String aspectRatio;

        PerGameOverrideSnapshot(@Nullable String enableCheats,
                                @Nullable String widescreen,
                                @Nullable String noInterlacing,
                                @Nullable String loadTextures,
                                @Nullable String asyncTextures,
                                @Nullable String precacheTextures,
                                @Nullable String showFps,
                                @Nullable String renderer,
                                @Nullable String aspectRatio) {
            this.enableCheats = enableCheats;
            this.widescreen = widescreen;
            this.noInterlacing = noInterlacing;
            this.loadTextures = loadTextures;
            this.asyncTextures = asyncTextures;
            this.precacheTextures = precacheTextures;
            this.showFps = showFps;
            this.renderer = renderer;
            this.aspectRatio = aspectRatio;
        }
    }
}
