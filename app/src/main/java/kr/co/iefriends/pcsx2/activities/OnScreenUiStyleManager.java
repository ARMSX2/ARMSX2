package kr.co.iefriends.pcsx2.activities;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.widget.TextView;

import androidx.annotation.DrawableRes;

import com.google.android.material.slider.Slider;

import java.io.IOException;
import java.io.InputStream;

import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.input.view.DPadView;
import kr.co.iefriends.pcsx2.input.view.JoystickView;
import kr.co.iefriends.pcsx2.input.view.PSButtonView;
import kr.co.iefriends.pcsx2.input.view.PSShoulderButtonView;
import kr.co.iefriends.pcsx2.utils.DebugLog;

class OnScreenUiStyleManager {

    static final String PREF_ONSCREEN_UI_STYLE = "on_screen_ui_style";
    static final String PREF_UI_SCALE_MULTIPLIER = "onscreen_ui_scale_multiplier";
    static final String STYLE_DEFAULT = "default";
    static final String STYLE_NETHER = "nether";
    static final float ONSCREEN_UI_SCALE_MIN = 0.2f;
    static final float ONSCREEN_UI_SCALE_MAX = 4.0f;

    private final MainActivity host;

    // State
    String currentStyle;
    float scaleMultiplier = 1.0f;

    OnScreenUiStyleManager(MainActivity host) {
        this.host = host;
        currentStyle = resolveStylePreference();
    }

    String resolveStylePreference() {
        String value = host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                .getString(PREF_ONSCREEN_UI_STYLE, STYLE_DEFAULT);
        if (STYLE_NETHER.equalsIgnoreCase(value)) {
            return STYLE_NETHER;
        }
        return STYLE_DEFAULT;
    }

    void refreshStyleIfNeeded() {
        String pref = resolveStylePreference();
        if (!pref.equals(currentStyle)) {
            currentStyle = pref;
            host.makeButtonTouch();
        }
    }

    Drawable loadNetherDrawable(String assetName) {
        try (InputStream is = host.getAssets().open("app_icons/controller_icons_nether/" + assetName)) {
            Drawable drawable = Drawable.createFromStream(is, assetName);
            if (drawable != null) {
                drawable = drawable.mutate();
            }
            return drawable;
        } catch (IOException e) {
            try { DebugLog.e("OnScreenUI", "Failed to load Nether icon " + assetName + ": " + e.getMessage()); } catch (Throwable ignored) {}
            return null;
        }
    }

    void applyButtonIcon(PSButtonView view, @DrawableRes int defaultResId, String netherAssetName) {
        if (view == null) {
            return;
        }
        if (STYLE_NETHER.equals(currentStyle)) {
            Drawable drawable = loadNetherDrawable(netherAssetName);
            if (drawable != null) {
                view.setIconDrawable(drawable);
                return;
            }
        }
        view.setIconResource(defaultResId);
    }

    void applyShoulderIcon(PSShoulderButtonView view, @DrawableRes int defaultResId, String netherAssetName) {
        if (view == null) {
            return;
        }
        if (STYLE_NETHER.equals(currentStyle)) {
            Drawable drawable = loadNetherDrawable(netherAssetName);
            if (drawable != null) {
                view.setIconDrawable(drawable);
                return;
            }
        }
        view.setIconResource(defaultResId);
    }

    void applyJoystickStyle(JoystickView joystick) {
        if (joystick == null) {
            return;
        }
        if (STYLE_NETHER.equals(currentStyle)) {
            Drawable base = loadNetherDrawable("ic_controller_analog_base.png");
            Drawable knob = loadNetherDrawable("ic_controller_analog_stick.png");
            if (base != null && knob != null) {
                joystick.setDrawables(base, knob);
                joystick.setKnobScaleFactor(1.2f);
                return;
            }
        }
        joystick.setDrawables(null, null);
        joystick.setKnobScaleFactor(1.0f);
    }

    void applyDpadStyle(DPadView dpadView) {
        if (dpadView == null) {
            return;
        }
        if (STYLE_NETHER.equals(currentStyle)) {
            Drawable up = loadNetherDrawable("ic_controller_up_button.png");
            Drawable down = loadNetherDrawable("ic_controller_down_button.png");
            Drawable left = loadNetherDrawable("ic_controller_left_button.png");
            Drawable right = loadNetherDrawable("ic_controller_right_button.png");
            dpadView.setDrawables(null, null);
            dpadView.setDirectionalDrawables(up, down, left, right);
        } else {
            dpadView.setDrawables(null, null);
            dpadView.setDirectionalDrawables(null, null, null, null);
        }
    }

