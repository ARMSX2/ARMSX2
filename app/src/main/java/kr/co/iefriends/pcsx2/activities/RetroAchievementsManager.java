package kr.co.iefriends.pcsx2.activities;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Drawable;
import android.text.TextUtils;
import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.File;

import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.utils.RetroAchievementsBridge;

class RetroAchievementsManager {
    private final MainActivity host;

    // State
    RetroAchievementsBridge.State currentState;
    private boolean lastLoggedIn = false;
    private int lastGameId = -1;
    private String lastIconPath = "";

    // Drawer views (set up in setupDrawerSection)
    private View drawerRaSection;
    private TextView drawerRaTitle;
    private TextView drawerRaSubtitle;
    private ImageView drawerRaIcon;
    private TextView drawerRaLabel;

    RetroAchievementsManager(MainActivity host) {
        this.host = host;
    }

    void setupDrawerSection() {
        drawerRaSection = host.findViewById(R.id.drawer_ra_section);
        drawerRaLabel = host.findViewById(R.id.drawer_ra_label);
        if (drawerRaSection == null) {
            return;
        }

        drawerRaTitle = drawerRaSection.findViewById(R.id.drawer_ra_title);
        drawerRaSubtitle = drawerRaSection.findViewById(R.id.drawer_ra_subtitle);
        drawerRaIcon = drawerRaSection.findViewById(R.id.drawer_ra_icon);

        drawerRaSection.setOnClickListener(v -> showGameDialog());
        updateDrawer(currentState);
    }

    void handleStateChanged(RetroAchievementsBridge.State state) {
        currentState = state;
        updateDrawer(state);

        if (state == null) {
            lastLoggedIn = false;
            lastGameId = -1;
            lastIconPath = "";
            return;
        }

        if (state.loggedIn && !lastLoggedIn) {
            String name = !TextUtils.isEmpty(state.displayName) ? state.displayName : state.username;
            if (!TextUtils.isEmpty(name)) {
                showToast(host.getString(R.string.drawer_ra_toast_connected, name));
            } else {
                showToast(host.getString(R.string.drawer_ra_toast_tracking_generic));
            }
        }

        if (state.hasActiveGame && state.gameId != 0 && state.gameId != lastGameId) {
            if (!TextUtils.isEmpty(state.gameTitle)) {
                showToast(host.getString(R.string.drawer_ra_toast_tracking, state.gameTitle));
            } else {
                showToast(host.getString(R.string.drawer_ra_toast_tracking_generic));
            }
        }

        lastLoggedIn = state.loggedIn;
        lastGameId = state.hasActiveGame ? state.gameId : -1;
    }

    private void updateDrawer(RetroAchievementsBridge.State state) {
        if (drawerRaSection == null) {
            return;
        }

        boolean shouldShow = state != null && state.achievementsEnabled && state.loggedIn && state.hasActiveGame
                && !TextUtils.isEmpty(state.gameTitle);
        int visibility = shouldShow ? View.VISIBLE : View.GONE;
        drawerRaSection.setVisibility(visibility);
        if (drawerRaLabel != null) {
            drawerRaLabel.setVisibility(visibility);
        }

        if (!shouldShow) {
            if (drawerRaIcon != null) {
                drawerRaIcon.setImageDrawable(null);
                drawerRaIcon.setVisibility(View.GONE);
            }
            lastIconPath = "";
            return;
        }

        if (drawerRaTitle != null) {
            drawerRaTitle.setText(state.gameTitle);
        }

        if (drawerRaSubtitle != null) {
            String subtitle = null;
            if (!TextUtils.isEmpty(state.richPresence)) {
                subtitle = state.richPresence;
            } else if (state.totalAchievements > 0) {
                subtitle = host.getString(R.string.drawer_ra_dialog_progress, state.unlockedAchievements, state.totalAchievements);
            }
            if (!TextUtils.isEmpty(subtitle)) {
                drawerRaSubtitle.setText(subtitle);
                drawerRaSubtitle.setVisibility(View.VISIBLE);
            } else {
                drawerRaSubtitle.setText("");
                drawerRaSubtitle.setVisibility(View.GONE);
            }
        }

        if (drawerRaIcon != null) {
            boolean iconVisible = false;
            if (!TextUtils.isEmpty(state.gameIconPath)) {
                File iconFile = new File(state.gameIconPath);
                if (iconFile.exists() && iconFile.isFile()) {
                    if (!state.gameIconPath.equals(lastIconPath)) {
                        Bitmap bitmap = BitmapFactory.decodeFile(iconFile.getAbsolutePath());
                        if (bitmap != null) {
                            drawerRaIcon.setImageBitmap(bitmap);
                            lastIconPath = state.gameIconPath;
                        } else {
                            drawerRaIcon.setImageDrawable(null);
                            lastIconPath = "";
                        }
                    }
                    iconVisible = drawerRaIcon.getDrawable() != null;
                } else {
                    drawerRaIcon.setImageDrawable(null);
                    lastIconPath = "";
                }
            } else {
                drawerRaIcon.setImageDrawable(null);
                lastIconPath = "";
            }
            drawerRaIcon.setVisibility(iconVisible ? View.VISIBLE : View.GONE);
        }
    }

    void showGameDialog() {
        RetroAchievementsBridge.State state = currentState;
        if (state == null || !state.achievementsEnabled || !state.loggedIn || !state.hasActiveGame) {
            return;
        }

        String title = !TextUtils.isEmpty(state.gameTitle) ? state.gameTitle : host.getString(R.string.drawer_ra_dialog_title);
        MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(host)
                .setTitle(title)
                .setPositiveButton(android.R.string.ok, null);

        StringBuilder message = new StringBuilder();
        if (!TextUtils.isEmpty(state.richPresence)) {
            message.append(state.richPresence.trim());
        }
        if (state.totalAchievements > 0) {
            if (message.length() > 0) {
                message.append("\n\n");
            }
            message.append(host.getString(R.string.drawer_ra_dialog_progress, state.unlockedAchievements, state.totalAchievements));
        }
        if (state.totalPoints > 0) {
            if (message.length() > 0) {
                message.append("\n");
            }
            message.append(host.getString(R.string.drawer_ra_dialog_points, state.unlockedPoints, state.totalPoints));
        }
        if (state.hasLeaderboards) {
            if (message.length() > 0) {
                message.append("\n");
            }
            message.append(host.getString(R.string.drawer_ra_dialog_leaderboards));
        }

        if (message.length() > 0) {
            builder.setMessage(message.toString());
        }

        if (drawerRaIcon != null) {
            Drawable drawable = drawerRaIcon.getDrawable();
            if (drawable != null) {
                builder.setIcon(drawable);
            }
        }

        builder.show();
    }

    private void showToast(String message) {
        if (TextUtils.isEmpty(message)) {
            return;
        }
        try {
            Toast.makeText(host, message, Toast.LENGTH_SHORT).show();
        } catch (Throwable ignored) {}
    }
}
