// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

import java.io.File

/**
 * The account names this device has saved, so [Scrubber] can take them out of logs. Only the
 * RetroAchievements user name is stored on the device: the core logs it when it signs in
 * (`Achievements.cpp`) and so does rcheevos, and both end up in `emulog.txt` and logcat.
 *
 * Read from the core's settings file rather than asked of the core, because a crash report is
 * prepared at the first moment of the next launch, before the core has started.
 */
object AccountNames {
    private const val INI_FILE = "PCSX2-Android.ini"

    /** Every `Username` value in the `[Achievements]` section of an INI file's [text]. */
    fun fromIni(text: String): List<String> {
        val names = mutableListOf<String>()
        var inSection = false
        for (raw in text.lineSequence()) {
            val line = raw.trim()
            if (line.startsWith("[")) {
                inSection = line.equals("[Achievements]", ignoreCase = true)
                continue
            }
            if (!inSection) continue
            val eq = line.indexOf('=')
            if (eq <= 0) continue
            if (line.substring(0, eq).trim().equals("Username", ignoreCase = true)) {
                line.substring(eq + 1).trim().takeIf { it.isNotEmpty() }?.let(names::add)
            }
        }
        return names
    }

    /** The names saved under [dataRoot], plus [extra] ones the app keeps elsewhere. Never throws. */
    fun read(dataRoot: File?, extra: Collection<String> = emptyList()): List<String> {
        val fromFile = runCatching {
            val ini = dataRoot?.let { File(it, INI_FILE) }
            if (ini != null && ini.isFile && ini.length() < MAX_INI_BYTES) fromIni(ini.readText()) else emptyList()
        }.getOrDefault(emptyList())
        return (fromFile + extra).map { it.trim() }.filter { it.isNotEmpty() }.distinct()
    }

    private const val MAX_INI_BYTES = 4L * 1024 * 1024
}
