package kr.co.iefriends.pcsx2.activities;

import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.lang.ref.WeakReference;

import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.input.ControllerMappingManager;
import kr.co.iefriends.pcsx2.utils.SDLControllerManager;

class ControllerManager {

    // Static state shared across all instances / JNI callbacks
    static final int RUMBLE_DURATION_MS = 160;
    static volatile int sLastControllerDeviceId = -1;
    static volatile boolean sVibrationEnabled = true;
    private static WeakReference<MainActivity> sInstanceRef = new WeakReference<>(null);

    // Deadzone constants
    private static final float ANALOG_DEADZONE = 0.08f;
    private static final float TRIGGER_DEADZONE = 0.04f;

    // Per-instance input state
    private final SparseIntArray analogStates = new SparseIntArray();
    private boolean hatUp, hatDown, hatLeft, hatRight;

    private final MainActivity host;

    ControllerManager(MainActivity host) {
        this.host = host;
    }

    static void setInstance(MainActivity host) {
        sInstanceRef = new WeakReference<>(host);
    }

    static void clearInstance() {
        sInstanceRef = new WeakReference<>(null);
    }

    void forwardKeyToPad(boolean down, int keycode) {
        int mapped = ControllerMappingManager.getPadCodeForKey(keycode);
        if (mapped == ControllerMappingManager.NO_MAPPING) {
            mapped = keycode;
        }
        NativeApp.setPadButton(mapped, 0, down);
    }

    void handleGamepadMotion(MotionEvent e) {
        updateLastControllerDeviceId(e.getDeviceId());
        float lx = getCenteredAxis(e, MotionEvent.AXIS_X);
        float ly = getCenteredAxis(e, MotionEvent.AXIS_Y);
        sendAnalog(111, Math.max(0f, lx));
        sendAnalog(113, Math.max(0f, -lx));
        sendAnalog(112, Math.max(0f, ly));
        sendAnalog(110, Math.max(0f, -ly));

        float rx = getCenteredAxis(e, MotionEvent.AXIS_RX);
        float ry = getCenteredAxis(e, MotionEvent.AXIS_RY);
        if (rx == 0f && ry == 0f) {
            rx = getCenteredAxis(e, MotionEvent.AXIS_Z);
            ry = getCenteredAxis(e, MotionEvent.AXIS_RZ);
        }
        sendAnalog(121, Math.max(0f, rx));
        sendAnalog(123, Math.max(0f, -rx));
        sendAnalog(122, Math.max(0f, ry));
        sendAnalog(120, Math.max(0f, -ry));

        float ltrig = e.getAxisValue(MotionEvent.AXIS_LTRIGGER);
        float rtrig = e.getAxisValue(MotionEvent.AXIS_RTRIGGER);
        if (ltrig == 0f) ltrig = e.getAxisValue(MotionEvent.AXIS_BRAKE);
        if (rtrig == 0f) rtrig = e.getAxisValue(MotionEvent.AXIS_GAS);
        sendAnalog(KeyEvent.KEYCODE_BUTTON_L2, normalizeTrigger(ltrig), TRIGGER_DEADZONE);
        sendAnalog(KeyEvent.KEYCODE_BUTTON_R2, normalizeTrigger(rtrig), TRIGGER_DEADZONE);

        float hatX = e.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        final float hatThreshold = 0.4f;
        boolean nowLeft = hatX < -hatThreshold;
        boolean nowRight = hatX > hatThreshold;
        boolean nowUp = hatY < -hatThreshold;
        boolean nowDown = hatY > hatThreshold;
        setAxisState(hatLeft, nowLeft, KeyEvent.KEYCODE_DPAD_LEFT);  hatLeft = nowLeft;
        setAxisState(hatRight, nowRight, KeyEvent.KEYCODE_DPAD_RIGHT); hatRight = nowRight;
        setAxisState(hatUp, nowUp, KeyEvent.KEYCODE_DPAD_UP); hatUp = nowUp;
        setAxisState(hatDown, nowDown, KeyEvent.KEYCODE_DPAD_DOWN); hatDown = nowDown;
    }

    private void setAxisState(boolean prev, boolean now, int code) {
        if (prev == now) return;
        if (!ControllerMappingManager.isPadCodeBound(code)) {
            return;
        }
        NativeApp.setPadButton(code, 0, now);
    }

    private float getCenteredAxis(MotionEvent e, int axis) {
        final InputDevice device = e.getDevice();
        if (device != null) {
            final InputDevice.MotionRange range = device.getMotionRange(axis, e.getSource());
            if (range != null) {
                float value = e.getAxisValue(axis);
                float flat = range.getFlat();
                if (Math.abs(value) > flat) return value;
            }
        }
        return 0f;
    }

