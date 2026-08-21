package com.armsx2

import android.content.Context
import com.armsx2.runtime.MainActivityRuntime
import java.io.File

/**
 * Which save-state slots exist for a game, without the game running.
 *
 * The in-game picker asks the native side (`NativeApp.getGamePathSlot`), which resolves against the
 * *running* VM's serial — so it cannot answer this question from the library, where nothing is
 * booted. This reads the same files off disk instead, using the same layout SaveManagerViewModel
 * walks: `<serial> (title).NN.p2s` under `sstates/` or `savestates/`.
 *
 * Kept deliberately small and blocking. Callers are already on a background dispatcher, and the
 * alternative — threading a ViewModel into the library's long-press menu — is far more machinery
 * than a directory listing deserves.
 */
object SaveSlotLookup {
    private val SLOT_PATTERN = Regex("\\.([0-9]{2})\\.p2s$", RegexOption.IGNORE_CASE)

    /** One existing save state. [slot] is 0..9; [label] is a short human description. */
    data class Slot(val slot: Int, val file: File, val modified: Long)

    /**
     * Slots that exist for [serial], newest first within slot order. Empty when the serial is
     * blank or nothing has been saved — the caller shows no menu entry in that case rather than
     * an empty list, since "Load state" that opens onto nothing is worse than no row at all.
     */
    fun slotsFor(context: Context, serial: String?): List<Slot> {
        if (serial.isNullOrBlank()) return emptyList()
        val root = runCatching { MainActivityRuntime.assetCopyRoot(context) }.getOrNull() ?: return emptyList()
        return listOf("sstates", "savestates")
            .map { File(root, it) }
            .filter { it.isDirectory }
            .flatMap { dir ->
                runCatching {
                    dir.walkTopDown().filter { it.isFile && it.extension.equals("p2s", true) }.toList()
                }.getOrDefault(emptyList())
            }
            // Same serial derivation as the save manager: the name is "<serial> (title).NN.p2s",
            // and everything before the first " (" is the serial.
            .filter { it.name.substringBefore(" (").equals(serial, ignoreCase = true) }
            .mapNotNull { file ->
                val slot = SLOT_PATTERN.find(file.name)?.groupValues?.getOrNull(1)?.toIntOrNull()
                    ?: return@mapNotNull null
                Slot(slot, file, file.lastModified())
            }
            .distinctBy { it.slot }
            .sortedBy { it.slot }
    }
}
