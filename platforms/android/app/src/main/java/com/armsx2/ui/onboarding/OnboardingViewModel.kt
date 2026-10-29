package com.armsx2.ui.onboarding

import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.BiosInfo
import com.armsx2.CustomDriver
import com.armsx2.GpuInfo
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

/** One importable BIOS the user can pick from the setup list. */
data class BiosCandidate(val name: String, val path: String, val info: BiosInfo)

/** The pages of the setup, in order. [Driver] is only there when one of our GPU drivers is built for this device. */
enum class SetupStep(val titleKey: String) {
    Welcome("setup.page.welcome.title"),
    Storage("setup.step.appData.title"),
    Bios("setup.page.bios.title"),
    Games("setup.page.roms.title"),
    Performance("setup.page.performance.title"),
    Driver("setup.page.driver.title"),
    Ready("setup.button.applyFinish"),
}

/** Where the GPU driver page is: looking up the pack, offering it, installing it, or done. */
enum class DriverStage { Loading, Ready, Unreachable, Installing, Installed, Failed }

data class OnboardingUiState(
    val page: Int = 0,
    val steps: List<SetupStep> = SetupStep.entries - SetupStep.Driver,
    val systemLocation: StorageLocation = StorageLocation.Internal,
    val biosName: String? = null,
    val biosInfo: BiosInfo? = null,
    val biosCount: Int = 0,
    // Every BIOS a folder import turned up, so the user can pick right here (not
    // just accept the auto-picked first one). Empty for a single-file import.
    val biosOptions: List<BiosCandidate> = emptyList(),
    val selectedBiosPath: String? = null,
    val gameFolders: List<String> = emptyList(),
    /** Automatic performance tuning (EmuCore/AutoTune). Off unless the player turned it on before. */
    val autoTune: Boolean = false,
    /** Name of the driver source offered on the driver page ("ARMSX2 · MaliSX2"); empty when the page is not shown. */
    val driverSource: String = "",
    /** The offered driver is malisx2, which has a kernel requirement worth telling the player about. */
    val driverIsMali: Boolean = false,
    val driverStage: DriverStage = DriverStage.Loading,
    /** The pack the driver page installs, once the release list has been read. */
    val driverPack: CustomDriver.RemoteDriver? = null,
    /** Name of the driver the page installed, for the confirmation. */
    val driverInstalledName: String? = null,
    val busy: Boolean = false,
    val error: String? = null,
) {
    val step: SetupStep get() = steps[page.coerceIn(0, steps.lastIndex)]
}

enum class StorageLocation { Internal, SdCard, Custom }

class OnboardingViewModel(application: Application) : AndroidViewModel(application) {
    /** The release-list lookup for the driver page, so a second load() does not start another. */
    private var driverOfferJob: Job? = null

    var state = androidx.compose.runtime.mutableStateOf(OnboardingUiState())
        private set

    fun load() {
        val existingBios = MainActivityRuntime.bios.value?.let(::File)?.takeIf(File::isFile)
        val info = existingBios?.let(::probeFile)
        val dir = MainActivityRuntime.systemDir.value
        val renderer = GpuInfo.rendererName()
        // The page is for a player still on the system driver, on a GPU one of our drivers is built
        // for. Decided once here: installing the driver must not take the page away under the player.
        val driverSource = CustomDriver.setupOfferLabel(renderer)
            ?.takeIf { runCatching { ConfigStore.loadGlobal().output.customDriverId.isBlank() }.getOrDefault(true) }
        val steps = if (driverSource != null) SetupStep.entries.toList() else SetupStep.entries - SetupStep.Driver
        state.value = state.value.copy(
            steps = steps,
            driverSource = driverSource.orEmpty(),
            driverIsMali = driverSource != null && CustomDriver.offersMaliSX2(renderer),
            systemLocation = when {
                dir == null -> StorageLocation.Internal
                dir == MainActivityRuntime.sdCardDataDir(getApplication()) -> StorageLocation.SdCard
                else -> StorageLocation.Custom
            },
            biosName = existingBios?.name,
            biosInfo = info,
            selectedBiosPath = existingBios?.absolutePath,
            gameFolders = MainActivityRuntime.romsDirs.value,
            autoTune = runCatching { ConfigStore.loadGlobal().emuCore.autoTune }.getOrDefault(false),
            page = if (MainActivityRuntime.setupComplete.value) 1 else 0,
        )
        if (driverSource != null && driverOfferJob == null) lookUpDriverOffer(renderer)
    }