    void sendAnalog(int keyCode, float normalized) {
        sendAnalog(keyCode, normalized, ANALOG_DEADZONE);
    }

    private void sendAnalog(int keyCode, float normalized, float deadzone) {
        if (Float.isNaN(normalized)) normalized = 0f;
        int padCode = ControllerMappingManager.getPadCodeForKey(keyCode);
        if (padCode == ControllerMappingManager.NO_MAPPING) {
            padCode = keyCode;
        }
        if (!ControllerMappingManager.isPadCodeBound(padCode)) {
            analogStates.put(padCode, 0);
            NativeApp.setPadButton(padCode, 0, false);
            return;
        }
        float value = Math.min(1f, Math.max(0f, normalized));
        if (value < deadzone) value = 0f;
        int scaled = Math.round(value * 255f);
        int prev = analogStates.get(padCode, -1);
        if (prev == scaled) return;
        analogStates.put(padCode, scaled);
        NativeApp.setPadButton(padCode, scaled, scaled > 0);
    }

    private static float normalizeTrigger(float raw) {
        if (Float.isNaN(raw)) return 0f;
        if (raw < 0f) {
            return Math.min(1f, Math.max(0f, (raw + 1f) * 0.5f));
        }
        return Math.min(1f, raw);
    }

    static void updateLastControllerDeviceId(int deviceId) {
        if (deviceId >= 0) {
            sLastControllerDeviceId = deviceId;
        }
    }

    public static void requestControllerRumble(float large, float small) {
        MainActivity activity = sInstanceRef != null ? sInstanceRef.get() : null;
        if (activity == null) {
            if (!sVibrationEnabled) stopControllerRumbleStatic();
            return;
        }
        activity.runOnUiThread(() -> {
            if (activity.mControllerManager != null) {
                activity.mControllerManager.dispatchControllerRumble(large, small);
            }
        });
    }

    void dispatchControllerRumble(float large, float small) {
        if (!sVibrationEnabled) {
            stopControllerRumble();
            return;
        }
        final float clampedLarge = clamp01(large);
        final float clampedSmall = clamp01(small);
        final float combined = Math.max(clampedLarge, clampedSmall);
        final int deviceId = sLastControllerDeviceId;

        if (combined <= 0f) {
            stopControllerRumble();
            return;
        }

        boolean usedController = false;
        if (deviceId >= 0 && SDLControllerManager.isDeviceSDLJoystick(deviceId)) {
            usedController = true;
            try {
                SDLControllerManager.hapticRumble(deviceId, clampedLarge, clampedSmall, RUMBLE_DURATION_MS);
            } catch (Throwable ignored) {}
        }

        final int vibratorServiceId = 999999;
        if (!usedController) {
            try {
                SDLControllerManager.hapticRun(vibratorServiceId, combined, RUMBLE_DURATION_MS);
            } catch (Throwable ignored) {}
        }
    }

    private void stopControllerRumble() {
        stopControllerRumbleStatic();
    }

    private static void stopControllerRumbleStatic() {
        final int deviceId = sLastControllerDeviceId;
        try {
            if (deviceId >= 0) SDLControllerManager.hapticStop(deviceId);
        } catch (Throwable ignored) {}
        try {
            SDLControllerManager.hapticStop(999999);
        } catch (Throwable ignored) {}
    }

    private static float clamp01(float value) {
        if (Float.isNaN(value)) return 0f;
        if (value <= 0f) return 0f;
        return Math.min(1f, value);
    }

    void refreshVibrationPreference() {
        boolean enabled = true;
        try {
            String vibration = NativeApp.getSetting("Pad1", "Vibration", "bool");
            if (vibration != null && !vibration.isEmpty()) {
                enabled = !"false".equalsIgnoreCase(vibration);
            } else {
                NativeApp.setSetting("Pad1", "Vibration", "bool", "true");
                enabled = true;
            }
        } catch (Exception ignored) {}
        setVibrationPreference(enabled);
    }

    public static void setVibrationPreference(boolean enabled) {
        sVibrationEnabled = enabled;
        MainActivity activity = sInstanceRef != null ? sInstanceRef.get() : null;
        if (!enabled) {
            if (activity != null) {
                activity.runOnUiThread(() -> {
                    if (activity.mControllerManager != null) {
                        activity.mControllerManager.stopControllerRumble();
                    }
                });
            } else {
                stopControllerRumbleStatic();
            }
        }
    }
}
