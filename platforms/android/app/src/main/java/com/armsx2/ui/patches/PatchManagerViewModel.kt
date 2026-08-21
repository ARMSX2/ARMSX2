package com.armsx2.ui.patches

import android.app.Application
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.GameInfo
import com.armsx2.PatchRepo
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.config.SettingsScope
import com.armsx2.i18n.I18n
import com.armsx2.ui.InGameOverlay
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

data class PatchManagerUiState(
    val settings: Settings = Settings(),
    val files: List<File> = emptyList(),
    // Online patch/cheat browser (backed by PatchRepo — GitHub PNACH repos by serial).
    val onlineLoading: Boolean = false,
    val onlineTitle: String = "",
    val onlineEntries: List<PatchRepo.Entry> = emptyList(),
    val onlineSelected: Set<String> = emptySet(),
    val onlineSerial: String = "",
    val onlineCrc: String = "",
    // Which game these online results were fetched for (its uri, or "" for none). The whole
    // patch UI shares ONE Activity-scoped ViewModel, so without this a previous game's results
    // linger into the next game's tab. The browser only renders onlineEntries when this equals
    // the game currently on screen — a hard guard that survives any missed lifecycle reset.
    val onlineForGameKey: String = "",
    // Local per-cheat manager: the expanded file's parsed cheats (name/enabled).
    val localExpandedPath: String? = null,
    val localCheats: List<PatchRepo.LocalCheat> = emptyList(),
    // What the BUNDLED patches.zip applies to this game. Invisible until now: the manager only
    // ever listed files on disk, so the patches we ship applied with nothing to show for them.
    val bundledEntry: String = "",
    val bundledCheats: List<PatchRepo.LocalCheat> = emptyList(),
    val bundledUnlabelled: Int = 0,
    // Raw .pnach text editor (#hanafuda: "add the cheat editor back so I can paste codes I grabbed
    // from the web"). Null path = closed. editorNew distinguishes "create" from "edit" so Save
    // knows whether it has to invent a filename.
    val editorPath: String? = null,
    val editorName: String = "",
    val editorText: String = "",
    val editorNew: Boolean = false,
    val editorLoading: Boolean = false,
    val message: String? = null,
    val error: String? = null,
)

class PatchManagerViewModel(application: Application) : AndroidViewModel(application) {
    init {
        // Point the repository-tree cache at app storage. Until this runs the caches are
        // memory-only, which is what made every cold start re-download and re-parse megabytes.
        com.armsx2.PatchRepo.setCacheDir(java.io.File(application.cacheDir, "patch-trees"))
    }

    /** The in-flight online scan, if any. See searchOnline for why this is tracked. */
    private var onlineSearchJob: kotlinx.coroutines.Job? = null

    /**
     * Stop any online scan in progress.
     *
     * Called when the browser leaves the screen. Cancellation is cooperative and PatchRepo now
     * checks between every repository and every file, so this takes effect within one request
     * rather than at the end of the whole scan.
     */
    fun cancelOnlineSearch() {
        onlineSearchJob?.cancel()
        onlineSearchJob = null
        if (state.value.onlineLoading)
            state.value = state.value.copy(onlineLoading = false)
    }

    private companion object {
        /** A user can hand us a storage root by accident; an unbounded SAF walk of that is a hang. */
        const val MAX_IMPORT_DEPTH = 4
        const val MAX_IMPORT_FILES = 200
    }

    var state = androidx.compose.runtime.mutableStateOf(PatchManagerUiState())
        private set

    fun refresh() {
        // Scope the list to the running game. Every .pnach on disk used to be listed under
        // EVERY game — and worse, syncAllEnableLists below then pushed all of their cheat
        // names into the enable list — which is why e.g. Rule of Rose cheats appeared inside
        // FFX. Mirrors the core's own lookup (Patch::GetPnachTemplate): <serial>_<crc>*.pnach
        // or <crc>*.pnach. With no game running (library context) keep the unfiltered list,
        // since there's nothing to scope to.
        // FAIL OPEN. currentSerial is a settingsKey: the serial for discs, but a FILENAME STEM
        // otherwise (and it can be stale from a previously-run game). Scoping on a stem hides
        // every installed file — they're named <SERIAL>_<CRC>.pnach — which showed up as
        // "Installed patches & cheats: 0" right after a successful install. Only scope when we
        // genuinely have a PS2 serial to scope by; otherwise show everything, because hiding a
        // file the user just installed is far worse than listing a few extras.
        val serial = InGameOverlay.currentSerial.value
            ?.trim()?.uppercase()
            ?.takeIf { Regex("^[A-Z]{4}-\\d{5}$").matches(it) }
        val crc = liveCrc()
        val files = patchDirectories().flatMap { directory ->
            if (!directory.isDirectory) emptyList() else directory.walkTopDown().filter { it.isFile && it.extension.equals("pnach", true) }.toList()
        }.filter { f ->
            if (serial == null) true else {
                val n = f.nameWithoutExtension.uppercase()
                n.startsWith("${serial}_") || (crc != null && n.startsWith(crc))
            }
        }.distinctBy { it.absolutePath }.sortedBy { it.name.lowercase() }
        state.value = state.value.copy(settings = scopedSettings(), files = files)
        loadBundled(serial, crc, files.isNotEmpty())
        // NOTHING IS ENABLED HERE. This used to call syncAllEnableLists(files), which walked every
        // .pnach on disk and persisted every uncommented group in it as "enabled" — so merely
        // OPENING this screen armed patches the user had never touched. Community pnach files ship
        // with their patch= lines uncommented, so that meant names like "60 FPS" and
        // "Widescreen 16:9" went into the enable list wholesale; and because the core matches
        // enabled patches purely BY NAME, those names then armed the identically-named group in
        // any of the ~4000 bundled pnach files, for games the user had never opened this screen
        // for. That is the "patches apply with all patch settings off, and won't turn off" bug
        // (KH2's bundled [60 FPS], and 16:9 appearing uninvited).
        //
        // Import still registers its own file (see import's syncEnableListForFile call), which was
        // the only legitimate reason this existed. Reading a screen must never persist state.
    }