    void loadScalePreference() {
        float value = 1.0f;
        try {
            value = host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                    .getFloat(PREF_UI_SCALE_MULTIPLIER, 1.0f);
        } catch (Exception ignored) {}
        if (value < ONSCREEN_UI_SCALE_MIN) value = ONSCREEN_UI_SCALE_MIN;
        if (value > ONSCREEN_UI_SCALE_MAX) value = ONSCREEN_UI_SCALE_MAX;
        scaleMultiplier = value;
    }

    void saveScalePreference(float value) {
        host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                .edit().putFloat(PREF_UI_SCALE_MULTIPLIER, value).apply();
    }

    void applyUserUiScale() {
        float multiplier = Math.max(ONSCREEN_UI_SCALE_MIN, Math.min(ONSCREEN_UI_SCALE_MAX, scaleMultiplier));
        scaleMultiplier = multiplier;
        applyScaleWithPivot(host.llPadSelectStart, multiplier, multiplier, 0.5f, 1f);
        View padRight = host.llPadRight != null ? host.llPadRight : host.findViewById(R.id.ll_pad_right);
        float faceScale = host.faceButtonsBaseScale * multiplier;
        applyScaleWithPivot(padRight, faceScale, faceScale, 1f, 1f);
        View leftShoulders = host.findViewById(R.id.ll_pad_shoulders_left);
        applyScaleWithPivot(leftShoulders, multiplier, multiplier, 0f, 0f);
        View rightShoulders = host.findViewById(R.id.ll_pad_shoulders_right);
        applyScaleWithPivot(rightShoulders, multiplier, multiplier, 1f, 0f);
        JoystickView joystickLeft = host.findViewById(R.id.joystick_left);
        applyScaleWithPivot(joystickLeft, multiplier, multiplier, 0f, 1f);
        JoystickView joystickRight = host.findViewById(R.id.joystick_right);
        applyScaleWithPivot(joystickRight, multiplier, multiplier, 1f, 1f);
        DPadView dpadView = host.findViewById(R.id.dpad_view);
        applyScaleWithPivot(dpadView, multiplier, multiplier, 0f, 1f);
    }

    private void applyScaleWithPivot(View view, float scaleX, float scaleY, float pivotXF, float pivotYF) {
        if (view == null) {
            return;
        }
        Runnable apply = () -> {
            float pivotX = view.getWidth() * pivotXF;
            float pivotY = view.getHeight() * pivotYF;
            view.setPivotX(pivotX);
            view.setPivotY(pivotY);
            view.setScaleX(scaleX);
            view.setScaleY(scaleY);
        };
        if (view.getWidth() == 0 || view.getHeight() == 0) {
            view.post(apply);
        } else {
            apply.run();
        }
    }

    void updateScaleLabel(TextView label) {
        if (label != null) {
            label.setText(host.getString(R.string.drawer_ui_scale_value, scaleMultiplier));
        }
    }

    void refreshScaleIfNeeded() {
        float stored = 1.0f;
        try {
            stored = host.getSharedPreferences(MainActivity.PREFS, Context.MODE_PRIVATE)
                    .getFloat(PREF_UI_SCALE_MULTIPLIER, 1.0f);
        } catch (Exception ignored) {}
        if (stored < ONSCREEN_UI_SCALE_MIN) stored = ONSCREEN_UI_SCALE_MIN;
        if (stored > ONSCREEN_UI_SCALE_MAX) stored = ONSCREEN_UI_SCALE_MAX;
        if (Math.abs(stored - scaleMultiplier) > 0.001f) {
            scaleMultiplier = stored;
            applyUserUiScale();
            Slider slider = host.findViewById(R.id.drawer_slider_ui_scale);
            if (slider != null && Math.abs(slider.getValue() - stored) > 0.001f) {
                slider.setValue(stored);
            }
            TextView label = host.findViewById(R.id.drawer_ui_scale_value);
            updateScaleLabel(label);
        }
    }
}
