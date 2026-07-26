package com.armsx2

import kr.co.iefriends.pcsx2.NativeApp

/**
 * Serial + CRC for a disc image, resolved WITHOUT booting it.
 *
 * The CRC used to appear only once a game had been launched, because every consumer read
 * `VMManager::GetCurrentCRC()`, which is set from the booted ELF and zeroed on shutdown. That left
 * the Info tab blank outside a game and made patch installs refuse outright, since the core only
 * loads `<SERIAL>_<CRC>.pnach` — you had to boot a game just to be allowed to install a patch for it.
 *
 * The capability already existed: `NativeApp.getGameTitle(path)` calls `GameList::PopulateEntryFromPath`,
 * which opens the image and computes serial and CRC with no VM at all. It had zero callers. This is
 * the wrapper it needed.
 *
 * **Blocking — never call from the main thread.** It reads the image's boot ELF (roughly 1–10 MB,
 * more for a compressed .chd, which decompresses on the way). Results are memoised per path for the
 * process lifetime; a disc image's identity cannot change without the file changing.
 */
object DiscIdentity {
    data class Id(val serial: String?, val crc: String?)

    private val cache = HashMap<String, Id>()

    /** Cached lookup, or null if the image could not be identified. */
    fun of(path: String): Id? {
        if (path.isBlank()) return null
        synchronized(cache) { cache[path]?.let { return it } }
        val id = probe(path) ?: return null
        synchronized(cache) { cache[path] = id }
        return id
    }

    /** Just the CRC as 8 uppercase hex digits, or null. */
    fun crcOf(path: String): String? = of(path)?.crc

    /** Same, from a [GameInfo]'s uri. Derives the native path exactly as the launcher does
     *  (MainActivityRuntime's launchPath), so identity and boot always agree on the same file. */
    fun of(uri: android.net.Uri): Id? = of(nativePath(uri))

    fun crcOf(uri: android.net.Uri): String? = of(uri)?.crc

    private fun nativePath(uri: android.net.Uri): String =
        if (uri.scheme == "file") uri.path ?: uri.toString() else uri.toString()

    private fun probe(path: String): Id? {
        // "title|serial|serial (CRC)" — the third field is the only place the CRC is exposed.
        val raw = runCatching { NativeApp.getGameTitle(path) }.getOrNull()
            ?.takeIf { it.isNotBlank() } ?: return null
        val parts = raw.split('|')
        if (parts.size < 3) return null
        val serial = parts[1].trim().takeIf { it.isNotBlank() }
        // Pull the CRC back out of "SLUS-20870 (FDD12792)". The formatter uses %08X unconditionally,
        // so an unidentified image yields the literal 00000000 sentinel rather than an empty field —
        // reject it, or callers will happily build a <serial>_00000000.pnach that can never load.
        val crc = Regex("\\(([0-9A-Fa-f]{8})\\)").find(parts[2])?.groupValues?.get(1)
            ?.uppercase()?.takeIf { it != "00000000" }
        if (serial == null && crc == null) return null
        return Id(serial, crc)
    }
}
