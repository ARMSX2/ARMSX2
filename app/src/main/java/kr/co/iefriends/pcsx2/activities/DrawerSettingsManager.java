package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.CompoundButton;
import android.widget.Spinner;

import androidx.annotation.IdRes;

import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.materialswitch.MaterialSwitch;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.DebugLog;
import kr.co.iefriends.pcsx2.input.view.DPadView;
import kr.co.iefriends.pcsx2.input.view.JoystickView;

class DrawerSettingsManager {
    private final MainActivity host;

    // Widescreen switch
    private MaterialSwitch drawerWidescreenSwitch;
    private final CompoundButton.OnCheckedChangeListener drawerWidescreenListener =
            (buttonView, isChecked) ->
                    NativeApp.setSetting("EmuCore", "EnableWideScreenPatches", "bool", isChecked ? "true" : "false");

    DrawerSettingsManager(MainActivity host) {
        this.host = host;
    }

    void setupRendererToggleGroup() {
        MaterialButtonToggleGroup rendererGroup = host.findViewById(R.id.drawer_tg_renderer);
        if (rendererGroup == null) {
            return;
        }

        int initialValue = -1;
        try {
            String renderer = NativeApp.getSetting("EmuCore/GS", "Renderer", "int");
            if (renderer != null && !renderer.isEmpty()) {
                initialValue = Integer.parseInt(renderer);
            }
        } catch (Exception ignored) {}

        int initialButton = rendererButtonForValue(initialValue);
        rendererGroup.check(initialButton);
        rendererGroup.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (!isChecked) {
                return;
            }
            int value = rendererValueForButton(checkedId);
            NativeApp.renderGpu(value);
        });
    }

    void setupDrawerSpinners() {
        Spinner aspectSpinner = host.findViewById(R.id.drawer_sp_aspect_ratio);
        if (aspectSpinner != null) {
            ArrayAdapter<CharSequence> aspectAdapter = ArrayAdapter.createFromResource(host, R.array.aspect_ratios, android.R.layout.simple_spinner_item);
            aspectAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            aspectSpinner.setAdapter(aspectAdapter);
            final String[] aspectChoices = host.getResources().getStringArray(R.array.aspect_ratios);
            int current = 0;
            try {
                String aspect = NativeApp.getSetting("EmuCore/GS", "AspectRatio", "string");
                if (aspect != null && !aspect.isEmpty()) {
                    for (int i = 0; i < aspectChoices.length; i++) {
                        if (aspect.equalsIgnoreCase(aspectChoices[i])) {
                            current = i;
                            break;
                        }
                    }
                }
            } catch (Exception ignored) {}
            if (current < 0 || current >= aspectAdapter.getCount()) current = 0;
            aspectSpinner.setSelection(current, false);
            aspectSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    if (position < 0 || position >= aspectChoices.length) return;
                    String value = aspectChoices[position];
                    NativeApp.setSetting("EmuCore/GS", "AspectRatio", "string", value);
                    NativeApp.setAspectRatio(position);
                }
                @Override public void onNothingSelected(AdapterView<?> parent) {}
            });
        }

        Spinner scaleSpinner = host.findViewById(R.id.drawer_sp_scale);
        if (scaleSpinner != null) {
            ArrayAdapter<CharSequence> scaleAdapter = ArrayAdapter.createFromResource(host, R.array.resolution_scales, android.R.layout.simple_spinner_item);
            scaleAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            scaleSpinner.setAdapter(scaleAdapter);
            int current = 0;
            try {
                String value = NativeApp.getSetting("EmuCore/GS", "upscale_multiplier", "float");
                if (value != null && !value.isEmpty()) {
                    float parsed = Float.parseFloat(value);
                    current = Math.max(1, Math.min(scaleAdapter.getCount(), Math.round(parsed))) - 1;
                }
            } catch (Exception ignored) {}
            if (current < 0 || current >= scaleAdapter.getCount()) current = 0;
            scaleSpinner.setSelection(current, false);
            scaleSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    NativeApp.setSetting("EmuCore/GS", "upscale_multiplier", "float", String.valueOf(position + 1));
                }
                @Override public void onNothingSelected(AdapterView<?> parent) {}
            });
        }

        Spinner blendSpinner = host.findViewById(R.id.drawer_sp_blending_accuracy);
        if (blendSpinner != null) {
            ArrayAdapter<CharSequence> blendAdapter = ArrayAdapter.createFromResource(host, R.array.acc_blending, android.R.layout.simple_spinner_item);
            blendAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            blendSpinner.setAdapter(blendAdapter);
            int current = 0;
            try {
                String value = NativeApp.getSetting("EmuCore/GS", "accurate_blending_unit", "int");
                if (value != null && !value.isEmpty()) {
                    current = Integer.parseInt(value);
                }
            } catch (Exception ignored) {}
            if (current < 0 || current >= blendAdapter.getCount()) current = 0;
            blendSpinner.setSelection(current, false);
            blendSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    NativeApp.setSetting("EmuCore/GS", "accurate_blending_unit", "int", Integer.toString(position));
                }
                @Override public void onNothingSelected(AdapterView<?> parent) {}
            });
        }
    }

    void setupControllerModeSpinner() {
        Spinner modeSpinner = host.findViewById(R.id.drawer_sp_controller_mode);
        if (modeSpinner != null) {
            String[] modes = new String[]{"2 Sticks", "1 Stick + Face Buttons", "D-Pad Only"};
            ArrayAdapter<String> adapter = new ArrayAdapter<>(host, android.R.layout.simple_spinner_item, modes);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            modeSpinner.setAdapter(adapter);

            int savedMode = host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).getInt("controller_mode", 0);
            modeSpinner.setSelection(savedMode, false);

            modeSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE).edit().putInt("controller_mode", position).apply();
                    applyControllerMode(position);
                }
                @Override public void onNothingSelected(AdapterView<?> parent) {}
            });

            applyControllerMode(savedMode);
        }
    }

    void applyControllerMode(int mode) {
        host.currentControllerMode = mode;

        JoystickView joystickLeft = host.findViewById(R.id.joystick_left);
        JoystickView joystickRight = host.findViewById(R.id.joystick_right);
        DPadView dpadView = host.findViewById(R.id.dpad_view);
        View llPadRight = host.findViewById(R.id.ll_pad_right);

        if (joystickLeft == null || joystickRight == null || dpadView == null || llPadRight == null) {
            return;
        }

        switch (mode) {
            case 0: // 2 Sticks
                joystickLeft.setVisibility(View.VISIBLE);
                joystickRight.setVisibility(View.VISIBLE);
                dpadView.setVisibility(View.VISIBLE);
                llPadRight.setVisibility(View.VISIBLE);

                ViewGroup.LayoutParams leftParams = joystickLeft.getLayoutParams();
                leftParams.width = host.dpToPx(140);
                leftParams.height = host.dpToPx(140);
                joystickLeft.setLayoutParams(leftParams);

                ViewGroup.LayoutParams rightParams = joystickRight.getLayoutParams();
                rightParams.width = host.dpToPx(140);
                rightParams.height = host.dpToPx(140);
                joystickRight.setLayoutParams(rightParams);

                ViewGroup.LayoutParams dpadParams = dpadView.getLayoutParams();
                dpadParams.width = host.dpToPx(105);
                dpadParams.height = host.dpToPx(105);
                dpadView.setLayoutParams(dpadParams);

                llPadRight.setScaleX(1.0f);
                llPadRight.setScaleY(1.0f);
                if (llPadRight.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
                    ViewGroup.MarginLayoutParams faceParams = (ViewGroup.MarginLayoutParams) llPadRight.getLayoutParams();
                    faceParams.bottomMargin = host.dpToPx(1);
                    llPadRight.setLayoutParams(faceParams);
                }
                host.faceButtonsBaseScale = 1.0f;
                break;

            case 1: // 1 Stick + Face Buttons
                joystickLeft.setVisibility(View.VISIBLE);
                joystickRight.setVisibility(View.GONE);
                dpadView.setVisibility(View.GONE);
                llPadRight.setVisibility(View.VISIBLE);

                ViewGroup.LayoutParams leftParams1 = joystickLeft.getLayoutParams();
                leftParams1.width = host.dpToPx(140);
                leftParams1.height = host.dpToPx(140);
                joystickLeft.setLayoutParams(leftParams1);

                ViewGroup.LayoutParams dpadParams1 = dpadView.getLayoutParams();
                dpadParams1.width = host.dpToPx(105);
                dpadParams1.height = host.dpToPx(105);
                dpadView.setLayoutParams(dpadParams1);

                llPadRight.setScaleX(1.4f);
                llPadRight.setScaleY(1.4f);

                if (llPadRight.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
                    ViewGroup.MarginLayoutParams faceParams1 = (ViewGroup.MarginLayoutParams) llPadRight.getLayoutParams();
                    faceParams1.bottomMargin = host.dpToPx(6) + host.dpToPx(11);
                    llPadRight.setLayoutParams(faceParams1);
                }
                host.faceButtonsBaseScale = 1.4f;
                break;

            case 2: // D-Pad Only
                joystickLeft.setVisibility(View.GONE);
                joystickRight.setVisibility(View.GONE);
                dpadView.setVisibility(View.VISIBLE);
                llPadRight.setVisibility(View.VISIBLE);

                ViewGroup.LayoutParams dpadParams2 = dpadView.getLayoutParams();
                dpadParams2.width = host.dpToPx(140);
                dpadParams2.height = host.dpToPx(140);
                dpadView.setLayoutParams(dpadParams2);

                ViewGroup.LayoutParams rightParams2 = joystickRight.getLayoutParams();
                rightParams2.width = host.dpToPx(140);
                rightParams2.height = host.dpToPx(140);
                joystickRight.setLayoutParams(rightParams2);

                llPadRight.setScaleX(1.4f);
                llPadRight.setScaleY(1.4f);

                if (llPadRight.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
                    ViewGroup.MarginLayoutParams faceParams2 = (ViewGroup.MarginLayoutParams) llPadRight.getLayoutParams();
                    faceParams2.bottomMargin = host.dpToPx(6) + host.dpToPx(11);
                    llPadRight.setLayoutParams(faceParams2);
                }
                host.faceButtonsBaseScale = 1.4f;
                break;
        }
        host.applyJoystickStyle(joystickLeft);
        host.applyJoystickStyle(joystickRight);
        host.applyDpadStyle(dpadView);
        host.applyUserUiScale();
    }

    void setupDrawerSwitches() {
        MaterialSwitch swEnableCheats = host.findViewById(R.id.drawer_sw_enable_cheats);
        if (swEnableCheats != null) {
            swEnableCheats.setChecked(host.readBoolSetting("EmuCore", "EnableCheats", false));
            swEnableCheats.setOnCheckedChangeListener((buttonView, isChecked) -> {
                NativeApp.setEnableCheats(isChecked);
                try { DebugLog.d("Cheats", "EnableCheats=" + isChecked); } catch (Throwable ignored) {}
            });
        }

        drawerWidescreenSwitch = host.findViewById(R.id.drawer_sw_widescreen);
        updateWidescreenToggleVisibility();

        MaterialSwitch swNoInterlacing = host.findViewById(R.id.drawer_sw_no_interlacing);
        if (swNoInterlacing != null) {
            swNoInterlacing.setChecked(host.readBoolSetting("EmuCore", "EnableNoInterlacingPatches", false));
            swNoInterlacing.setOnCheckedChangeListener((buttonView, isChecked) ->
                    NativeApp.setSetting("EmuCore", "EnableNoInterlacingPatches", "bool", isChecked ? "true" : "false"));
        }

        MaterialSwitch swLoadTextures = host.findViewById(R.id.drawer_sw_load_textures);
        if (swLoadTextures != null) {
            swLoadTextures.setChecked(host.readBoolSetting("EmuCore/GS", "LoadTextureReplacements", false));
            swLoadTextures.setOnCheckedChangeListener((buttonView, isChecked) -> {
                NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", isChecked ? "true" : "false");
                try { DebugLog.d("Textures", "LoadTextureReplacements=" + isChecked); } catch (Throwable ignored) {}
            });
        }

        MaterialSwitch swAsyncTextures = host.findViewById(R.id.drawer_sw_async_textures);
        if (swAsyncTextures != null) {
            swAsyncTextures.setChecked(host.readBoolSetting("EmuCore/GS", "LoadTextureReplacementsAsync", false));
            swAsyncTextures.setOnCheckedChangeListener((buttonView, isChecked) -> {
                NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", isChecked ? "true" : "false");
                try { DebugLog.d("Textures", "LoadTextureReplacementsAsync=" + isChecked); } catch (Throwable ignored) {}
            });
        }

        MaterialSwitch swPrecacheTextures = host.findViewById(R.id.drawer_sw_precache_textures);
        if (swPrecacheTextures != null) {
            swPrecacheTextures.setChecked(host.readBoolSetting("EmuCore/GS", "PrecacheTextureReplacements", false));
            swPrecacheTextures.setOnCheckedChangeListener((buttonView, isChecked) -> {
                NativeApp.setSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", isChecked ? "true" : "false");
                try { DebugLog.d("Textures", "PrecacheTextureReplacements=" + isChecked); } catch (Throwable ignored) {}
            });
        }

        MaterialSwitch swDevHud = host.findViewById(R.id.drawer_sw_dev_hud);
        if (swDevHud != null) {
            swDevHud.setChecked(host.readBoolSetting("EmuCore/GS", "OsdShowFPS", false));
            swDevHud.setOnCheckedChangeListener((buttonView, isChecked) ->
                    NativeApp.setSetting("EmuCore/GS", "OsdShowFPS", "bool", isChecked ? "true" : "false"));
        }
    }

    void updateWidescreenToggleVisibility() {
        if (drawerWidescreenSwitch == null) {
            return;
        }
        boolean hasPatch = false;
        try {
            hasPatch = NativeApp.hasWidescreenPatch();
        } catch (Throwable ignored) {}
        if (!hasPatch) {
            drawerWidescreenSwitch.setVisibility(View.GONE);
            drawerWidescreenSwitch.setOnCheckedChangeListener(null);
            return;
        }
        drawerWidescreenSwitch.setVisibility(View.VISIBLE);
        drawerWidescreenSwitch.setText(R.string.drawer_apply_widescreen_patch);
        drawerWidescreenSwitch.setOnCheckedChangeListener(null);
        drawerWidescreenSwitch.setChecked(host.readBoolSetting("EmuCore", "EnableWideScreenPatches", false));
        drawerWidescreenSwitch.setOnCheckedChangeListener(drawerWidescreenListener);
    }

    @IdRes int rendererButtonForValue(int value) {
        switch (value) {
            case 12: return R.id.drawer_tb_gl;
            case 13: return R.id.drawer_tb_sw;
            case 14: return R.id.drawer_tb_vk;
            default: return R.id.drawer_tb_at;
        }
    }

    int rendererValueForButton(@IdRes int buttonId) {
        if (buttonId == R.id.drawer_tb_gl) return 12;
        if (buttonId == R.id.drawer_tb_sw) return 13;
        if (buttonId == R.id.drawer_tb_vk) return 14;
        return -1;
    }

    void applyRendererSelection(int rendererValue) {
        MaterialButtonToggleGroup rendererGroup = host.findViewById(R.id.drawer_tg_renderer);
        if (rendererGroup != null) {
            rendererGroup.check(rendererButtonForValue(rendererValue));
        } else {
            NativeApp.renderGpu(rendererValue);
        }
    }
}
