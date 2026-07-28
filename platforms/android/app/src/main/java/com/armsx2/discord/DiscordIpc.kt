package com.armsx2.discord

/**
 * The wire between ARMSX2 and the Discord helper process.
 *
 * Deliberately dumb. Both sides live in the same APK, so it would be easy to pass rich objects
 * around — and that is exactly what must not happen. The GPL/proprietary separation rests on these
 * being two programs exchanging simple messages, not one program split across a boundary, so the
 * protocol carries nothing but strings and ints: a game title, a serial, an image URL, a list of
 * names. No emulator types cross this line in either direction.
 *
 * Everything is a plain Bundle over a [android.os.Messenger]; there is no AIDL interface, because
 * an interface is the sort of thing that tempts you to widen it.
 */
object DiscordIpc {
    // App -> helper.
    const val MSG_START = 1        // DATA_TOKEN: resume with a saved token, or empty to idle
    const val MSG_AUTHORIZE = 2    // begin browser sign-in
    const val MSG_SET_PLAYING = 3  // DATA_SERIAL / DATA_TITLE / DATA_COVER
    const val MSG_QUERY = 4        // request one MSG_STATE; replyTo carries the answer
    const val MSG_STOP = 5         // sign out and tear the client down

    // Helper -> app.
    const val MSG_STATE = 100

    const val DATA_TOKEN = "token"
    const val DATA_SERIAL = "serial"
    const val DATA_TITLE = "title"
    const val DATA_COVER = "cover"

    const val DATA_STATUS = "status"
    const val DATA_FRIENDS = "friends"
    const val DATA_SELF = "self"
    const val DATA_ERROR = "error"
    const val DATA_FRESH_TOKEN = "freshToken"
    const val DATA_AVAILABLE = "available"

    /** Must match kFieldSep / kRecordSep in cpp/discord_bridge.cpp. */
    const val FIELD_SEP = ''
    const val RECORD_SEP = ''
}
