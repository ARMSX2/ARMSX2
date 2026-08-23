// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: MIT
//
// Licensed MIT, deliberately, and NOT GPL like the rest of ARMSX2.
//
// This file is on the helper side of the Discord process boundary: it links, or
// belongs to the process that links, Discord's proprietary Social SDK. A GPL-3.0+
// file cannot be combined with a proprietary library whose corresponding source we
// cannot supply, so licensing this GPL would recreate exactly the defect the
// separate process exists to avoid -- the boundary has to hold in the licence
// headers as well as in the linker.
//
// MIT rather than Apache-2.0 because it is GPL-compatible in the other direction
// too: the emulator side (DiscordPresence.kt and the Friends UI, which stay GPL)
// consumes the shared IPC definitions here, and that only works if this side is
// permissive.
//
// Do not "fix" this back to the PCSX2 GPL header. It is not PCSX2 code, and the
// licence is load-bearing.
//
// Copyright (c) 2026 ARMSX2 contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

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
    companion object {
        /** Intent extra: also open the browser sign-in, rather than only handing over the Activity. */
        const val EXTRA_AUTHORIZE = "authorize"

        /**
         * True once setEngineActivity has run in THIS process.
         *
         * The SDK keeps the Activity in a static, and statics are per-process, so this resets
         * every time Android restarts :discord -- which it does freely. Anything that needs the
         * SDK must check this rather than assume sign-in already happened.
         */
        @Volatile
        var engineBound = false
            private set

        /** Ran once, when the Activity below hands itself to the SDK. Set by DiscordService. */
        @Volatile
        var onEngineBound: (() -> Unit)? = null
    }

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
        runCatching {
            Class.forName("com.discord.socialsdk.DiscordSocialSdkInit")
                .getMethod("setEngineActivity", Activity::class.java)
                .invoke(null, this)
        }
            .onSuccess {
                engineBound = true
                val waiting = onEngineBound
                onEngineBound = null
                waiting?.invoke()
            }
            .onFailure { Log.w("ARMSX2DiscordSvc", "setEngineActivity failed: ${it.message}") }

        // Only when asked. Handing the Activity over is now also done on a plain start, and that
        // must not drag the browser up with it.
        if (intent?.getBooleanExtra(EXTRA_AUTHORIZE, false) == true) {
            runCatching { DiscordNative.authorize() }
                .onFailure { Log.w("ARMSX2DiscordSvc", "authorize failed: ${it.message}") }
        }

        finish()
        overridePendingTransition(0, 0)
    }
}
