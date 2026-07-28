package com.armsx2.discord

import android.app.Activity
import android.os.Bundle
import android.util.Log

/**
 * Invisible Activity that exists purely to give the SDK an Activity in ITS OWN process.
 *
 * The SDK launches the browser by calling startActivity on an Activity handed to
 * DiscordSocialSdkInit. Since the SDK now lives in :discord, that Activity has to live there too —
 * ARMSX2's MainActivity is in another process and the reference would be meaningless here.
 *
 * It binds itself, kicks off authorization and finishes immediately. The browser hand-back lands on
 * the SDK's own AuthenticationActivity (also declared in :discord), so nothing further is needed
 * here; the app process learns the outcome from the next state poll like any other change.
 */
class DiscordAuthActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (!DiscordNative.load()) {
            finish()
            return
        }

        // Touching any com.discord class runs DiscordSocialSdkInit's static initializer, which
        // System.loadLibrary's the SDK and runs its JNI_OnLoad — a path that ABORTS the process on
        // failure rather than throwing, so runCatching cannot save us. Doing it here means the
        // blast radius is this helper process, never the emulator.
        runCatching { com.discord.socialsdk.DiscordSocialSdkInit.setEngineActivity(this) }
            .onFailure { Log.w("ARMSX2DiscordSvc", "setEngineActivity failed: ${it.message}") }

        runCatching { DiscordNative.authorize() }
            .onFailure { Log.w("ARMSX2DiscordSvc", "authorize failed: ${it.message}") }

        finish()
        overridePendingTransition(0, 0)
    }
}