    fun next() {
        if (canContinue()) state.value = state.value.copy(page = (state.value.page + 1).coerceAtMost(state.value.steps.lastIndex), error = null)
    }

    fun previous() {
        state.value = state.value.copy(page = (state.value.page - 1).coerceAtLeast(0), error = null)
    }

    fun goTo(page: Int) {
        state.value = state.value.copy(page = page.coerceIn(0, state.value.steps.lastIndex), error = null)
    }

    /** Reads the release list of the driver the page offers, off the main thread. */
    private fun lookUpDriverOffer(renderer: String?) {
        state.value = state.value.copy(driverStage = DriverStage.Loading)
        driverOfferJob = viewModelScope.launch {
            val pack = withContext(Dispatchers.IO) { CustomDriver.fetchSetupOffer(renderer) }
            state.value = state.value.copy(
                driverPack = pack,
                driverStage = if (pack != null) DriverStage.Ready else DriverStage.Unreachable,
            )
            driverOfferJob = null
        }
    }

    /** "Try again" after the release list could not be read. */
    fun retryDriverOffer() {
        if (driverOfferJob == null && state.value.driverStage == DriverStage.Unreachable) {
            lookUpDriverOffer(GpuInfo.rendererName())
        }
    }

    /** Downloads the offered driver and makes it the global driver. It takes effect when a game
     *  next starts, the same as picking it in the GPU driver settings. Navigation is locked while
     *  it runs, as for a BIOS import. */
    fun installDriver() {
        val pack = state.value.driverPack ?: return
        val stage = state.value.driverStage
        if (stage != DriverStage.Ready && stage != DriverStage.Failed) return
        state.value = state.value.copy(driverStage = DriverStage.Installing, busy = true)
        val context = getApplication<Application>()
        viewModelScope.launch {
            val installed = withContext(Dispatchers.IO) { CustomDriver.download(context, pack) }
            if (installed != null) {
                runCatching {
                    val global = ConfigStore.loadGlobal()
                    ConfigStore.saveGlobal(global.copy(output = global.output.copy(customDriverId = installed.id)))
                }
                MainActivityRuntime.customDriverId.value = installed.id
            }
            state.value = state.value.copy(
                driverStage = if (installed != null) DriverStage.Installed else DriverStage.Failed,
                driverInstalledName = installed?.name,
                busy = false,
            )
        }
    }

    fun selectStorage(location: StorageLocation) {
        when (location) {
            StorageLocation.Internal -> {
                MainActivityRuntime.systemDir.value = null
                MainActivityRuntime.prefs.edit().remove("systemDir").apply()
                state.value = state.value.copy(systemLocation = location, error = null)
            }
            StorageLocation.SdCard -> {
                val path = MainActivityRuntime.sdCardDataDir(getApplication())
                if (path == null) {
                    state.value = state.value.copy(error = "No writable SD card was found.")
                    return
                }
                MainActivityRuntime.systemDir.value = path
                MainActivityRuntime.prefs.edit().putString("systemDir", path).apply()
                state.value = state.value.copy(systemLocation = location, error = null)
            }
            // Custom is driven by the UI (all-files-access grant + folder pick, github
            // flavor only) — the resolved POSIX path arrives via selectCustomStorage.
            StorageLocation.Custom -> {}
        }
    }

    /** Point the data root at a user-picked folder (github flavor: all-files access).
     *  [path] is a resolved POSIX path under shared storage; the native core writes
     *  memcards / saves / configs there. Requires MANAGE_EXTERNAL_STORAGE, which the
     *  onboarding UI secures before calling this. */
    fun selectCustomStorage(path: String) {
        if (path.isBlank()) {
            state.value = state.value.copy(error = "Couldn't resolve that folder.")
            return
        }
        // Check now, while the player can pick again. Unchecked, an unwritable folder was only
        // found out at the next launch.
        if (!MainActivityRuntime.validateSystemDirWritable(path)) {
            state.value = state.value.copy(error = com.armsx2.i18n.I18n.get("setup.systemDir.error.tryAnother"))
            return
        }
        MainActivityRuntime.systemDir.value = path
        MainActivityRuntime.prefs.edit().putString("systemDir", path).apply()
        state.value = state.value.copy(systemLocation = StorageLocation.Custom, error = null)
    }

