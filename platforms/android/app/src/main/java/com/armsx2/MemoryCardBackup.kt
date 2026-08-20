package com.armsx2

import android.content.Context
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Rolling per-card snapshots of the PS2 memory cards, so a card that turns up damaged can be put
 * back the way it was.
 *
 * Nothing today keeps a previous version of a card. A file card is written straight through to
 * disk as the game plays — each save seeks into the live file, merges the new bits into the sector
 * and writes it out again — so the card on storage is always the only card there is. Anything that
 * interrupts a write leaves the real card damaged, and the player finds out when they load.
 *
 * This is not [BackupManager]. That exports everything a reinstall would destroy into one archive
 * the user asks for and chooses a home for; it is a migration tool, it is manual, and restoring a
 * card from it also rolls back every setting changed since. This is automatic, per-card, and
 * silent.
 *
 * **When a snapshot is taken.** Immediately before the emulation thread starts, from
 * `MainActivityRuntime.start()`. At that instant the card file is not open, so the copy cannot
 * catch a half-finished write and there is no thread timing to reason about. It also means the
 * copy holds the card as it stood when the player last finished successfully — if this session is
 * the one that breaks things, the snapshot is clean by construction. The cost is that restoring
 * loses the current session's saves, which is the trade a save-state slot already makes.
 *
 * **The rules that make it useful rather than harmful**, both in [snapshotActiveCards]:
 *
 *  - A card that fails [verify] is never snapshotted. There is nothing worth saving and a rotation
 *    slot to lose. That same check is what detects the user's problem, so it is reported rather
 *    than swallowed.
 *  - [prune] never deletes the newest snapshot that passed verification. Without that rule the
 *    rotation is a shredder: the card breaks, the player relaunches five times trying to work out
 *    why, and every good copy has been overwritten with a copy of the broken card.
 *
 * Deliberately written against the Java file APIs rather than the core. The storage memory cards
 * live on is FUSE-emulated on some Android configurations, where libc's file-creation call is
 * denied outright — a problem this tree has hit twice already (see MemoryCardViewModel.create and
 * the CreateDirectoryPath fallback in native-lib.cpp), and worked around both times by doing the
 * work from Java. A snapshot writer in the core would fail silently on exactly the devices that
 * need it most.
 */
object MemoryCardBackup {
    private const val SCHEMA = 1
    private const val MANIFEST = "armsx2-mcbak.json"
    private const val PAYLOAD = "card/"
    private const val EXT = ".mcbak"
    private const val TMP_EXT = ".part"
    private const val BACKUP_DIR = "memcard-backups"
    private const val CARDS_DIR = "memcards"

    /** Scratch space for the restore swap, under the backups root rather than the cards
     *  folder — see [restore]. Leading dot so it never reads as a card backup directory. */
    private const val WORK_DIR = ".restore"

    /** A folder memory card is a directory carrying this marker; the core identifies one by it. */
    private const val SUPERBLOCK = "_pcsx2_superblock"

    private const val PREF_ENABLED = "mcdbackup.enabled"

    /** Newest N snapshots always kept, whatever their age. */
    private const val KEEP_RECENT = 3

    /** ...plus the newest snapshot from each of the preceding N distinct days.
     *
     *  Three copies from one afternoon protect against one afternoon. Corruption that goes
     *  unnoticed for days needs a history that spans days, and thinning to one-per-day covers a
     *  week in seven slots at no extra storage. */
    private const val KEEP_DAYS = 4

    /** The signature the console stamps at the head of a formatted card. Same test the core
     *  trusts in FileMcd_IsMemoryCardFormatted, so a card we call good is one it will mount. */
    private val PS2_MAGIC = "Sony PS2 Memory Card Format".toByteArray(Charsets.US_ASCII)
    private val PSX_MAGIC = "MC".toByteArray(Charsets.US_ASCII)

    /** A PSX card is 128 KB; nothing smaller is a card at all. Deliberately a floor rather than a
     *  table of exact sizes — being strict about the signature and permissive about the size
     *  refuses damaged cards without refusing unusual ones that work fine. */
    private const val MIN_CARD_BYTES = 128L * 1024L

