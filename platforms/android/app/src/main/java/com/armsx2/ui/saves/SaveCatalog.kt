package com.armsx2.ui.saves

import android.content.Context
import com.armsx2.runtime.MainActivityRuntime
import java.io.File

enum class SaveKind { Slot, Autosave }

/**
 * What a save-state file is, read from its name alone.
 *
 * The core writes `<serial> (<CRC>).NN.p2s` for a numbered slot, `<serial> (<CRC>).autosave.p2s` for
 * the newest autosave and `<serial> (<CRC>).autosave.N.p2s` for the older ones. No game has to be
 * running to read that, which is the point: the Save Manager works from the library.
 */
data class SaveFileId(
    val serial: String,
    val crc: String,
    val kind: SaveKind,
    /** 0-based slot of a numbered save, otherwise null. */
    val slot: Int?,
    /** 1 for the newest autosave, 2 and up for the older ones, otherwise null. */
    val autosaveIndex: Int?,
)

object SaveFileName {
    private val NAME = Regex(
        """^(.*?)\s*\(([0-9A-Fa-f]{8})\)\.(?:([0-9]{2})|autosave(?:\.([0-9]+))?)\.p2s$""",
        RegexOption.IGNORE_CASE,
    )

    /** Null for anything that is not a save state: `.p2s.backup` copies, `.drop` leftovers, other files. */
    fun parse(fileName: String): SaveFileId? {
        val m = NAME.matchEntire(fileName) ?: return null
        val serial = m.groupValues[1].trim().uppercase()
        val crc = m.groupValues[2].uppercase()
        val slot = m.groupValues[3].takeIf { it.isNotEmpty() }?.toInt()
        return if (slot != null) {
            SaveFileId(serial, crc, SaveKind.Slot, slot, null)
        } else {
            SaveFileId(serial, crc, SaveKind.Autosave, null, m.groupValues[4].toIntOrNull() ?: 1)
        }
    }
}

data class SaveEntry(
    val id: SaveFileId,
    val file: File,
    /** The `sstates` or `savestates` folder the file was found under. */
    val root: File,
    val modified: Long,
    /** A copy made by the manager's Backup, kept under `<root>/backups/`. */
    val isBackup: Boolean,
) {
    val serial: String get() = id.serial

    /**
     * Whether the Save Manager can load this file. The core loads an autosave only through its own
     * loaders, which look the file up by name in the save folder, so a backup copy of one, sitting
     * in a subfolder, cannot be loaded from there. It can still be deleted.
     */
    val canLoad: Boolean get() = !(isBackup && id.kind == SaveKind.Autosave)
}

/** Every save of one game, already ordered for display. */
data class SaveSet(
    val serial: String,
    /** Newest autosave first. */
    val autosaves: List<SaveEntry>,
    /** Slot 1 first. */
    val slots: List<SaveEntry>,
    /** Newest first. */
    val backups: List<SaveEntry>,
) {
    /** Saves that are live, not backup copies. */
    val count: Int get() = autosaves.size + slots.size

    /** When the game was last saved; a game with only backups falls back to those. */
    val newest: Long
        get() = (autosaves + slots).maxOfOrNull { it.modified } ?: backups.maxOfOrNull { it.modified } ?: 0L
}

object SaveCatalog {
    /**
     * The save folders that exist. A device with a configured system directory has two bases:
     * `assetCopyRoot` resolves to that (typically the SD card) while some states stay under
     * `getExternalFilesDir`. On a test device 13 states were in one and 5 in the other, so reading
     * only one silently under-reports and reads as "this game has no saves".
     */
    fun roots(context: Context): List<File> {
        val bases = buildList {
            runCatching { MainActivityRuntime.assetCopyRoot(context) }.getOrNull()?.let(::add)
            runCatching { context.getExternalFilesDir(null)?.absolutePath }.getOrNull()?.let(::add)
        }.distinct()
        return bases
            .flatMap { base -> listOf("sstates", "savestates").map { File(base, it) } }
            .filter { it.isDirectory }
            .distinctBy { it.absoluteFile.normalize().path }
    }

    fun scan(context: Context): List<SaveEntry> = scan(roots(context))

    /** Every save state under [roots]. The same save found in two roots is listed once, the newer
     *  file winning, since that is the one the emulator wrote last. Backups are never merged. */
    fun scan(roots: List<File>): List<SaveEntry> {
        val found = ArrayList<SaveEntry>()
        for (root in roots) {
            val backups = File(root, "backups")
            val files = runCatching { root.walkTopDown().filter { it.isFile }.toList() }.getOrDefault(emptyList())
            for (file in files) {
                val id = SaveFileName.parse(file.name) ?: continue
                found += SaveEntry(id, file, root, file.lastModified(), isBackup = isUnder(file, backups))
            }
        }
        return found
            .groupBy { e ->
                if (e.isBackup) "backup|" + e.file.absoluteFile.normalize().path
                else "${e.serial}|${e.id.kind}|${e.id.slot ?: e.id.autosaveIndex}"
            }
            .values
            .map { copies -> copies.maxByOrNull { it.modified }!! }
    }

    fun groupBySerial(entries: List<SaveEntry>): List<SaveSet> =
        entries.groupBy { it.serial }
            .map { (serial, list) ->
                SaveSet(
                    serial = serial,
                    autosaves = list.filter { !it.isBackup && it.id.kind == SaveKind.Autosave }
                        .sortedBy { it.id.autosaveIndex },
                    slots = list.filter { !it.isBackup && it.id.kind == SaveKind.Slot }
                        .sortedBy { it.id.slot },
                    backups = list.filter { it.isBackup }.sortedByDescending { it.modified },
                )
            }
            // The game you saved last comes first; the serial settles ties so the order is stable.
            .sortedWith(compareByDescending<SaveSet> { it.newest }.thenBy { it.serial })

    /** Whether [serial] has any live save, for deciding if the library's menu offers the manager. */
    fun hasSaves(context: Context, serial: String?): Boolean {
        if (serial.isNullOrBlank()) return false
        return scan(context).any { !it.isBackup && it.serial.equals(serial, ignoreCase = true) }
    }

    private fun isUnder(file: File, dir: File): Boolean =
        file.absoluteFile.normalize().path.startsWith(dir.absoluteFile.normalize().path + File.separator)
}
