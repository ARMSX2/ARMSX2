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

package com.armsx2.discord;

/**
 * JNI surface of libarmsx2_discord.so.
 *
 * Loaded ONLY in the :discord process. That is a licensing boundary, not an implementation detail:
 * ARMSX2 is GPL-3.0+ and the Discord Social SDK is proprietary, so the emulator and the SDK are
 * kept as separate programs that talk over IPC rather than one linked binary. libemucore has no
 * DT_NEEDED on the SDK and never loads it; everything here runs on the far side of that line.
 *
 * Nothing outside com.armsx2.discord should touch this class — the app process talks to
 * {@link DiscordService} instead, and calling these methods from the main process would just fail
 * to link.
 */
public final class DiscordNative {
    private static boolean sLoaded;
    private static boolean sTried;

    private DiscordNative() {}

    /**
     * Load the bridge, once, tolerating its absence.
     *
     * A build without the SDK staged has no libarmsx2_discord.so at all, and an install that
     * somehow lacks it must not take the process down — this is an opt-in feature nobody has
     * necessarily asked for.
     */
    public static synchronized boolean load() {
        if (sTried) return sLoaded;
        sTried = true;
        try {
            System.loadLibrary("armsx2_discord");
            sLoaded = true;
        } catch (Throwable t) {
            android.util.Log.w("ARMSX2Discord", "libarmsx2_discord.so unavailable: " + t.getMessage());
            sLoaded = false;
        }
        return sLoaded;
    }

    /** False when the SDK was not staged at build time. */
    public static native boolean available();

    /** Create the client; a non-empty token skips the browser and connects directly. Idempotent. */
    public static native void start(String savedToken);

    /** Full browser authorization. Needs an Activity bound via DiscordSocialSdkInit first. */
    public static native void authorize();

    /** The freshly-issued token, exactly once, so the caller can persist it. Empty otherwise. */
    public static native String takeToken();

    /** Mirrors BridgeStatus in discord_bridge.cpp. */
    public static native int status();

    /** Last error text, when status() reports failure. */
    public static native String error();

    /** Publish what is running. Empty title = back in the library. raPresence is
     *  RetroAchievements' own rich-presence line, or empty when RA is off. */
    public static native void setPlaying(String serial, String title, String coverUrl, String raPresence);

    /** Friends in ARMSX2, as name/game/serial/avatar records. */
    public static native String friends();

    /** The signed-in account as "name<FS>avatar". */
    public static native String self();

    /** Drain the SDK's callback queue. Must be called from the thread that created the client. */
    public static native void pump();

    /** Tear the client down. */
    public static native void stop();
}