    enum class Health { GOOD, UNREADABLE, MISSING }

    enum class Reason { SESSION, SAVE, MANUAL, PRE_RESTORE }

    data class Snapshot(
        val file: File,
        val cardName: String,
        val takenAt: Long,
        val reason: Reason,
        val sizeBytes: Long,
        val contentHash: String,
        /** Whether the card passed [verify] at the moment this was captured. A PRE_RESTORE
         *  snapshot is the one case where this is routinely false — we snapshot a broken card
         *  before overwriting it precisely so restoring the wrong copy is not the act that
         *  destroys the evidence. */
        val healthy: Boolean,
        val game: String?,
        val isFolder: Boolean,
    ) {
        val takenAtText: String
            get() = SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(Date(takenAt))
    }

    data class Outcome(val ok: Boolean, val detail: String)

    // ---- settings -------------------------------------------------------------------------

    /** On by default. A user who never opens settings is exactly who this is for. */
    fun isEnabled(): Boolean =
        runCatching { MainActivityRuntime.prefs.getBoolean(PREF_ENABLED, true) }.getOrDefault(true)

    fun setEnabled(value: Boolean) {
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(PREF_ENABLED, value).apply() }
    }

    // ---- locations ------------------------------------------------------------------------

    fun cardsDir(context: Context): File =
        File(MainActivityRuntime.assetCopyRoot(context), CARDS_DIR)

    /**
     * Sibling of the cards folder, never inside it. The core enumerates cards by scanning
     * `memcards/`, and the settings screen lists any subdirectory it finds there as a folder
     * memory card — a `backups/` directory inside would show up in the UI as a card you could
     * assign to a slot.
     */
    fun backupsRoot(context: Context): File =
        File(MainActivityRuntime.assetCopyRoot(context), BACKUP_DIR)

    private fun backupsFor(context: Context, cardName: String): File =
        File(backupsRoot(context), sanitize(cardName))

    private fun sanitize(name: String): String =
        name.replace(Regex("""[\\/:*?"<>|]"""), "_").trim().ifBlank { "card" }

    // ---- verification ---------------------------------------------------------------------

    /**
     * Card-level health. Note what this cannot see: a card can pass here perfectly — right size,
     * right signature — while the save file inside it is damaged and the game refuses to load it.
     * Recognising that would mean understanding each game's save format. Which is why the restore
     * UI must always be reachable by hand, not only offered when this trips.
     */
    fun verify(card: File): Health = when {
        !card.exists() -> Health.MISSING
        card.isDirectory -> if (File(card, SUPERBLOCK).exists()) Health.GOOD else Health.UNREADABLE
        card.length() < MIN_CARD_BYTES -> Health.UNREADABLE
        else -> runCatching {
            val head = ByteArray(PS2_MAGIC.size)
            val read = card.inputStream().use { it.read(head) }
            when {
                read >= PS2_MAGIC.size && head.contentEquals(PS2_MAGIC) -> Health.GOOD
                read >= PSX_MAGIC.size && head.copyOf(PSX_MAGIC.size).contentEquals(PSX_MAGIC) -> Health.GOOD
                else -> Health.UNREADABLE
            }
        }.getOrDefault(Health.UNREADABLE)
    }

    /**
     * Content hash, so a launch that changed nothing does not cost a rotation slot. Folder cards
     * hash over (relative path, length, bytes) in sorted path order so the result does not depend
     * on directory enumeration order.
     */
    fun hash(card: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(64 * 1024)
        fun feed(f: File) {
            f.inputStream().use { input ->
                while (true) {
                    val n = input.read(buffer)
                    if (n <= 0) break
                    digest.update(buffer, 0, n)
                }
            }
        }
        if (card.isDirectory) {
            card.walkTopDown().filter { it.isFile }.sortedBy { it.relativeTo(card).invariantPath() }
                .forEach { f ->
                    digest.update(f.relativeTo(card).invariantPath().toByteArray(Charsets.UTF_8))
                    digest.update(0)
                    digest.update(java.nio.ByteBuffer.allocate(8).putLong(f.length()).array())
                    feed(f)
                }
        } else {
            feed(card)
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun File.invariantPath(): String = path.replace(File.separatorChar, '/')

    private fun sizeOf(card: File): Long =
        if (card.isDirectory) card.walkTopDown().filter { it.isFile }.sumOf { it.length() } else card.length()

    // ---- listing --------------------------------------------------------------------------

    /** Newest first. A snapshot whose manifest is unreadable is still listed, marked unhealthy —
     *  hiding it would be worse than showing something the user can judge. */
    fun list(context: Context, cardName: String): List<Snapshot> =
        backupsFor(context, cardName).listFiles()
            .orEmpty()
            .filter { it.isFile && it.name.endsWith(EXT) }
            .mapNotNull { read(it, cardName) }
            .sortedByDescending { it.takenAt }

    private fun read(f: File, cardName: String): Snapshot? {
        val fallbackTime = f.name.substringBefore('-').toLongOrNull() ?: f.lastModified()
        val manifest = runCatching {
            ZipInputStream(f.inputStream().buffered()).use { zip ->
                while (true) {
                    val e = zip.nextEntry ?: break
                    if (e.name == MANIFEST) return@use JSONObject(zip.readBytes().toString(Charsets.UTF_8))
                    zip.closeEntry()
                }
                null
            }
        }.getOrNull()
        if (manifest == null) {
            return Snapshot(f, cardName, fallbackTime, Reason.MANUAL, f.length(), "", false, null, false)
        }
        return Snapshot(
            file = f,
            cardName = manifest.optString("card", cardName),
            takenAt = manifest.optLong("takenAt", fallbackTime),
            reason = runCatching { Reason.valueOf(manifest.optString("reason", "MANUAL")) }
                .getOrDefault(Reason.MANUAL),
            sizeBytes = manifest.optLong("sizeBytes", 0L),
            contentHash = manifest.optString("hash", ""),
            healthy = manifest.optBoolean("healthy", false),
            game = manifest.optString("game", "").ifBlank { null },
            isFolder = manifest.optBoolean("folder", false),
        )
    }

    // ---- capture --------------------------------------------------------------------------

    /**
     * The gate, in order: the card is one we are about to use, it verifies, and its contents
     * differ from the newest snapshot. Returns a human-readable summary for the log.
     *
     * [serial] resolves the per-game card override — a game with its own card must snapshot that
     * card, not the global one. Null falls back to global, which is right for a BIOS boot.
     */
    fun snapshotActiveCards(context: Context, serial: String?, reason: Reason, game: String?): String {
        if (!isEnabled()) return "disabled"

        val cfg = runCatching { ConfigStore.resolveForGame(serial) }
            .getOrElse { runCatching { ConfigStore.loadGlobal() }.getOrNull() ?: return "no config" }

        val active = buildList {
            if (cfg.memoryCardSlot1Enabled) add(cfg.memoryCardSlot1Filename)
            if (cfg.memoryCardSlot2Enabled) add(cfg.memoryCardSlot2Filename)
        }.filter { it.isNotBlank() }.distinct()

        if (active.isEmpty()) return "no active cards"

        val notes = mutableListOf<String>()
        for (name in active) {
            val card = File(cardsDir(context), name)
            when (verify(card)) {
                Health.MISSING -> {
                    // Not an error: the core creates a card on first use, so a fresh install has
                    // nothing here yet.
                    notes += "$name: absent"
                    continue
                }
                Health.UNREADABLE -> {
                    // The corruption signal. Refuse to snapshot — there is nothing worth saving
                    // and a rotation slot to lose. The caller surfaces this.
                    notes += "$name: UNREADABLE"
                    continue
                }
                Health.GOOD -> Unit
            }

            val digest = runCatching { hash(card) }.getOrElse {
                notes += "$name: unreadable while hashing"
                continue
            }
            if (list(context, name).firstOrNull()?.contentHash == digest) {
                notes += "$name: unchanged"
                continue
            }

            val snap = snapshot(context, card, reason, game, healthy = true, contentHash = digest)
            notes += if (snap != null) "$name: snapshotted" else "$name: FAILED"
            prune(context, name)
        }
        return notes.joinToString("; ")
    }

    /** Cards flagged UNREADABLE by the launch gate, for the restore prompt. */
    fun unreadableActiveCards(context: Context, serial: String?): List<String> {
        val cfg = runCatching { ConfigStore.resolveForGame(serial) }
            .getOrElse { runCatching { ConfigStore.loadGlobal() }.getOrNull() ?: return emptyList() }
        return buildList {
            if (cfg.memoryCardSlot1Enabled) add(cfg.memoryCardSlot1Filename)
            if (cfg.memoryCardSlot2Enabled) add(cfg.memoryCardSlot2Filename)
        }.filter { it.isNotBlank() }.distinct()
            .filter { verify(File(cardsDir(context), it)) == Health.UNREADABLE }
            .filter { list(context, it).any { s -> s.healthy } }
    }

    /**
     * Write one snapshot. Blocking.
     *
     * Written to a `.part` file and renamed into place, so a process killed mid-write leaves no
     * truncated archive that would list as a valid snapshot — which would be the same class of bug
     * this whole feature exists to undo.
     */
    fun snapshot(
        context: Context,
        card: File,
        reason: Reason,
        game: String?,
        healthy: Boolean = verify(card) == Health.GOOD,
        contentHash: String = runCatching { hash(card) }.getOrDefault(""),
    ): Snapshot? {
        val dir = backupsFor(context, card.name).apply { mkdirs() }
        val takenAt = System.currentTimeMillis()
        val target = File(dir, "$takenAt-${reason.name.lowercase(Locale.ROOT)}$EXT")
        val part = File(dir, target.name + TMP_EXT)
        val isFolder = card.isDirectory
        val sizeBytes = sizeOf(card)

        val built = runCatching {
            ZipOutputStream(part.outputStream().buffered()).use { zip ->
                val manifest = JSONObject()
                    .put("schemaVersion", SCHEMA)
                    .put("card", card.name)
                    .put("takenAt", takenAt)
                    .put("reason", reason.name)
                    .put("sizeBytes", sizeBytes)
                    .put("hash", contentHash)
                    .put("healthy", healthy)
                    .put("folder", isFolder)
                    .put("game", game ?: "")
                    .put("appVersion", appVersion())
                zip.putNextEntry(ZipEntry(MANIFEST))
                zip.write(manifest.toString().toByteArray(Charsets.UTF_8))
                zip.closeEntry()

                if (isFolder) {
                    card.walkTopDown().filter { it.isFile }.forEach { f ->
                        zip.putNextEntry(ZipEntry(PAYLOAD + f.relativeTo(card).invariantPath()))
                        f.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                    }
                } else {
                    zip.putNextEntry(ZipEntry(PAYLOAD + card.name))
                    card.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
            true
        }.getOrDefault(false)

        if (!built || !part.renameTo(target)) {
            part.delete()
            return null
        }
        return read(target, card.name)
    }

    private fun appVersion(): String =
        runCatching { kr.co.iefriends.pcsx2.NativeApp.getBuildVersion() }.getOrDefault("")

    // ---- retention ------------------------------------------------------------------------

    /**
     * Keep the newest [KEEP_RECENT], plus the newest from each of the preceding [KEEP_DAYS]
     * distinct days, plus — always — the newest snapshot that passed verification, however old
     * and even when it falls outside both.
     *
     * That last clause is the load-bearing one. Refusing to snapshot an unreadable card already
     * stops the rotation filling with garbage in the ordinary case; this is the second line of
     * defence, for the case that matters more — a card whose header is intact and whose contents
     * are subtly wrong.
     */
    fun prune(context: Context, cardName: String) {
        val all = list(context, cardName)
        if (all.isEmpty()) return

        val keep = HashSet<File>()
        all.take(KEEP_RECENT).forEach { keep += it.file }

        val seenDays = LinkedHashSet<String>()
        val recentDays = all.take(KEEP_RECENT).map { dayKey(it.takenAt) }.toSet()
        for (snap in all) {
            val day = dayKey(snap.takenAt)
            if (day in recentDays || day in seenDays) continue
            seenDays += day
            keep += snap.file
            if (seenDays.size >= KEEP_DAYS) break
        }

        all.firstOrNull { it.healthy }?.let { keep += it.file }

        all.filter { it.file !in keep }.forEach { it.file.delete() }
    }

    private fun dayKey(millis: Long): String {
        val c = Calendar.getInstance().apply { timeInMillis = millis }
        return "%04d-%03d".format(c.get(Calendar.YEAR), c.get(Calendar.DAY_OF_YEAR))
    }

    // ---- restore --------------------------------------------------------------------------

    /**
     * Put [snap] back over the live card. Blocking.
     *
     * The caller must have the VM stopped. Restoring under a running game does not work and
     * cannot be made to work by reopening the card: the console keeps its own cached picture of
     * the card's directory in guest memory, so swapping the bytes underneath leaves that picture
     * stale and the next write scribbles over the restore.
     *
     * Takes a PRE_RESTORE snapshot of the live card first, unconditionally — including a broken
     * one, marked unhealthy. Restoring the wrong copy must not be the act that destroys the
     * evidence.
     */
    fun restore(context: Context, snap: Snapshot): Outcome {
        val cards = cardsDir(context).apply { mkdirs() }
        val live = File(cards, snap.cardName)

        if (live.exists()) {
            snapshot(context, live, Reason.PRE_RESTORE, game = null)
        }

        // Staging lives OUTSIDE the cards folder. A folder memory card is a directory, and the
        // card list shows every directory under memcards/ as a folder card — so staging there
        // would flash a phantom card mid-restore, and leave one for good if the process died.
        // Same data root, so the renames below are still within one filesystem.
        val work = File(backupsRoot(context), WORK_DIR).apply { mkdirs() }
        val staging = File(work, snap.cardName)
        staging.deleteRecursively()

        val unpacked = runCatching {
            var entries = 0
            ZipInputStream(snap.file.inputStream().buffered()).use { zip ->
                while (true) {
                    val e = zip.nextEntry ?: break
                    if (e.isDirectory || !e.name.startsWith(PAYLOAD)) { zip.closeEntry(); continue }
                    val rel = e.name.removePrefix(PAYLOAD)
                    val dest = if (snap.isFolder) File(staging, rel) else staging
                    // Zip-slip guard: a crafted entry name like "../../x" would otherwise write
                    // anywhere the app can reach. Compare canonical paths, not the raw strings.
                    if (snap.isFolder &&
                        !dest.canonicalPath.startsWith(staging.canonicalPath + File.separator)
                    ) {
                        zip.closeEntry()
                        continue
                    }
                    dest.parentFile?.mkdirs()
                    dest.outputStream().use { zip.copyTo(it) }
                    entries++
                    zip.closeEntry()
                }
            }
            entries
        }.getOrDefault(0)

        if (unpacked == 0) {
            staging.deleteRecursively()
            return Outcome(false, "That backup could not be read.")
        }

        // Swap through a sidelined copy rather than deleting first, so a failure part-way leaves
        // the original recoverable instead of leaving no card at all.
        val sidelined = File(work, "${snap.cardName}.previous")
        sidelined.deleteRecursively()
        if (live.exists() && !live.renameTo(sidelined)) {
            staging.deleteRecursively()
            return Outcome(false, "The current card is in use and could not be replaced.")
        }
        if (!staging.renameTo(live)) {
            sidelined.renameTo(live)
            staging.deleteRecursively()
            return Outcome(false, "The restored card could not be put in place.")
        }
        sidelined.deleteRecursively()
        work.delete()

        return Outcome(true, "Restored ${snap.cardName} from ${snap.takenAtText}.")
    }
}
