package com.armsx2.data.library

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import com.armsx2.i18n.I18n

/**
 * Asks the user, once, whether one named app may read the recently-played list.
 *
 * Why this exists rather than only the Settings toggle: a companion app cannot work until sharing
 * is on, but nothing tells the user that, and a switch in App settings is not somewhere anyone
 * looks unprompted. The result is a feature that appears broken. Here the app that wants the data
 * launches this, the user is asked in context, and one tap is the whole flow.
 *
 * It is also the narrower grant. The Settings switch opens the library to every app on the device;
 * this admits exactly the caller named in the prompt, and it can be taken back per app.
 *
 * Launch it with startActivityForResult and WITHOUT FLAG_ACTIVITY_NEW_TASK:
 *
 *     val intent = Intent("com.armsx2.action.REQUEST_RECENT_GAMES_ACCESS")
 *         .setPackage("com.armsx2")
 *     startActivityForResult(intent, REQUEST_CODE)
 *
 * RESULT_OK means granted. Anything else means it was not, including the user backing out.
 *
 * The caller is identified with [getCallingPackage], which Binder fills in and the caller cannot
 * set. That is the whole security of this screen, and it is also why the launch has to be
 * startActivityForResult: a plain startActivity, or NEW_TASK, leaves it null, and a request that
 * cannot name its asker is refused rather than attributed to a guess.
 *
 * Feature contributed by misantronic (PR #616), following the provider itself (PR #566).
 */
class RecentGamesAccessActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val caller = callingPackage
        if (caller == null) {
            // Nothing trustworthy to attribute a grant to. Do not fall back to the referrer or an
            // intent extra: both are caller-supplied, and a grant is exactly the thing an app
            // would want to mis-attribute.
            setResult(RESULT_CANCELED)
            finish()
            return
        }

        val prefs = RecentGamesAccess.prefs(this)
        if (RecentGamesAccess.isGranted(prefs, caller)) {
            setResult(RESULT_OK)
            finish()
            return
        }
        
        val ownLabel = appLabelFor(packageName)
        AlertDialog.Builder(this)
            .setTitle(I18n.get("library.shareRequest.title").format(ownLabel))
            .setMessage(
                I18n.get("library.shareRequest.message")
                    .format(appLabelFor(caller), caller, ownLabel)
            )
            .setPositiveButton(I18n.get("library.shareRequest.allow")) { _, _ ->
                RecentGamesAccess.grant(prefs, caller)
                setResult(RESULT_OK)
                finish()
            }
            .setNegativeButton(I18n.get("library.shareRequest.deny")) { _, _ ->
                RecentGamesAccess.decline(prefs, caller)
                setResult(RESULT_CANCELED)
                finish()
            }
            .setOnCancelListener {
                // Backing out is an answer too, otherwise the prompt returns on the next query.
                RecentGamesAccess.decline(prefs, caller)
                setResult(RESULT_CANCELED)
                finish()
            }
            .show()
    }

    /** The caller's own display name, so the prompt names the app the user recognises rather than
     *  a package id. Falls back to the package when the label cannot be read. */
    private fun appLabelFor(packageName: String): String = runCatching {
        val info = packageManager.getApplicationInfo(packageName, 0)
        packageManager.getApplicationLabel(info).toString()
    }.getOrDefault(packageName)
}