    /**
     * The settings tier this tab is being shown for — per-game when opened for a game,
     * else global.
     *
     * This tab used to `loadGlobal()` here and `saveGlobal()` in [update], hard-wired to
     * the global tier at BOTH ends. Every other tab routes through
     * [InGameOverlay.saveSettings], so it was the only one that ignored the Global/Game
     * switch above it: the switch appeared to do nothing on this page, and a patch setting
     * meant for one game was written globally (while the pause menu's own patch rows,
     * which DO honour the scope, wrote per-game — the two then contradicted each other,
     * which is the reported "individual patch settings overwrite global and vice versa").
     */
    private fun scopedSettings(): Settings {
        val serial = InGameOverlay.currentSerial.value?.takeIf { it.isNotEmpty() }
        return if (InGameOverlay.settingsScope.value == SettingsScope.Game && serial != null)
            ConfigStore.resolveForGame(serial)
        else
            ConfigStore.loadGlobal()
    }

    fun update(transform: (Settings) -> Settings) {
        // Transform the CURRENT scoped settings, not this screen's snapshot — see the note in
        // EmulationMenuViewModel.updateSettings. scopedSettings() resolves the same tier this
        // save will land in, so the round-trip is consistent.
        val updated = transform(scopedSettings())
        // The shared entry point: picks the tier from the scope, live-applies, and keeps
        // settingsState in step so the pause menu and the other tabs see the same values.
        InGameOverlay.saveSettings(updated)
        state.value = state.value.copy(settings = updated)
    }

    /** The CRC of whatever is booted, or null. `getGameCRC()` formats "%08X" unconditionally, so
     *  with no VM it returns the literal "00000000" — 8 characters, which sails through a bare
     *  `length == 8` check and yields a `<serial>_00000000.pnach` the core can never load. */
    private fun liveCrc(): String? =
        runCatching { NativeApp.getGameCRC() }.getOrNull()
            ?.takeIf { it.length == 8 && it != "00000000" }?.uppercase()
        // No VM, or a different game booted: identify the image directly rather than refusing.
        // This is why installing a patch used to demand you launch the game first — the CRC was
        // only ever read from the running VM. Blocking (reads the boot ELF), so callers must be off
        // the main thread; import() and installSelected() both are.
            ?: MainActivityRuntime.contextGame.value?.uri?.let { com.armsx2.DiscIdentity.crcOf(it) }

    /** Best known serial: the pause overlay's, then the live VM's, then the last game opened
     *  (which outlives quitting to the library, unlike the other two). */
    private fun bestSerial(): String? =
        (InGameOverlay.currentSerial.value
            ?: runCatching { NativeApp.getGameSerial() }.getOrNull()
            ?: MainActivityRuntime.contextGame.value?.serial)
            ?.trim()?.uppercase()?.takeIf { Regex("^[A-Z]{4}-\\d{5}$").matches(it) }

    /**
     * Import every .pnach in a picked folder, recursively.
     *
     * People keep their cheats as a folder of files, not one file at a time, and the single-file
     * picker made adding a set a repetitive chore. Reuses [import] per file so each one still gets
     * the canonical-name treatment and the cheats/patches routing — a folder import must not be a
     * second, subtly different code path.
     *
     * Requested by Fun (SD712).
     */
    fun importFolder(tree: Uri) {
        val context = getApplication<Application>()
        viewModelScope.launch {
            val found = withContext(Dispatchers.IO) {
                runCatching { collectPnachFiles(context, tree) }.getOrDefault(emptyList())
            }
            if (found.isEmpty()) {
                state.value = state.value.copy(error = I18n.get("patches.import.noneFound"))
                return@launch
            }
            found.forEach { import(it) }
        }
    }

    /** Depth-limited walk of a SAF tree for .pnach files. Bounded because a user can hand us their
     *  whole storage root by accident, and an unbounded walk of that is a hang. */
    private fun collectPnachFiles(context: android.content.Context, tree: Uri): List<Uri> {
        val root = DocumentFile.fromTreeUri(context, tree)
            ?: return emptyList()
        val out = ArrayList<Uri>()
        fun walk(dir: DocumentFile, depth: Int) {
            if (depth > MAX_IMPORT_DEPTH || out.size >= MAX_IMPORT_FILES) return
            dir.listFiles().forEach { f ->
                if (out.size >= MAX_IMPORT_FILES) return
                when {
                    f.isDirectory -> walk(f, depth + 1)
                    f.name?.endsWith(".pnach", ignoreCase = true) == true -> out.add(f.uri)
                    // .txt too: the packs people are handed often ship cheats named that way, and
                    // import() already renames to the canonical form.
                    f.name?.endsWith(".txt", ignoreCase = true) == true -> out.add(f.uri)
                }
            }
        }
        walk(root, 0)
        return out
    }

