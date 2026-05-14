package kr.co.iefriends.pcsx2.utils;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.PopupWindow;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.StringRes;

import kr.co.iefriends.pcsx2.R;

/**
 * One-shot "what's new" tooltip that highlights a newly added UI element.
 *
 * <p>Each tooltip is keyed by a stable feature ID. Once the user dismisses or
 * the tooltip auto-times-out, the key is recorded in SharedPreferences and the
 * tooltip never shows again for that user.
 *
 * <p>Usage from any Activity/Fragment after the view is laid out:
 * <pre>
 *     FeatureTooltipHelper.showOnce(
 *         anchorView,
 *         "my_feature_v1",
 *         R.string.tooltip_my_feature);
 * </pre>
 *
 * <p>Safe to call before the view is attached — the tooltip will defer itself
 * until the next layout pass. Never throws; logs and exits silently on any
 * unexpected state so a misuse cannot break existing UI.
 */
public final class FeatureTooltipHelper {

    private static final String PREFS_NAME = "feature_tooltips";
    private static final String KEY_PREFIX = "seen_";
    private static final long AUTO_DISMISS_MS = 10_000L;

    private FeatureTooltipHelper() {}

    public static void showOnce(@NonNull View anchor,
                                @NonNull String featureKey,
                                @StringRes int messageRes) {
        final Context ctx = anchor.getContext();
        if (ctx == null) return;
        if (hasBeenSeen(ctx, featureKey)) return;
        showInternal(anchor, featureKey, ctx.getString(messageRes));
    }

    public static void showOnce(@NonNull View anchor,
                                @NonNull String featureKey,
                                @NonNull CharSequence message) {
        final Context ctx = anchor.getContext();
        if (ctx == null) return;
        if (hasBeenSeen(ctx, featureKey)) return;
        showInternal(anchor, featureKey, message);
    }

    public static boolean hasBeenSeen(@NonNull Context ctx, @NonNull String featureKey) {
        return prefs(ctx).getBoolean(KEY_PREFIX + featureKey, false);
    }

    public static void markSeen(@NonNull Context ctx, @NonNull String featureKey) {
        prefs(ctx).edit().putBoolean(KEY_PREFIX + featureKey, true).apply();
    }

    public static void resetSeen(@NonNull Context ctx, @NonNull String featureKey) {
        prefs(ctx).edit().remove(KEY_PREFIX + featureKey).apply();
    }

    private static SharedPreferences prefs(@NonNull Context ctx) {
        return ctx.getApplicationContext()
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    private static void showInternal(@NonNull View anchor,
                                     @NonNull String featureKey,
                                     @NonNull CharSequence message) {
        if (!anchor.isAttachedToWindow()) {
            anchor.post(() -> showInternal(anchor, featureKey, message));
            return;
        }

        final Context ctx = anchor.getContext();
        final LayoutInflater inflater = LayoutInflater.from(ctx);
        final View content = inflater.inflate(R.layout.view_feature_tooltip, null, false);

        final TextView tv = content.findViewById(R.id.tv_feature_tooltip_message);
        if (tv != null) tv.setText(message);

        final PopupWindow popup = new PopupWindow(
                content,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                true);
        popup.setOutsideTouchable(true);
        popup.setFocusable(false);

        final Runnable dismiss = () -> {
            markSeen(ctx, featureKey);
            try { popup.dismiss(); } catch (Exception ignored) {}
        };

        content.setOnClickListener(v -> dismiss.run());
        final View closeBtn = content.findViewById(R.id.iv_feature_tooltip_dismiss);
        if (closeBtn != null) closeBtn.setOnClickListener(v -> dismiss.run());
        popup.setOnDismissListener(() -> markSeen(ctx, featureKey));

        content.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);

        final int[] anchorLoc = new int[2];
        anchor.getLocationOnScreen(anchorLoc);
        final Rect visibleFrame = new Rect();
        anchor.getWindowVisibleDisplayFrame(visibleFrame);

        final int tipH = content.getMeasuredHeight();
        final int spaceAbove = anchorLoc[1] - visibleFrame.top;
        final boolean showAbove = spaceAbove >= tipH + 16;
        final int yOff = showAbove ? -(anchor.getHeight() + tipH + 8) : 8;

        try {
            popup.showAsDropDown(anchor, 0, yOff);
        } catch (Exception e) {
            DebugLog.e("FeatureTooltip", "failed to show tooltip for " + featureKey, e);
            return;
        }

        new Handler(Looper.getMainLooper()).postDelayed(dismiss, AUTO_DISMISS_MS);
    }
}
