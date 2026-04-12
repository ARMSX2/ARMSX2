package kr.co.iefriends.pcsx2.activities;

import android.net.Uri;

class GameEntry {
    final String title;
    final Uri uri;
    String serial;
    String gameTitle;

    GameEntry(String t, Uri u) { title = t; uri = u; }

    String fileTitleNoExt() {
        int i = title.lastIndexOf('.');
        return (i > 0) ? title.substring(0, i) : title;
    }
}