    fun import(uri: Uri) {
        val context = getApplication<Application>()
        val original = DocumentFile.fromSingleUri(context, uri)?.name?.takeIf(String::isNotBlank) ?: "imported.pnach"
        val stem = original.substringBeforeLast('.')
        // The core only ever globs "<SERIAL>_<CRC>*.pnach" or "<CRC>*.pnach", case-SENSITIVELY
        // (FileSystem::FindFiles -> WildcardMatch defaults to case_sensitive=true). Copying the
        // file under its source name — which is what this did — produced something the Patch
        // Manager happily listed and the core could never load, so it looked installed and did
        // nothing. Rename to the canonical form, keeping the original stem after the CRC: the
        // trailing wildcard still matches it, so the user can recognise their own file.
        val serial = bestSerial()
        val crc = liveCrc()
        // Patch::GetPnachTemplate accepts TWO canonical forms: "<SERIAL>_<CRC>*.pnach" and a bare
        // "<CRC>*.pnach". A stem already in either form only needs its extension corrected.
        //
        // ★ Always build from `stem`, never from `original`. Appending to the full filename turned
        // "F0A6D880.txt" into "F0A6D880.txt.pnach" — reported by Rei Ayanami. That name matches
        // neither glob, so the import looked fine and could never load. It is also exactly the
        // CRC-only case: "F0A6D880" IS a valid pnach name, so the right answer is F0A6D880.pnach.
        val up = stem.uppercase()
        // The prefix must be UPPERCASE to be found: the template is built with "{:08X}" and an
        // uppercase disc serial, and WildcardMatch is case-sensitive — so "f0a6d880.pnach" is just
        // as invisible to the core as "F0A6D880.txt.pnach" was. Uppercase the canonical prefix and
        // leave the user's own trailing text alone so they can still recognise their file.
        val canonicalSerialCrc = Regex("^[A-Z]{4}-\\d{5}_[0-9A-F]{8}").find(up)
        // Exactly 8 hex digits at the start, not part of a longer hex run.
        val leadingCrc = Regex("^[0-9A-F]{8}(?![0-9A-F])").find(up)?.value
        fun canonicalise(prefixLength: Int) = up.take(prefixLength) + stem.drop(prefixLength)
        val requested = when {
            canonicalSerialCrc != null -> "${canonicalise(canonicalSerialCrc.value.length)}.pnach"
            // The stem already leads with THIS game's CRC, so it is already canonical for it.
            leadingCrc != null && crc != null && leadingCrc == crc -> "${canonicalise(8)}.pnach"
            // Otherwise prefer the fully-qualified form: a stem that merely happens to begin with
            // 8 hex characters is not necessarily this game's CRC, and guessing wrong produces a
            // file the core silently never loads.
            serial != null && crc != null -> "${serial}_$crc $stem.pnach"
            // Nothing better available. A leading CRC is still a valid form on its own, and for
            // anything else the caller is warned below that it won't load until re-imported.
            else -> "$stem.pnach"
        }
        // Cheats are gated behind EnableCheats and suppressed under RA hardcore; widescreen and
        // no-interlacing patches must not be. Route by what the file actually contains rather
        // than dumping everything in cheats/ as before.
        val text = runCatching {
            context.contentResolver.openInputStream(uri)?.use { it.readBytes().decodeToString() }
        }.getOrNull().orEmpty()
        val isPatchNotCheat = Regex("(?i)\\[(widescreen|no-?interlacing)|gsaspectratio=|gsinterlacemode=")
            .containsMatchIn(text)
        val dirs = patchDirectories()
        val directory = (if (isPatchNotCheat) dirs[1] else dirs[0]).apply { mkdirs() }
        val target = uniqueFile(directory, requested)
        val success = runCatching {
            context.contentResolver.openInputStream(uri)?.use { input -> target.outputStream().use(input::copyTo) }
                ?: error("Unable to read file")
            target.length() > 0L
        }.getOrDefault(false)
        if (!success) target.delete()
        state.value = if (success) {
            // Both canonical forms count as loadable — a bare <CRC>.pnach is valid, so don't warn
            // about it. Mirrors the naming rules above.
            val n = target.name.uppercase()
            val loadable = Regex("^[A-Z]{4}-\\d{5}_[0-9A-F]{8}").containsMatchIn(n) ||
                Regex("^[0-9A-F]{8}([^0-9A-F]|$)").containsMatchIn(n)
            state.value.copy(
                message = if (loadable) "Imported as ${target.name}."
                else "Imported ${target.name}, but the core only loads <SERIAL>_<CRC>.pnach and " +
                    "no CRC is known yet — launch this game once, then re-import to have it renamed.",
            )
        } else state.value.copy(error = "Patch import failed.")
        if (success) {
            // Register the imported file's enabled (labelled) cheats in the native Enable
            // list BEFORE reloading, or the first reload skips them (see syncEnableListForFile).
            syncEnableListForFile(target)
            reloadCore()
        }
        refresh()
    }