    fun importBios(uri: Uri) {
        state.value = state.value.copy(busy = true, error = null)
        val context = getApplication<Application>()
        runCatching { context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        val imported = importBiosFile(context, uri)
        state.value = if (imported != null) {
            val (target, info) = imported
            selectBios(context, target)
            state.value.copy(
                biosName = target.name,
                biosInfo = info,
                biosCount = 1,
                biosOptions = listOf(BiosCandidate(target.name, target.absolutePath, info)),
                selectedBiosPath = target.absolutePath,
                busy = false,
            )
        } else {
            state.value.copy(busy = false, error = "The selected file is not a valid PlayStation 2 BIOS.")
        }
    }

    // Item 7: point at a folder and import every valid BIOS inside it (refresh
    // parity), instead of one file at a time. Runs off the main thread — copying
    // dozens of BIOS files would otherwise freeze setup for ~10s / risk an ANR.
    // Every valid BIOS is kept as a selectable option; we auto-pick the first so
    // setup can proceed, but the user can choose a different one right on this page
    // (or later in BIOS settings).
    fun importBiosFolder(treeUri: Uri) {
        state.value = state.value.copy(busy = true, error = null)
        val context = getApplication<Application>()
        runCatching { context.contentResolver.takePersistableUriPermission(treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        viewModelScope.launch {
            val options = withContext(Dispatchers.IO) {
                val children = DocumentFile.fromTreeUri(context, treeUri)?.listFiles()?.filter(DocumentFile::isFile).orEmpty()
                // Source stem -> imported stem, so the .mec/.nvm companions below can follow
                // whatever name the BIOS actually got after sanitising/de-duplication.
                val importedStems = HashMap<String, String>()
                val candidates = children.mapNotNull { child ->
                    val (target, info) = importBiosFile(context, child.uri) ?: return@mapNotNull null
                    child.name?.substringBeforeLast('.')?.lowercase()?.let { srcStem ->
                        importedStems[srcStem] = target.name.substringBeforeLast('.')
                    }
                    BiosCandidate(target.name, target.absolutePath, info)
                }.sortedBy { it.name.lowercase() }
                // The BIOS's own NVRAM/mechacon files are not BIOS images, so importBiosFile
                // rejects them and they were dropped — the .bin arrived without the console
                // settings that belong to it. Requested by Rei Ayanami.
                copyBiosSiblings(context, children, importedStems)
                candidates
            }
            val chosen = options.firstOrNull()
            state.value = if (chosen != null) {
                selectBios(context, File(chosen.path))
                state.value.copy(
                    biosName = chosen.name,
                    biosInfo = chosen.info,
                    biosCount = options.size,
                    biosOptions = options,
                    selectedBiosPath = chosen.path,
                    busy = false,
                )
            } else {
                state.value.copy(busy = false, error = "No PlayStation 2 BIOS files were found in that folder.")
            }
        }
    }

    /** Copy .mec / .nvm companions in beside the BIOS they belong to, renamed to its final stem
     *  (the core looks for them by stem). Folder import only — a single-document URI cannot see
     *  its siblings. Mirrors BiosManagerViewModel.copyBiosSiblings. */
    private fun copyBiosSiblings(
        context: android.content.Context,
        children: List<DocumentFile>,
        importedStems: Map<String, String>,
    ) {
        if (importedStems.isEmpty()) return
        val directory = MainActivityRuntime.internalBiosDir(context)
        children.forEach { child ->
            val name = child.name ?: return@forEach
            val ext = name.substringAfterLast('.', "").lowercase()
            if (ext != "mec" && ext != "nvm") return@forEach
            val stem = importedStems[name.substringBeforeLast('.').lowercase()] ?: return@forEach
            runCatching {
                val target = File(directory, "$stem.$ext")
                context.contentResolver.openInputStream(child.uri)?.use { input ->
                    target.outputStream().use(input::copyTo)
                }
            }
        }
    }

    /** Switch the active BIOS to one the user tapped in the setup list. */
    fun selectBiosCandidate(candidate: BiosCandidate) {
        selectBios(getApplication(), File(candidate.path))
        state.value = state.value.copy(
            biosName = candidate.name,
            biosInfo = candidate.info,
            selectedBiosPath = candidate.path,
        )
    }

    // Copy+validate a single BIOS into the internal BIOS dir. Returns the stored
    // file and its parsed info, or null for anything that isn't a valid BIOS (so
    // folder import can silently skip non-BIOS files).
    private fun importBiosFile(context: Context, uri: Uri): Pair<File, BiosInfo>? = runCatching {
        val name = DocumentFile.fromSingleUri(context, uri)?.name?.takeIf(String::isNotBlank) ?: "bios.bin"
        val descriptor = context.contentResolver.openFileDescriptor(uri, "r") ?: return@runCatching null
        val info = NativeApp.getBiosInfoFromFd(descriptor.detachFd()) ?: return@runCatching null
        val directory = MainActivityRuntime.internalBiosDir(context).apply { mkdirs() }
        val safeName = name.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val target = File(directory, safeName)
        val temporary = File(directory, ".$safeName.import")
        context.contentResolver.openInputStream(uri)?.use { input ->
            temporary.outputStream().use(input::copyTo)
        } ?: return@runCatching null
        if (temporary.length() == 0L) {
            temporary.delete()
            return@runCatching null
        }
        if (target.exists()) target.delete()
        if (!temporary.renameTo(target)) {
            temporary.copyTo(target, overwrite = true)
            temporary.delete()
        }
        target to info
    }.getOrNull()

    private fun selectBios(context: Context, target: File) {
        MainActivityRuntime.bios.value = target.absolutePath
        MainActivityRuntime.biosDir.value = null
        MainActivityRuntime.prefs.edit().putString("bios", target.absolutePath).remove("biosDir").apply()
    }

    fun addGameFolder(uri: Uri) {
        val context = getApplication<Application>()
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
        }
        val updated = (state.value.gameFolders + uri.toString()).distinct()
        MainActivityRuntime.setRomsDirs(updated)
        state.value = state.value.copy(gameFolders = updated, error = null)
    }

    fun removeGameFolder(uri: String) {
        val updated = state.value.gameFolders - uri
        MainActivityRuntime.setRomsDirs(updated)
        state.value = state.value.copy(gameFolders = updated)
    }

    /** Whether the library scans [uri]'s subfolders too. The library scans again for it the next time
     *  it is on screen. */
    fun setFolderSubfolders(uri: String, include: Boolean) {
        MainActivityRuntime.setRomsDirSubfolders(uri, include)
        com.armsx2.data.library.LibraryRefresh.request()
    }

    fun setAutoTune(enabled: Boolean) {
        state.value = state.value.copy(autoTune = enabled)
    }

    fun canContinue(): Boolean = when (state.value.step) {
        SetupStep.Welcome, SetupStep.Storage, SetupStep.Performance, SetupStep.Driver -> true
        SetupStep.Bios -> state.value.biosInfo != null
        SetupStep.Games -> state.value.gameFolders.isNotEmpty()
        SetupStep.Ready -> state.value.biosInfo != null && state.value.gameFolders.isNotEmpty()
    }

    fun finish() {
        if (!canContinue()) return
        // The answer is saved even when it was left as it was, so the saved settings show the player was asked.
        runCatching {
            val global = ConfigStore.loadGlobal()
            ConfigStore.saveGlobal(global.copy(emuCore = global.emuCore.copy(autoTune = state.value.autoTune)))
        }
        val previousRoot = MainActivityRuntime.currentInitDataRoot()
        MainActivityRuntime.finishSetup()
        // No folder chosen means app-private storage, which is what the pin holds in that case;
        // comparing against null restarted the app every time setup was reopened.
        val selectedRoot = MainActivityRuntime.systemDirPosix()
            ?: getApplication<Application>().getExternalFilesDir(null)?.absolutePath
        if (MainActivityRuntime.nativeReady.value && previousRoot != null && previousRoot != selectedRoot) {
            MainActivityRuntime.restartApp(getApplication())
        }
    }

    fun dismissError() {
        state.value = state.value.copy(error = null)
    }

    private fun probeFile(file: File): BiosInfo? = runCatching {
        val descriptor = android.os.ParcelFileDescriptor.open(file, android.os.ParcelFileDescriptor.MODE_READ_ONLY)
        NativeApp.getBiosInfoFromFd(descriptor.detachFd())
    }.getOrNull()
}

