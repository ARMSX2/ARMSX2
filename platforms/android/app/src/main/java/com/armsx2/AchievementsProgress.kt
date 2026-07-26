package com.armsx2

import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject

/**
 * Records the running game's achievement progress into [PlayTime] so the library can show it for
 * games that are not loaded.
 *
 * This exists because the achievements panel was the only thing that ever recorded progress: open
 * it and the numbers were stored, never open it and the library showed nothing at all — which is
 * every game for most people. The core can only be asked about the game it currently has loaded
 * (`getAchievementsJSON` is VM-scoped), so the numbers have to be captured while the game runs.
 */
object AchievementsProgress {
    /** rc_client's RC_CLIENT_ACHIEVEMENT_UNLOCKED_* bits, as emitted in "unlockedMask". */
    private const val MASK_SOFTCORE = 1
    private const val MASK_HARDCORE = 2

    /**
     * Read the live set and store softcore/hardcore counts for [serial]. Safe to call when no game
     * is loaded or the user is logged out — an absent or empty set records nothing rather than
     * blanking a real figure. Blocking (builds and parses the set JSON), so keep it off hot paths;
     * it is cheap enough once per pause or on a slow poll.
     */
    /** [snapshot] for whatever is loaded now. Called from the RA sound hook, which fires on every
     *  unlock, so the library figure moves as achievements are earned instead of lagging the poll. */
    @JvmStatic
    fun snapshotCurrentGame() {
        val serial = runCatching {
            com.armsx2.runtime.MainActivityRuntime.currentGame.value?.serial
                ?: NativeApp.getGameSerial()
        }.getOrNull()
        snapshot(serial)
    }

    fun snapshot(serial: String?) {
        val s = serial?.takeIf { it.isNotEmpty() } ?: return
        val json = runCatching { NativeApp.getAchievementsJSON() }.getOrNull().orEmpty()
        if (json.isEmpty()) return
        runCatching {
            val items = JSONObject(json).optJSONArray("items") ?: return
            val total = items.length()
            if (total == 0) return
            var softcore = 0
            var hardcore = 0
            for (i in 0 until total) {
                val item = items.optJSONObject(i) ?: continue
                // Prefer the mask: "unlocked" alone cannot tell the two modes apart, and rc_client
                // reports a hardcore unlock as softcore too. Fall back to the boolean for any build
                // that predates the mask, counting it as softcore — the conservative reading, since
                // claiming an unearned hardcore unlock is the worse error.
                val mask = item.optInt("unlockedMask", -1)
                if (mask >= 0) {
                    if ((mask and MASK_SOFTCORE) != 0 || (mask and MASK_HARDCORE) != 0) softcore++
                    if ((mask and MASK_HARDCORE) != 0) hardcore++
                } else if (item.optBoolean("unlocked")) {
                    softcore++
                }
            }
            PlayTime.recordAchievements(s, softcore, hardcore, total)
        }
    }
}