    fun delete(file: File) {
        // Disarm before deleting. The enable list is a list of NAMES, not of files, so deleting the
        // file left its names armed forever — and the core would then satisfy them from the
        // identically-named bundled group, meaning "I deleted the patch and it still applies".
        // Read the names off disk while the file still exists.
        val names = runCatching {
            PatchRepo.parseInstalled(file.readText(), file.parentFile?.name ?: "cheats").second
                .mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct()
        }.getOrDefault(emptyList())
        val success = runCatching { file.delete() }.getOrDefault(false)
        state.value = if (success) state.value.copy(message = "Deleted ${file.name}.") else state.value.copy(error = "Unable to delete ${file.name}.")
        if (success) {
            if (names.isNotEmpty() && MainActivityRuntime.nativeReady.value) {
                val cheatsSection = file.parentFile?.name == "cheats"
                // all = the names to drop, enabled = nothing to re-add.
                runCatching {
                    NativeApp.setEnabledPatches(cheatsSection, names.toTypedArray(), emptyArray())
                }
            }
            reloadCore()
        }
        refresh()
    }

    // -- Online browser: fetch PNACH patch/cheat entries for the current game from
    //    the community GitHub repos (all logic already in PatchRepo), let the user
    //    tick the ones they want, then assemble + install a .pnach. --

    /** Drop the online browser's results. The browser is manual-fetch (nothing repopulates
     *  it until the user taps Search), so on a GAME SWITCH the previous game's fetched
     *  cheats/patches would otherwise linger in the next game's tab — the reported "GT4 shows
     *  GTA:SA's cheats". refresh() deliberately doesn't touch these (it's also called after
     *  import/delete, where wiping a fresh fetch would be wrong), so game-change clearing is
     *  its own call. Also collapses any expanded local file for the same reason. */
    fun resetOnlineForGame() {
        state.value = state.value.copy(
            onlineLoading = false,
            onlineTitle = "",
            onlineEntries = emptyList(),
            onlineSelected = emptySet(),
            onlineSerial = "",
            onlineCrc = "",
            onlineForGameKey = "",
            localExpandedPath = null,
            localCheats = emptyList(),
            error = null,
        )
    }

    fun fetchOnline(game: GameInfo?) {
        val gameKey = game?.uri?.toString() ?: ""
        state.value = state.value.copy(
            onlineLoading = true, error = null, onlineEntries = emptyList(), onlineForGameKey = gameKey,
        )
        // ★ Tracked so it can be STOPPED, and so a second search cannot stack on the first.
        //
        // The scan walks four repositories, each a multi-megabyte GitHub tree that is downloaded
        // and regex-scanned. Left running behind the emulator that is enough CPU to cost a
        // low-end device full speed and heat it badly — reported on a Helio G99, where going
        // back to the game did not stop it. viewModelScope alone was not enough: it only
        // cancels when the ViewModel clears, which does not happen merely because the user
        // returned to the game.
        onlineSearchJob?.cancel()
        onlineSearchJob = viewModelScope.launch {
            // Serial priority: the library's (filename-derived) serial, then the running
            // game's serial, then — for a plainly-named file whose filename yielded no
            // serial and that isn't running — read it straight off the disc image
            // (SYSTEM.CNF), the same probe the library scan uses. The last step is why
            // online patches now work from the library, not only in-game: GameInfo.serial
            // is often null there, but the disc always carries the real serial.
            val serial = withContext(Dispatchers.IO) {
                game?.serial?.takeIf { it.isNotBlank() }
                    ?: runCatching { NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotBlank() }
                    ?: game?.uri?.let { probeSerialFromDisc(it) }
            }
            if (serial == null) {
                state.value = state.value.copy(
                    onlineLoading = false,
                    error = "No game serial to look up patches for. Open this from a game.",
                )
                return@launch
            }
            val result = withContext(Dispatchers.IO) {
                // The bundled patch DB (offline, complete), so patches resolve without the
                // rate-limited/truncating GitHub tree — for BOTH the booted (by-CRC) and the
                // library (by-serial) paths.
                val bundled = File(MainActivityRuntime.assetCopyRoot(getApplication()), "resources/patches.zip")
                val crc = runCatching { NativeApp.getGameCRC() }.getOrNull()?.takeIf { it.length == 8 }
                if (crc != null) PatchRepo.fetchForGame(serial, crc, bundled)
                else PatchRepo.fetchForSerial(serial, bundled)
            }
            state.value = state.value.copy(
                onlineLoading = false,
                onlineTitle = result.gametitle,
                onlineEntries = result.entries,
                onlineSelected = emptySet(),
                onlineSerial = result.serial.ifBlank { serial },
                onlineCrc = result.crc,
                onlineForGameKey = gameKey,
                error = result.error,
            )
        }
    }

    /** Read the PS2 serial straight from the disc image (SYSTEM.CNF) via the same native
     *  probe the library scan uses — the fallback for a plainly-named file whose filename
     *  gave no serial and which isn't the running game. getGameSerialFromFd may tag its
     *  result "platform:serial", so strip any tag back to the bare serial. IO thread. */
    private fun probeSerialFromDisc(uri: Uri): String? = runCatching {
        val descriptor = getApplication<Application>().contentResolver.openFileDescriptor(uri, "r")
            ?: return@runCatching null
        val raw = NativeApp.getGameSerialFromFd(descriptor.detachFd())?.takeIf { it.isNotBlank() }
            ?: return@runCatching null
        raw.substringAfterLast(':').takeIf { it.isNotBlank() }
    }.getOrNull()

    fun toggleOnline(name: String) {
        val selected = state.value.onlineSelected.toMutableSet()
        if (!selected.add(name)) selected.remove(name)
        state.value = state.value.copy(onlineSelected = selected)
    }

    fun installSelected() {
        val snapshot = state.value
        val chosen = snapshot.onlineEntries.filter { it.name in snapshot.onlineSelected }
        if (chosen.isEmpty()) {
            state.value = snapshot.copy(error = "Select at least one patch or cheat first.")
            return
        }
        // Route each selected entry by its declared source. "patches" (widescreen / 60fps /
        // game-fix) → the patches/ folder + [Patches] Enable list, which Patch.cpp applies
        // UNCONDITIONALLY. Everything else → cheats/ + [Cheats], which only applies when
        // Enable Cheats is on (and never under RA hardcore). Writing a patch into cheats/ was
        // the bug: 60fps patches (e.g. KH2FM) silently stopped applying. dirs: [0]=cheats [1]=patches.
        val patchEntries = chosen.filter { it.source == "patches" }
        val cheatEntries = chosen.filter { it.source != "patches" }
        // At LOAD time the core only matches <serial>_<CRC>*.pnach or <CRC>*.pnach
        // (Patch::GetPnachTemplate — the bare <serial>_* wildcard is UI-enumeration only,
        // since FindPatchFilesOnDisk passes all_crcs=for_ui). A serial-only filename
        // therefore matches NOTHING: the install appeared to succeed and the cheats could
        // never apply. Fall back to the running game's CRC, and refuse outright rather than
        // write a file the core can never load.
        // liveCrc() rejects the "00000000" no-VM sentinel, which the old `length == 8` check let
        // through — so this "refuse" branch never fired with nothing booted and it wrote a
        // <serial>_00000000.pnach that could never load.
        val crcForName = snapshot.onlineCrc.takeIf { it.isNotBlank() } ?: liveCrc()
        if (crcForName == null) {
            runCatching {
                NativeApp.emulog(
                    "@@ANDROID_PNACH_INSTALL@@ REFUSED serial=${snapshot.onlineSerial} " +
                        "onlineCrc='${snapshot.onlineCrc}' (no CRC; cannot name a loadable pnach)",
                )
            }
            state.value = state.value.copy(
                error = "Can't install for ${snapshot.onlineSerial}: no disc CRC known. " +
                    "Launch the game once, then install — the core only loads <serial>_<CRC>.pnach.",
            )
            return
        }
        val fileName = "${snapshot.onlineSerial}_${crcForName}.pnach"
        viewModelScope.launch {
            val dirs = patchDirectories()
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    if (patchEntries.isNotEmpty()) {
                        File(dirs[1].apply { mkdirs() }, fileName)
                            .writeText(PatchRepo.buildPnach(snapshot.onlineTitle, patchEntries))
                    }
                    if (cheatEntries.isNotEmpty()) {
                        File(dirs[0].apply { mkdirs() }, fileName)
                            .writeText(PatchRepo.buildPnach(snapshot.onlineTitle, cheatEntries))
                    }
                    true
                }.getOrDefault(false)
            }
            // Decisive install trace: the filename actually written, where, and whether it
            // stuck. "I installed it but the list shows 0" must be answerable from an emulog.
            runCatching {
                NativeApp.emulog(
                    "@@ANDROID_PNACH_INSTALL@@ ok=$ok file=$fileName patches=${patchEntries.size} " +
                        "cheats=${cheatEntries.size} patchDir=${dirs[1].absolutePath} cheatDir=${dirs[0].absolutePath}",
                )
            }
            if (ok) {
                // Labelled groups only apply when their name is in the matching Enable list.
                if (patchEntries.isNotEmpty()) {
                    val pn = patchEntries.mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
                    if (pn.isNotEmpty()) runCatching { NativeApp.setEnabledPatches(false, pn, pn) } // [Patches]
                }
                if (cheatEntries.isNotEmpty()) {
                    update { it.copy(enableCheats = true) } // only cheats are gated on Enable Cheats
                    val cn = cheatEntries.mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
                    if (cn.isNotEmpty()) runCatching { NativeApp.setEnabledPatches(true, cn, cn) } // [Cheats]
                }
                reloadCore()
                state.value = state.value.copy(
                    message = "Installed ${chosen.size} item(s) for ${snapshot.onlineSerial}. Restart the game if it's running.",
                    onlineSelected = emptySet(),
                )
            } else {
                state.value = state.value.copy(error = "Couldn't install the selected patches.")
            }
            refresh()
        }
    }

    // -- Local manager: expand an installed .pnach to reveal its individual cheats and
    //    toggle each one. A toggle rewrites just that cheat's `patch=` lines on disk
    //    (comment out to disable, uncomment to enable), then asks the core to reload.
    //    Rewriting the file — rather than a runtime enable-list call keyed to the loaded
    //    game — keeps the change persistent AND avoids clobbering a different game's
    //    active cheats when the browsed file isn't the one currently running. --

    fun expandLocal(file: File) {
        if (state.value.localExpandedPath == file.absolutePath) {
            state.value = state.value.copy(localExpandedPath = null, localCheats = emptyList())
            return
        }
        // Show the row immediately; fill in cheats once parsed.
        state.value = state.value.copy(localExpandedPath = file.absolutePath, localCheats = emptyList())
        viewModelScope.launch {
            val cheats = withContext(Dispatchers.IO) {
                runCatching {
                    val source = file.parentFile?.name ?: "cheats"
                    PatchRepo.parseInstalled(file.readText(), source).second
                }.getOrDefault(emptyList())
            }
            // Ignore if the user collapsed/switched files while we were parsing.
            if (state.value.localExpandedPath == file.absolutePath) {
                state.value = state.value.copy(localCheats = cheats)
            }
        }
    }

    /**
     * Turn every labelled cheat in the open file on or off in one pass.
     *
     * Requested by Rei: a big community pnach can hold a hundred entries, and disabling them one
     * switch at a time means scrolling the whole list twice. Files like that are exactly the ones
     * you most want to switch off wholesale when a game starts misbehaving.
     *
     * Rewrites the file once rather than per cheat: the same read-modify-write repeated a hundred
     * times is a hundred chances to half-apply, and the enable list only needs pushing once.
     */
    fun setAllLocalCheats(enable: Boolean) {
        val path = state.value.localExpandedPath ?: return
        val before = state.value.localCheats
        if (before.isEmpty() || before.all { it.enabled == enable }) return

        // Optimistic, and carry each new body forward so a later single toggle still finds its
        // block on disk — same reason toggleLocalCheat advances `body`.
        val after = before.map { it.copy(enabled = enable, body = setBodyEnabled(it.body, enable)) }
        state.value = state.value.copy(localCheats = after)

        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    val file = File(path)
                    // Normalise line endings first: the parser builds each body from
                    // lines()+"\n", so a CRLF file never matches verbatim.
                    var text = file.readText().replace("\r\n", "\n").replace("\r", "\n")
                    var changed = 0
                    before.forEachIndexed { i, cheat ->
                        val newBody = after[i].body
                        if (newBody == cheat.body) return@forEachIndexed
                        val replaced = text.replaceFirst(cheat.body, newBody)
                        if (replaced != text) { text = replaced; changed++ }
                    }
                    if (changed == 0) return@runCatching false
                    file.writeText(text)
                    true
                }.getOrDefault(false)
            }
            if (ok) {
                pushEnableList(path)
                reloadCore()
            } else {
                // Nothing was rewritten — put the switches back rather than leave the UI lying.
                state.value = state.value.copy(
                    localCheats = before,
                    error = "Couldn't update ${File(path).name} (unusual formatting). Edit it as text instead.",
                )
            }
        }
    }

    fun toggleLocalCheat(name: String) {
        val path = state.value.localExpandedPath ?: return
        val target = state.value.localCheats.firstOrNull { it.name == name } ?: return
        val nowEnabled = !target.enabled
        val newBody = setBodyEnabled(target.body, nowEnabled)
        // Optimistic UI — flip the switch AND advance the in-memory body so a second
        // toggle of the same cheat still finds its (now-rewritten) block on disk.
        state.value = state.value.copy(
            localCheats = state.value.localCheats.map { if (it.name == name) it.copy(enabled = nowEnabled, body = newBody) else it },
        )
        if (newBody == target.body) return // no patch= lines changed; nothing to write
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    val file = File(path)
                    // The parser builds each cheat's `body` from pnach.lines()+"\n" (always
                    // LF), so a file saved with CRLF/CR line endings never matched verbatim —
                    // that's the "unusual formatting" toggle failure. Normalize the file the
                    // same way before locating the block, then write it back (PCSX2 reads LF).
                    val original = file.readText().replace("\r\n", "\n").replace("\r", "\n")
                    val updated = original.replaceFirst(target.body, newBody)
                    if (updated == original) return@runCatching false // body genuinely not found
                    file.writeText(updated)
                    true
                }.getOrDefault(false)
            }
            if (ok) {
                // PCSX2 only applies a LABELLED patch group ([Name]) whose name is in the
                // [Cheats]/[Patches] "Enable" list (Patch.cpp::EnablePatches auto-enables
                // ONLY unlabelled groups). Uncommenting the patch= lines alone therefore
                // does nothing for the common case of named community cheats. Mirror this
                // file's per-cheat enabled state into that Enable list so labelled cheats
                // actually take effect. This enable-list write was lost in the monorepo UI
                // migration, which kept only the body-comment rewrite — the cause of the
                // "cheats don't work" regression. setEnabledPatches drops only THIS file's
                // names before re-adding the enabled subset, so other games' active cheats
                // (different names) are preserved.
                pushEnableList(path)
                reloadCore()
            } else {
                // Revert both the flip and the body — the file wasn't rewritten.
                state.value = state.value.copy(
                    localCheats = state.value.localCheats.map { if (it.name == name) it.copy(enabled = target.enabled, body = target.body) else it },
                    error = "Couldn't update ${File(path).name} (unusual formatting). Edit it as text instead.",
                )
            }
        }
    }

    /** Comment out (disable) or uncomment (enable) every `patch=` line in a cheat block. */
    private fun setBodyEnabled(body: String, enable: Boolean): String =
        body.lines().joinToString("\n") { line ->
            if (!PatchRepo.isPatchCommand(line)) return@joinToString line
            val lead = line.takeWhile { it == ' ' || it == '\t' }
            val bare = line.substring(lead.length).trimStart('/').trimStart()
            if (enable) lead + bare else "$lead//$bare"
        }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }

    /**
     * What the bundled patches.zip contributes to this game.
     *
     * We ship ~2 MB of community patches and the manager never showed any of it, because it lists
     * files in the patches folder and these live in a zip. So a game could report "3 game patches
     * are active" with nothing anywhere to explain it, let alone turn it off — the core auto-applies
     * every UNLABELLED group it finds (Patch.cpp: "we auto enable anything that's not labelled"),
     * and an unlabelled group has no name for a toggle to hang off.
     *
     * Skipped when the game already has a pnach on disk: the core prefers disk over the zip
     * (EnumeratePnachFiles), so in that case the zip contributes nothing and listing it would be a
     * lie. That same precedence is what makes [extractBundled] a real fix rather than a copy.
     */
    private fun loadBundled(serial: String?, crc: String?, hasDiskFiles: Boolean) {
        if (serial == null || crc == null || hasDiskFiles) {
            state.value = state.value.copy(bundledEntry = "", bundledCheats = emptyList(), bundledUnlabelled = 0)
            return
        }
        viewModelScope.launch {
            val found = withContext(Dispatchers.IO) {
                runCatching {
                    val zip = File(MainActivityRuntime.assetCopyRoot(getApplication()), "resources/patches.zip")
                    if (!zip.isFile) return@runCatching null
                    java.util.zip.ZipFile(zip).use { z ->
                        // Same names the core looks for: <SERIAL>_<CRC>.pnach, then <CRC>.pnach.
                        val names = listOf("${serial}_$crc.pnach", "$crc.pnach")
                        val entry = names.firstNotNullOfOrNull { n ->
                            z.entries().asSequence().firstOrNull { it.name.equals(n, ignoreCase = true) }
                        } ?: return@runCatching null
                        val text = z.getInputStream(entry).bufferedReader().readText()
                        Triple(entry.name, PatchRepo.parseInstalled(text, entry.name).second, text)
                    }
                }.getOrNull()
            }
            state.value = if (found == null) {
                state.value.copy(bundledEntry = "", bundledCheats = emptyList(), bundledUnlabelled = 0)
            } else {
                state.value.copy(
                    bundledEntry = found.first,
                    bundledCheats = found.second,
                    bundledUnlabelled = found.second.count { it.name.equals("Unlabelled", true) },
                )
            }
        }
    }

    /**
     * Copy the bundled pnach into the patches folder so it can actually be edited.
     *
     * Not just convenience: the core prefers a pnach on disk over the zip, and explicitly disables
     * the bundled copy when an unlabelled patch is found on disk ("Disabling any bundled
     * 'patches.zip' patches due to unlabeled patch being loaded"). So extracting hands the user a
     * file whose per-cheat switches work AND takes the invisible zip version out of play — which is
     * the only way to turn an unlabelled bundled patch off at all.
     */
    fun extractBundled() {
        val entry = state.value.bundledEntry.takeIf { it.isNotBlank() } ?: return
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    val root = File(MainActivityRuntime.assetCopyRoot(getApplication()))
                    val zip = File(root, "resources/patches.zip")
                    val dest = uniqueFile(File(root, "patches").apply { mkdirs() }, entry.substringAfterLast('/'))
                    java.util.zip.ZipFile(zip).use { z ->
                        val e = z.getEntry(entry) ?: return@runCatching false
                        dest.writeText(z.getInputStream(e).bufferedReader().readText())
                    }
                    true
                }.getOrDefault(false)
            }
            state.value = state.value.copy(
                message = if (ok) I18n.get("patches.bundled.extracted") else null,
                error = if (ok) null else I18n.get("patches.bundled.extractFailed"),
            )
            if (ok) { refresh(); reloadCore() }
        }
    }

    // ---- Raw .pnach text editor ------------------------------------------
    //
    // Requested by hanafuda: codes found on the web are raw `patch=` lines, and without an editor
    // the only way in was to write the file on a PC and side-load it. Editing the FILE (rather
    // than offering some structured code-entry form) is deliberate — pnach is the format people
    // copy, comments and section headers included, so anything that re-serialised it would mangle
    // what they pasted.

    /** Open an existing .pnach for editing. */
    fun openEditor(file: File) {
        state.value = state.value.copy(
            editorPath = file.absolutePath,
            editorName = file.name,
            editorText = "",
            editorNew = false,
            editorLoading = true,
        )
        viewModelScope.launch {
            val text = withContext(Dispatchers.IO) {
                runCatching { file.readText() }.getOrDefault("")
            }
            if (state.value.editorPath == file.absolutePath) {
                state.value = state.value.copy(editorText = text, editorLoading = false)
            }
        }
    }

    /**
     * Start a new .pnach for the game in context.
     *
     * Named `<SERIAL>_<CRC>.pnach` because that is the only shape the core loads. When the CRC
     * isn't known yet the name still gets created, and [saveEditor] reports that it won't load —
     * better than refusing to let someone paste their codes.
     */
    fun newEditor() {
        state.value = state.value.copy(
            editorPath = "",
            editorName = "",
            editorText = "",
            editorNew = true,
            editorLoading = true,
        )
        viewModelScope.launch {
            // liveCrc() can read the disc, so keep it off the main thread.
            val (serial, crc) = withContext(Dispatchers.IO) { bestSerial() to liveCrc() }
            val name = when {
                serial != null && crc != null -> "${serial}_$crc.pnach"
                serial != null -> "$serial.pnach"
                else -> "patch.pnach"
            }
            if (state.value.editorNew) {
                state.value = state.value.copy(
                    editorName = name,
                    editorLoading = false,
                    // A skeleton, so the format is obvious to someone pasting for the first time.
                    // The group header matters: an UNLABELLED patch auto-applies, a labelled one
                    // has to be switched on, and people pasting raw codes expect them to work.
                    editorText = "gametitle=${MainActivityRuntime.contextGame.value?.title.orEmpty()}\n" +
                        "\n" +
                        "// Paste codes below. Lines look like:\n" +
                        "//   patch=1,EE,00000000,extended,00000000\n",
                )
            }
        }
    }

    fun updateEditorText(text: String) {
        state.value = state.value.copy(editorText = text)
    }

    fun closeEditor() {
        state.value = state.value.copy(
            editorPath = null, editorName = "", editorText = "", editorNew = false, editorLoading = false,
        )
    }

    /**
     * Write the editor buffer to disk and reload the core.
     *
     * Does NOT touch the enable list. A pasted file's groups arm only when the user switches them
     * on in the list below — auto-arming whatever a file happens to contain is exactly the bug
     * that made patches apply with every patch setting off.
     */
    fun saveEditor() {
        val snapshot = state.value
        val path = snapshot.editorPath ?: return
        val text = snapshot.editorText
        val name = snapshot.editorName.trim().ifBlank { "patch.pnach" }
            .let { if (it.endsWith(".pnach", true)) it else "$it.pnach" }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val target = if (snapshot.editorNew) {
                        // Cheats folder: that's where the manager's own installs land, and it is
                        // the section the core reads user cheats from.
                        val dir = patchDirectories().first().apply { mkdirs() }
                        uniqueFile(dir, name)
                    } else {
                        File(path)
                    }
                    target.parentFile?.mkdirs()
                    target.writeText(text)
                    target
                }
            }
            state.value = result.fold(
                onSuccess = { f ->
                    val loadable = Regex("^[A-Z]{4}-\\d{5}_[0-9A-F]{8}", RegexOption.IGNORE_CASE)
                        .containsMatchIn(f.name) ||
                        Regex("^[0-9A-F]{8}([^0-9A-F]|$)", RegexOption.IGNORE_CASE).containsMatchIn(f.name)
                    state.value.copy(
                        editorPath = null, editorName = "", editorText = "", editorNew = false,
                        message = if (loadable) "Saved ${f.name}."
                        else "Saved ${f.name}, but the core only loads <SERIAL>_<CRC>.pnach — " +
                            "launch this game once, then rename or re-save.",
                    )
                },
                onFailure = { state.value.copy(error = "Could not save the patch file.") },
            )
            if (result.isSuccess) {
                reloadCore()
                refresh()
            }
        }
    }

    private fun patchDirectories(): List<File> {
        val root = File(MainActivityRuntime.assetCopyRoot(getApplication()))
        return listOf(File(root, "cheats"), File(root, "patches"), File(root, "cheats_ws"))
    }

    private fun uniqueFile(directory: File, requested: String): File {
        val base = requested.substringBeforeLast('.', requested)
        val extension = requested.substringAfterLast('.', "").let { if (it.isBlank()) "" else ".$it" }
        var file = File(directory, requested)
        var index = 2
        while (file.exists()) file = File(directory, "$base-$index$extension").also { index++ }
        return file
    }

    private fun reloadCore() {
        if (!MainActivityRuntime.nativeReady.value) return
        kotlin.concurrent.thread(name = "armsx2-patch-reload") {
            runCatching { NativeApp.reloadPatches() }
        }
    }

    /** Mirror the currently-expanded file's per-cheat enabled state into the native
     *  [Cheats]/[Patches] "Enable" list so LABELLED groups apply (Patch.cpp only
     *  auto-enables unlabelled groups). Files in the cheats folder use the [Cheats]
     *  section; patches/widescreen use [Patches]. Only this file's names are dropped
     *  then re-added, so a different game's enabled cheats are left intact. */
    private fun pushEnableList(path: String) {
        if (!MainActivityRuntime.nativeReady.value) return
        val cheatsSection = File(path).parentFile?.name == "cheats"
        val all = state.value.localCheats.mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
        if (all.isEmpty()) return
        val enabled = state.value.localCheats.filter { it.enabled }
            .mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
        runCatching { NativeApp.setEnabledPatches(cheatsSection, all, enabled) }
    }

    /** Reflect one file's on-disk body state into the native Enable list (parses it).
     *  Cheats folder → [Cheats] section, else [Patches]. Only this file's names are
     *  dropped then re-added, so other games' enabled cheats are preserved. Synchronous
     *  so callers can sequence it before reloadPatches. */
    private fun syncEnableListForFile(file: File) {
        if (!MainActivityRuntime.nativeReady.value) return
        val cheatsSection = file.parentFile?.name == "cheats"
        val parsed = runCatching {
            PatchRepo.parseInstalled(file.readText(), file.parentFile?.name ?: "cheats").second
        }.getOrDefault(emptyList())
        val all = parsed.mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
        if (all.isEmpty()) return
        val enabled = parsed.filter { it.enabled }
            .mapNotNull { it.name.takeIf(String::isNotBlank) }.distinct().toTypedArray()
        runCatching { NativeApp.setEnabledPatches(cheatsSection, all, enabled) }
    }

    // syncAllEnableLists() was removed deliberately — see the note in refresh(). Reflecting every
    // on-disk file's body into the enable list is only ever correct for a file the user just
    // imported, which import does for itself. Don't reintroduce a bulk variant.
}
