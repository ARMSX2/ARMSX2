package com.armsx2

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Process
import android.os.SystemClock
import android.provider.Settings
import android.view.InputDevice
import android.view.KeyEvent
import android.view.KeyCharacterMap
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.addCallback
import androidx.activity.compose.setContent
import androidx.activity.result.ActivityResult
import androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.armsx2.input.ControllerMappings
import com.armsx2.events.TestResult
import com.armsx2.ui.Colors
import com.armsx2.ui.GamesList
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.SetupImpl
import com.armsx2.ui.WindowImpl
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.Android
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.launch
import androidx.lifecycle.lifecycleScope
import org.libsdl.app.HIDDeviceManager
import kr.co.iefriends.pcsx2.MainActivity
import kr.co.iefriends.pcsx2.NativeApp
import org.libsdl.app.SDLControllerManager
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.min

class SurfaceCallbacks(context: Context) : SurfaceView(context), SurfaceHolder.Callback {
    init {
        holder.addCallback(this)
        // Make the SurfaceView itself focusable so gamepad key events
        // route here directly without requiring a tap-to-focus or A-press
        // to grant focus first. The Compose AndroidView wrapper also has
        // .focusable() + a focusRequester pinned to it; both layers
        // converge on this view as the focus target.
        isFocusable = true
        isFocusableInTouchMode = true
        // API 26+ draws a built-in semi-transparent focus highlight over
        // focusable Views on focus. With the SurfaceView focused (D-pad /
        // gamepad path) that overlay tints the game output grey. Suppress
        // it — we never paint a "selected" affordance on the surface.
        defaultFocusHighlightEnabled = false
    }
    override fun surfaceCreated(holder: SurfaceHolder) {
        // Pull focus the moment the surface is ready. Without this the
        // AndroidView starts un-focused and gamepad input falls on the
        // floor until the user touches the screen / presses A.
        requestFocus()
    }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeApp.onNativeSurfaceChanged(holder.surface, width, height)
    }
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeApp.onNativeSurfaceChanged(null, 0, 0)
    }

}

private const val STICK_DEAD = 0.15f
private const val UI_NAV_DEAD = 0.20f
private const val UI_NAV_RELEASE_DEAD = 0.06f
private const val UI_HAT_DEAD = 0.50f
private const val UI_NAV_DOMINANCE = 1.35f
private const val UI_OVERLAY_RELEASE_MS = 80L
private const val UI_KEY_AXIS_SUPPRESS_MS = 220L
// Hold-to-repeat cadence for controller menu navigation: first auto-repeat
// after the initial hold, then steady repeats while the stick/dpad is held.
private const val NAV_REPEAT_INITIAL_MS = 340L
private const val NAV_REPEAT_INTERVAL_MS = 110L

val codeGenTests = mutableStateOf("")
val patchTests = mutableStateOf("")
val vuJitTests = mutableStateOf("")
val eeJitTests = mutableStateOf("")
val vifTests = mutableStateOf("")
val eeSeqTests = mutableStateOf("")

/**
 * Gate UI shown when the user's system folder is outside the app-private
 * sandbox and MANAGE_EXTERNAL_STORAGE hasn't been granted. Blocks the main
 * emulator UI until the user either flips the toggle in Settings or opts
 * to fall back to the app-private folder.
 */
@androidx.compose.runtime.Composable
fun AllFilesAccessScreen(onGrant: () -> Unit, onUseAppPrivate: () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(Colors.surface.value),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxSize().padding(32.dp),
            verticalArrangement = androidx.compose.foundation.layout.Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Storage Access Required",
                color = Color.White,
                fontSize = 26.sp,
                fontWeight = androidx.compose.ui.text.font.FontWeight.Bold,
            )
            Spacer(Modifier.size(12.dp))
            Text(
                "Your chosen system folder is outside the app's private storage. " +
                "Android requires the \"All files access\" permission to read and write " +
                "memory cards, save states, shaders, and logs there.",
                color = Color.LightGray,
                fontSize = 14.sp,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
            Spacer(Modifier.size(24.dp))
            androidx.compose.material3.Button(
                onClick = onGrant,
                colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                    containerColor = Colors.pasx2_blue,
                    contentColor = Color.White,
                ),
                shape = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(
                    horizontal = 18.dp, vertical = 10.dp),
            ) {
                Text("Grant Access")
            }
            Spacer(Modifier.size(12.dp))
            androidx.compose.material3.Button(
                onClick = onUseAppPrivate,
                colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF333333),
                    contentColor = Color.White,
                ),
                shape = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(
                    horizontal = 18.dp, vertical = 10.dp),
            ) {
                Text("Use App-Private Folder Instead")
            }
            Spacer(Modifier.size(20.dp))
            Text(
                "Tip: after granting, return here — access takes effect immediately.",
                color = Color(0xFF888888),
                fontSize = 12.sp,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
        }
    }
}

class Main: ComponentActivity() {
    private var lastUiNavCode = 0
    private var lastUiNavAt = 0L
    private var lastUiNavWasAxis = false
    private var overlayAxisX = 0
    private var overlayAxisY = 0
    private var overlayHorizontalReleaseAt = 0L
    private var libraryAxisX = 0
    private var libraryAxisY = 0

    companion object {
        var instance : Main? = null
        lateinit var prefs: SharedPreferences
        val setupComplete = mutableStateOf(false)
        val setupEditorVisible = mutableStateOf(false)
        val nativeReady = mutableStateOf(false)
        // Tree URI of the user-picked PCSX2 system folder (where bios/,
        // memcards/, etc. should live). Persisted as `systemDir` pref.
        // When unset, emucore falls back to getExternalFilesDir(null)
        // (Android/data/<package>/files).
        val systemDir = mutableStateOf<String?>(null)
        val bios = mutableStateOf<String?>(null)
        // Tree URI of the folder the user picked their BIOS from. Persisted
        // separately from `bios` (the path of the copied private file) so
        // re-entering setup can re-scan the original folder without
        // forcing the user to re-pick.
        val biosDir = mutableStateOf<String?>(null)

        /** Persisted list of ROM-folder tree URIs. Replaces the legacy
         *  single-folder `romsDir` pref (kept readable as a one-element
         *  list at load time). The setup wizard's ROMs page lets the user
         *  add/remove entries; GamesList scans every entry and merges
         *  results de-duplicated by URI. Empty list = no library. */
        val romsDirs = mutableStateOf<List<String>>(emptyList())

        /** Update [romsDirs] state and persist as JSON. Drops the legacy
         *  single-string pref so we don't keep two views in sync forever. */
        fun setRomsDirs(dirs: List<String>) {
            romsDirs.value = dirs
            val arr = org.json.JSONArray()
            for (d in dirs) arr.put(d)
            prefs.edit()
                .putString("romsDirs", arr.toString())
                .remove("roms")
                .apply()
        }

        // Default backend is "auto" — emucore's GSUtil::GetPreferredRenderer
        // picks at runtime per device. The setup wizard no longer asks; the
        // in-game overlay's Renderer tab is where users override (OpenGL /
        // Software cycle, plus Mali/Adreno-specific paths once those land).
        // `upscale` (1.0..5.0) still persists; it's exposed in the in-game
        // overlay's Renderer tab.
        val renderer = mutableStateOf("auto")
        val upscale = mutableStateOf(1.0f)

        /** Active custom Vulkan driver id (matches `CustomDriver.InstalledDriver.id`).
         *  Null = system Vulkan loader. Set from the setup wizard's driver
         *  chip. Applied to native via CustomDriver.applyToNative inside
         *  applyRendererPrefs BEFORE runVMThread enters MTGS::Open, which
         *  is when Vulkan::LoadVulkanLibrary reads the pinned path. */
        val customDriverId = mutableStateOf<String?>(null)

        /**
         * Tracks Android 11+ "All files access" (MANAGE_EXTERNAL_STORAGE)
         * grant. Required when the user's `systemDir` points outside the
         * app-private sandbox: emucore writes memcards / savestates /
         * configs / shaders / patches.zip / logs via raw `fopen`/`mkdir`,
         * which scoped storage rejects with EACCES even when the SAF
         * persistable URI was granted. Refreshed on Activity.onResume so
         * a return-from-Settings flips this true automatically.
         *
         * Always true on pre-Android 11 (scoped storage doesn't apply).
         */
        val allFilesAccessGranted = mutableStateOf(true)

        /** True iff Build.VERSION.SDK_INT >= R AND not granted. Pre-R the
         *  legacy WRITE_EXTERNAL_STORAGE grant covers things, so we never
         *  prompt. Used by the UI to decide whether to show the grant
         *  screen. */
        fun needsAllFilesAccess(): Boolean {
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                   !Environment.isExternalStorageManager()
        }

        /** Returns true iff the resolved system dir is the app-private
         *  externalFilesDir (or unset → defaults to it). When this is
         *  true the MANAGE_EXTERNAL_STORAGE grant isn't needed; emucore
         *  writes are inside the scoped sandbox. */
        fun systemDirIsAppPrivate(context: Context): Boolean {
            val resolved = systemDirPosix() ?: return true
            val privateRoot = context.getExternalFilesDir(null)?.absolutePath ?: return false
            return resolved.startsWith(privateRoot)
        }

        /** Open Settings → "All files access" for this app so the user
         *  can flip the toggle. Returns true if the intent launched. */
        fun requestAllFilesAccess(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
            return try {
                val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                intent.data = Uri.parse("package:${context.packageName}")
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
                true
            } catch (_: Exception) {
                // Fallback to the all-apps version of the screen on devices
                // whose ROM rejects the package-targeted variant.
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    context.startActivity(intent)
                    true
                } catch (_: Exception) { false }
            }
        }

        private val eDispatcher = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        private val eScope = CoroutineScope(eDispatcher)

        /**
         * Resolve the user-chosen system folder (a SAF tree URI persisted
         * as `systemDir`) to a POSIX path emucore can use as
         * `EmuFolders::DataRoot`. Memcards / savestates / configs land
         * under it.
         *
         * Tree URIs from OpenDocumentTree look like
         *   content://com.android.externalstorage.documents/tree/primary%3APCSX2
         * The "primary:" prefix means the volume is the primary external
         * storage (`/storage/emulated/0`); other prefixes are SD-card or
         * removable volume IDs which mount under `/storage/<volumeId>`.
         *
         * Returns null when systemDir is unset, malformed, or this
         * Android build can't translate the tree URI (rare). Caller
         * falls back to the app's externalFilesDir in that case.
         *
         * Caveat: emucore's POSIX FileSystem APIs require the resolved
         * path to be writable. On Android 11+ that's usually only true
         * for app-owned scoped paths. Folders the user picks elsewhere
         * may translate fine but fail on write — that's a SAF-vs-POSIX
         * gap, not a translation bug.
         */
        fun systemDirPosix(): String? = resolveTreeUriToPosix(systemDir.value)

        /** Directory holding the configured BIOS file, used by
         *  NativeApp.initializeOnce to point EmuFolders::Bios at the real
         *  BIOS location (which tracks the chosen data root). Null when no
         *  BIOS is configured yet — initializeOnce then falls back to
         *  <dataRoot>/bios. */
        fun biosFolderPosix(): String? =
            bios.value?.takeIf { it.isNotEmpty() }?.let { File(it).parent }

        /** URI-string-independent POSIX resolver. Pulled out of
         *  systemDirPosix so the setup wizard can probe a freshly-picked
         *  URI for writability before persisting it. Returns null if the
         *  URI is malformed or its volume ID isn't translatable. */
        fun resolveTreeUriToPosix(uriString: String?): String? {
            val raw = uriString ?: return null
            val uri = try { android.net.Uri.parse(raw) } catch (_: Exception) { return null }
            val docId = try {
                android.provider.DocumentsContract.getTreeDocumentId(uri)
            } catch (_: Exception) { null } ?: return null
            val parts = docId.split(":", limit = 2)
            if (parts.size != 2) return null
            val (volumeId, relPath) = parts
            return when (volumeId) {
                "primary" -> "/storage/emulated/0/$relPath"
                else -> "/storage/$volumeId/$relPath"
            }
        }

        /**
         * Probe the resolved POSIX path for emucore-compatible write
         * access. Creates a `.armsx2-write-probe` file, deletes it,
         * returns true on success.
         *
         * Catches the scoped-storage trap that bit users picking a
         * non-app-private folder without the MANAGE_EXTERNAL_STORAGE
         * grant: Android lets the SAF tree-URI permission survive the
         * picker, so reads work, but raw `fopen`/`mkdir` from emucore
         * fails with EACCES — which then crashes the VM during memcard
         * / savestate / config generation. We probe up-front so the
         * wizard can refuse to advance until either the grant lands or
         * the user picks the app-private fallback.
         */
        fun validateSystemDirWritable(posixPath: String): Boolean {
            return try {
                val dir = File(posixPath)
                if (!dir.exists() && !dir.mkdirs()) return false
                if (!dir.isDirectory) return false
                val probe = File(dir, ".armsx2-write-probe")
                val ok = probe.createNewFile()
                if (ok) probe.delete()
                ok
            } catch (_: Exception) {
                false
            }
        }

        val surface = mutableStateOf<SurfaceView?>(null)

        @JvmField
        val eState = mutableStateOf(EmuState.STOPPED)

        // Active quick save/load slot (0-9), cycled by the "Cycle Save Slot"
        // hotkey. Quick Save/Load State hotkeys read this so users aren't pinned
        // to slot 0.
        val currentSaveSlot = mutableStateOf(0)

        // Cached metadata for the currently-running game. Populated when
        // GamesList taps a card (so we have title, serial, compatibility,
        // extension and the cover URL ready), cleared when the user
        // launches via paths that don't have a GameInfo handy (Swap/Boot Disc
        // file picker, BIOS-only boot). InGameOverlay reads this for its
        // top-left game info block — falls back to NativeApp.getPause* +
        // a runtime compat lookup when it's null.
        val currentGame = mutableStateOf<GameInfo?>(null)

        val focusRequester = FocusRequester()

        private var m_szGamefile = ""
        private val pendingExternalLaunch = mutableStateOf<String?>(null)

        fun onTestResults(result: TestResult) {
            when (result.name) {
                "VuJitTests" -> vuJitTests.value = "${result.passed}/${result.total}"
                "PatchTests" -> patchTests.value = "${result.passed}/${result.total}"
                "CodegenTests" -> codeGenTests.value = "${result.passed}/${result.total}"
                "EeJitTests" -> eeJitTests.value = "${result.passed}/${result.total}"
                "VifTests" -> vifTests.value = "${result.passed}/${result.total}"
                "EeSeqTests" -> eeSeqTests.value = "${result.passed}/${result.total}"
                else -> println("Test:${result.name}: ${result.passed}/${result.total}")
            }
        }

        fun invoke(task: suspend () -> Unit) {
            eScope.launch {
                task()
            }
        }

        private val vmLifecycleLock = Any()
        @Volatile private var vmStopInProgress = false
        @Volatile private var vmRestartAfterStop = false
        @Volatile private var vmRunLoopActive = false

        @JvmStatic
        fun isVmStopInProgress(): Boolean = vmStopInProgress

        fun start() {
            synchronized(vmLifecycleLock) {
                if (vmStopInProgress || vmRunLoopActive || eState.value != EmuState.STOPPED) {
                    vmRestartAfterStop = true
                    return
                }
                vmRunLoopActive = true
            }

            invoke {
                try {
                    eState.value = EmuState.RUNNING
                    println("@@ANDROID_START_VM@@ kind=game path=${m_szGamefile.take(240)}")
                    WindowImpl.showLibrary.value = false
                    WindowImpl.overlayVisible.value = false
                    WindowImpl.toolbarVisible.value = false
                    applyRendererPrefs()
                    NativeApp.runVMThread(m_szGamefile)
                } finally {
                    // runVMThread blocks until the VM exits (Stopping/Shutdown
                    // observed). Drop back to STOPPED only after native has
                    // actually unwound, so users can't launch the next game
                    // while the previous VM is still tearing down.
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmRunLoopActive = false
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    } else {
                        WindowImpl.toolbarVisible.value = true
                        WindowImpl.showLibrary.value = false
                        WindowImpl.overlayVisible.value = false
                    }
                }
            }
        }

        /** Push setup-wizard renderer/upscale choices into emucore's
         *  EmuConfig before runVMThread. ApplySettings inside Initialize
         *  picks them up; if a VM is already up, the JNI helpers also
         *  call MTGS::ApplySettings inline.
         *
         *  Also resolves and applies the per-game / global Settings from
         *  ConfigStore (MTVU and friends) — currentGame.serial picks the
         *  right override tier; null falls back to global. Resolution
         *  order: per-game JSON overlay → global → hardcoded defaults. */
        private fun applyRendererPrefs() {
            NativeApp.renderUpscalemultiplier(upscale.value)
            // Pin custom Vulkan driver (if any) BEFORE the renderer write —
            // the renderer JNI may trigger MTGS::ApplySettings which can
            // re-open the GS device and run Vulkan::LoadVulkanLibrary. The
            // VK loader reads the pinned path lazily so the order matters.
            val ctx = instance?.applicationContext
            val picked: com.armsx2.CustomDriver.InstalledDriver? =
                if (ctx != null) customDriverId.value?.let { id ->
                    com.armsx2.CustomDriver.listInstalled(ctx).firstOrNull { it.id == id }
                } else null
            if (ctx != null) com.armsx2.CustomDriver.applyToNative(ctx, picked)
            when (renderer.value) {
                "vulkan" -> NativeApp.renderVulkan()
                "opengl" -> NativeApp.renderOpenGL()
                "software" -> NativeApp.renderSoftware()
                else -> NativeApp.renderAuto()
            }
            com.armsx2.config.ConfigStore
                .resolveForGame(currentGame.value?.serial)
                .applyTo()

            // Settings.applyTo() above writes the persisted FrameLimitEnable
            // into the BASE settings layer; override it here with the
            // in-session overlay toggle so the user's runtime choice sticks
            // across game launches within one app run. Both writes are
            // needed: native-lib's runVMThread re-reads FrameLimitEnable
            // from the BASE layer right after VMManager::Initialize and
            // calls SetLimiterMode based on that, so a bare
            // speedhackLimitermode() here gets clobbered by VM init.
            // Mode 0 = Nominal (60fps cap), 3 = Unlimited.
            val limit = com.armsx2.ui.InGameOverlay.frameLimitOn.value
            NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", limit.toString())
            NativeApp.speedhackLimitermode(if (limit) 0 else 3)
        }

        private fun readUpscalePref(): Float {
            val all = prefs.all
            fun coerce(raw: Any?): Float? = when (raw) {
                is Float -> raw
                is Double -> raw.toFloat()
                is Int -> raw.toFloat()
                is Long -> raw.toFloat()
                is String -> raw.toFloatOrNull()
                else -> null
            }?.coerceIn(1.0f, 5.0f)

            return coerce(all["upscaleFloat"])
                ?: coerce(all["upscale"])
                ?: 1.0f
        }

        /**
         * Set the active game path/URI and restart the VM. Used by
         * GamesList card taps — the URI comes from the user's persisted
         * ROMs tree (already has read perm) so emucore's FileSystem
         * routines can open it via the content:// JNI bridge.
         *
         * `info` is the GameInfo from the library scan when available;
         * stored on Main.currentGame so the in-game overlay can show
         * cover art / extension badge / pre-resolved compat stars
         * without re-querying gamedb. Pass null when launching from a
         * path that doesn't have a GameInfo (Swap/Boot Disc file picker).
         */
        fun launchGame(uri: String, info: GameInfo? = null) {
            if (uri.isBlank()) {
                println("@@ANDROID_LAUNCH_REJECT@@ reason=blank_uri title=${info?.title ?: ""}")
                return
            }
            println(
                "@@ANDROID_LAUNCH_GAME@@ title=${info?.title ?: "<direct>"} " +
                    "uri=${uri.take(240)} state=${eState.value} runLoop=$vmRunLoopActive " +
                    "stopping=$vmStopInProgress nativeReady=${nativeReady.value}"
            )
            currentGame.value = info
            m_szGamefile = uri
            synchronized(vmLifecycleLock) {
                if (eState.value != EmuState.STOPPED || vmStopInProgress || vmRunLoopActive) {
                    vmRestartAfterStop = true
                }
            }
            if (eState.value == EmuState.STOPPED && !vmStopInProgress && !vmRunLoopActive)
                start()
            else
                stop(restartAfterStop = true)
        }

        private fun launchPendingExternalGameIfReady() {
            val queued = pendingExternalLaunch.value
            if (queued.isNullOrEmpty() || !setupComplete.value || !nativeReady.value)
                return

            pendingExternalLaunch.value = null
            launchGame(queued, null)
        }

        /**
         * Boot to BIOS (no game disc). Unlike `start()` this does NOT
         * hide the toolbar — the BIOS card in GamesList wants the
         * library/toolbar to remain visible so the user can pick a game
         * once BIOS finishes booting.
         */
        fun startBios() {
            currentGame.value = null
            m_szGamefile = ""
            val shouldStart = synchronized(vmLifecycleLock) {
                if (vmStopInProgress || vmRunLoopActive || eState.value != EmuState.STOPPED) {
                    vmRestartAfterStop = true
                    false
                } else {
                    vmRunLoopActive = true
                    true
                }
            }
            if (!shouldStart) {
                stop(restartAfterStop = true)
                return
            }
            invoke {
                try {
                    eState.value = EmuState.RUNNING
                    println("@@ANDROID_START_VM@@ kind=bios path=<empty>")
                    applyRendererPrefs()
                    NativeApp.runVMThread(m_szGamefile)
                } finally {
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmRunLoopActive = false
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    }
                }
            }
        }

        // pause/resume run on a dedicated serialized executor, NOT the UI
        // thread. The native side queues the real pause/resume onto the CPU
        // thread, so a fast open→close still lands in the right order without
        // making the Android UI wait for MTVU/MTGS to park.
        // eState is updated by Host::OnVMPaused/Resumed → vmSetPaused, so the
        // UI never claims PAUSED before the VM actually parked.
        private val vmControl = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
            Thread(r, "VMControl")
        }
        private val vmStopControl = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
            Thread(r, "VMStop")
        }

        fun pause() {
            if (vmStopInProgress)
                return
            vmControl.execute {
                if (!vmStopInProgress)
                    NativeApp.pause()
            }
        }

        fun pauseForOverlay() {
            if (vmStopInProgress)
                return
            NativeApp.pause()
        }

        fun resume() {
            if (vmStopInProgress)
                return
            vmControl.execute {
                if (!vmStopInProgress)
                    NativeApp.resume()
            }
        }

        fun stop(saveAutosave: Boolean = false, restartAfterStop: Boolean = false) {
            val nativeActive = runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
            val shouldStop = synchronized(vmLifecycleLock) {
                if (restartAfterStop)
                    vmRestartAfterStop = true
                else
                    vmRestartAfterStop = false

                if (vmStopInProgress) {
                    nativeActive
                } else if (eState.value == EmuState.STOPPED && !vmRunLoopActive && !nativeActive) {
                    false
                } else {
                    vmStopInProgress = true
                    true
                }
            }
            if (!shouldStop)
                return

            WindowImpl.overlayVisible.value = false
            WindowImpl.showLibrary.value = false
            vmStopControl.execute {
                println("@@ANDROID_STOP_JAVA@@ begin saveAutosave=$saveAutosave restart=$restartAfterStop")
                if (saveAutosave)
                    NativeApp.saveAutosaveState()
                NativeApp.shutdown()
                println("@@ANDROID_STOP_JAVA@@ shutdown_return active=${NativeApp.hasActiveVM()} runLoop=$vmRunLoopActive state=${eState.value}")
                if (!vmRunLoopActive && (eState.value == EmuState.STOPPED || !NativeApp.hasActiveVM())) {
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    } else {
                        synchronized(vmLifecycleLock) {
                            WindowImpl.toolbarVisible.value = true
                            WindowImpl.showLibrary.value = false
                            WindowImpl.overlayVisible.value = false
                        }
                    }
                }
            }
        }

        fun restart() {
            synchronized(vmLifecycleLock) {
                vmRestartAfterStop = true
            }
            if (eState.value == EmuState.STOPPED && !vmStopInProgress && !vmRunLoopActive)
                start()
            else
                stop(restartAfterStop = true)
        }

        fun finishSetup() {
            prefs.edit().putBoolean("setupComplete", true).apply()
            setupComplete.value = true
            setupEditorVisible.value = false
        }

        fun reopenSetup() {
            setupEditorVisible.value = true
        }

        fun renderOpenGL() {
            NativeApp.renderOpenGL()
        }

        fun renderVulkan() {
            NativeApp.renderVulkan()
        }

        fun renderSoftware() {
            NativeApp.renderSoftware()
        }

        /** Resolved root that bundled APK assets (resources/, bios/) are
         *  copied to. If the user picked a non-app-private systemDir, we
         *  copy there so emucore — which reads via POSIX from the same
         *  EmuFolders root — finds them at the expected paths
         *  (e.g. <systemDir>/resources/shaders/opengl/convert.glsl).
         *  Otherwise falls back to getExternalFilesDir(null). Requires
         *  MANAGE_EXTERNAL_STORAGE for the non-app-private case; the
         *  AllFilesAccessScreen gate ensures that's granted before the
         *  emulator UI is reachable. */
        fun assetCopyRoot(context: Context): String {
            return systemDirPosix()
                ?: context.getExternalFilesDir(null)?.absolutePath
                ?: context.dataDir.absolutePath
        }

        fun copyAssetAll(p_context: Context, srcPath: String) {
            copyAssetAll(p_context, srcPath, assetCopyRoot(p_context))
        }

        private fun copyAssetAll(p_context: Context, srcPath: String, rootDir: String) {
            val assetMgr = p_context.assets
            try {
                val destPath = rootDir + File.separator + srcPath
                assetMgr.list(srcPath)?.let {
                    if (it.isEmpty()) {
                        MainActivity.copyFile(p_context, srcPath, destPath)
                    } else {
                        val dir = File(destPath)
                        if (!dir.exists()) dir.mkdirs()
                        for (element in it) {
                            copyAssetAll(p_context, srcPath + File.separator + element, rootDir)
                        }
                    }
                }
            } catch (e: IOException) {
                android.util.Log.e("ARMSX2", "copyAssetAll failed: $srcPath -> $rootDir: ${e.message}")
            }
        }

        private fun sameFilePath(a: File, b: File): Boolean {
            val ca = runCatching { a.canonicalFile }.getOrDefault(a.absoluteFile)
            val cb = runCatching { b.canonicalFile }.getOrDefault(b.absoluteFile)
            return ca == cb
        }

        private fun copyFileViaTemp(src: File, target: File): Boolean {
            if (sameFilePath(src, target))
                return target.exists() && target.length() > 0L
            if (!src.exists() || src.length() <= 0L)
                return false

            val parent = target.parentFile ?: return false
            if (!parent.exists() && !parent.mkdirs())
                return false

            val tmp = File(parent, ".${target.name}.migrate.tmp")
            if (tmp.exists())
                tmp.delete()

            return runCatching {
                src.copyTo(tmp, overwrite = true)
                if (tmp.length() <= 0L)
                    return@runCatching false
                if (target.exists() && !target.delete())
                    return@runCatching false
                val installed = tmp.renameTo(target) || runCatching {
                    tmp.copyTo(target, overwrite = true)
                    true
                }.getOrDefault(false)
                installed && target.exists() && target.length() > 0L
            }.getOrDefault(false).also {
                tmp.delete()
            }
        }

        fun getSupportedGLESVersion(context: Context): Double {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val info = am.deviceConfigurationInfo
            return info.glEsVersion.toDouble()
        }

        fun isAndroidEmulator(): Boolean {
            return Build.MODEL.startsWith("sdk_")
        }
    }

    val swapDiscAction = registerForActivityResult(
        StartActivityForResult()
    ) { result: ActivityResult ->
        if (result.resultCode == RESULT_OK) {
            try {
                val intent = result.data
                val uri = intent?.dataString ?: ""
                if (uri.isNotEmpty()) {
                    // Swap the mounted disc instead of rebooting. The old path
                    // (restart()) booted the picked disc as a fresh VM, which
                    // dropped CodeBreaker/multi-disc hand-offs and never showed
                    // a "disc changed" notification. NativeApp.changeDisc keeps
                    // the running VM, cycles the tray so the game detects the
                    // new disc, and emits the on-screen "Disc changed to …" OSD.
                    // Runs off-thread since it parks the CPU thread and blocks.
                    println("@@ANDROID_SWAP_DISC@@ uri=${uri.take(240)}")
                    kotlin.concurrent.thread {
                        val ok = runCatching { NativeApp.changeDisc(uri) }.getOrDefault(false)
                        instance?.runOnUiThread {
                            if (ok) {
                                // changeDisc parks the VM to swap on the CPU
                                // thread; unpause so the game runs and detects
                                // the new disc (otherwise the screen sits frozen
                                // on the paused frame).
                                resume()
                            } else {
                                // Swap Disc is swap-only. If native rejected
                                // the image it already restored the old disc,
                                // so just resume the existing session.
                                resume()
                            }
                        }
                    }
                }
            } catch (_: Exception) { }
        }
    }

    val bootDiscAction = registerForActivityResult(
        StartActivityForResult()
    ) { result: ActivityResult ->
        if (result.resultCode == RESULT_OK) {
            try {
                val uri = result.data?.dataString ?: ""
                if (uri.isNotEmpty()) {
                    println("@@ANDROID_BOOT_DISC@@ uri=${uri.take(240)}")
                    launchGame(uri, null)
                }
            } catch (_: Exception) { }
        }
    }

    init {
        instance = this
    }

    /** Latched on first kickoffEmucoreInit so a second call (e.g. after
     *  the user re-enters setup via the cog) is a no-op. Heavy init —
     *  asset copy, EmuFolders setup, JIT test prelude — must run once
     *  per process. */
    private var emucoreInitDone = false

    /** Latch for the debug-build auto-boot-to-BIOS path. Fires once per
     *  process from kickoffEmucoreInit's tail so JIT tests finish first,
     *  then runs startBios() with no game disc. Used for perfape baseline
     *  captures without manually tapping the BIOS card. */
    private var autoBootBiosFired = false

    /** Build-config flag for the auto-boot-to-BIOS path above. Flip to
     *  true (here, or move to BuildConfig via app/build.gradle.kts if a
     *  variant-level toggle is wanted) to drop straight into the BIOS
     *  shell on app launch — useful for perfape captures. */
    private val AUTO_BOOT_BIOS = false

    /**
     * Run the heavy one-shot emucore init (asset copy + EmuFolders +
     * SDL/HID setup + EE/VIF JIT-test prelude). MUST be called only
     * AFTER the user has finished the setup wizard so `Main.systemDir`
     * resolves to the chosen path before `NativeApp.initializeOnce`
     * locks `EmuFolders::AppRoot` in for the rest of the process.
     *
     * Idempotent — guarded by emucoreInitDone. Safe to call from both
     * onCreate (returning user, setupComplete already true) and the
     * setContent LaunchedEffect (first-time user, setupComplete just
     * flipped).
     */
    private fun kickoffEmucoreInit() {
        if (emucoreInitDone) return
        emucoreInitDone = true

        // Default resources — shaders, GameIndex, fonts, fullscreenui,
        // patches.zip, controller DB. assetCopyRoot resolves to the
        // user's chosen systemDir (now valid post-setup) so emucore
        // finds them at <systemDir>/resources/...
        copyAssetAll(applicationContext, "bios")
        copyAssetAll(applicationContext, "resources")

        // Move the configured BIOS under the active data root so it tracks a
        // custom system folder instead of staying app-private (older builds
        // pinned BIOS to externalFilesDir/bios). No-op when no BIOS is set or
        // it's already there; on copy failure we leave the pref untouched, and
        // biosFolderPosix still points emucore at the old (working) location.
        bios.value?.takeIf { it.isNotEmpty() }?.let { current ->
            val src = File(current)
            val target = File(File(assetCopyRoot(applicationContext), "bios").apply { mkdirs() }, src.name)
            if (!sameFilePath(target, src)) {
                val present = (target.exists() && target.length() > 0L) ||
                    copyFileViaTemp(src, target)
                if (present) {
                    bios.value = target.absolutePath
                    prefs.edit().putString("bios", target.absolutePath).apply()
                }
            } else if (target.exists() && target.length() > 0L) {
                bios.value = target.absolutePath
                prefs.edit().putString("bios", target.absolutePath).apply()
            }
        }

        invoke {
            NativeApp.initializeOnce(applicationContext)
            nativeReady.value = true

            // Pin Filenames/BIOS to the file the setup wizard copied —
            // deferred to here because Host::SetBaseStringSettingValue
            // null-derefs when called before initializeOnce installs the
            // base settings layer. finishBiosStep (in SetupImpl) only
            // persists to Main.bios + Java prefs; the actual setSetting
            // happens here, AFTER the layer is installed AND
            // SetDefaultSettings has run (so default-empty doesn't
            // overwrite our pin).
            //
            // Without this pin emucore's LoadBIOS falls back to
            // FindBiosImage()'s alphabetical scan, ignoring the wizard's
            // selection — see armsx2_bios_filename_pin memo.
            bios.value?.let { biosPath ->
                val name = File(biosPath).name
                if (name.isNotEmpty()) {
                    NativeApp.setSetting("Filenames", "BIOS", "string", name)
                    NativeApp.commitSettings()
                }
            }

            // Set up JNI
            SDLControllerManager.nativeSetupJNI()
            SDLControllerManager.initialize()
            HIDDeviceManager(applicationContext)

            println("PCSX2_INIT")

            // Tests that need VTLB/eeMem — run after init
            NativeApp.runEeJitTests()
            NativeApp.runEeSeqTests()
            NativeApp.runVifTests()

            // Debug-build auto-boot to BIOS. Lets us drop straight into the
            // BIOS shell on app launch for perfape baseline captures —
            // skips tapping through GamesList. One-shot via latch so
            // re-entering Setup and back doesn't relaunch. Currently
            // gated off via AUTO_BOOT_BIOS — flip to true to re-enable.
            @Suppress("KotlinConstantConditions")
            if (AUTO_BOOT_BIOS && BuildConfig.DEBUG && !autoBootBiosFired &&
                eState.value == EmuState.STOPPED) {
                autoBootBiosFired = true
                startBios()
            }
        }
    }

    fun sendKeyAction(p_action: KeyEventType, p_keycode: Int) {
        // Any physical gamepad key event implies the user is on a
        // controller — latch the on-screen touch controls hidden until a
        // screen press flips them back on. Idempotent.
        com.armsx2.ui.touch.TouchControls.onControllerInputDetected()
        if (p_action == KeyEventType.KeyDown) {
            var pad_force = 0
            if (p_keycode >= 110) {
                var _abs = 90f // Joystic test value
                _abs = min(_abs, 100f)
                pad_force = (_abs * 32766.0f / 100).toInt()
            }
            NativeApp.setPadButton(p_keycode, pad_force, true)
        } else if (p_action == KeyEventType.KeyUp || p_action == KeyEventType.Unknown) {
            NativeApp.setPadButton(p_keycode, 0, false)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Swallow back presses unconditionally. Compose BackHandlers (the
        // in-game overlay's submenu drill-down, the library's eventual
        // back-to-game escape, etc.) register at higher priority and
        // consume the event when they're appropriate; this low-priority
        // no-op catches anything they don't, so the system never falls
        // through to finish() on the activity. Same callback also stops
        // controller "B"/"Circle" buttons that the OS maps to KEYCODE_BACK
        // (Xbox/DualShock default) from killing the app.
        onBackPressedDispatcher.addCallback(this) {
            // intentionally empty — pure stay-alive sentinel
        }
        prefs = applicationContext.getSharedPreferences("ARMSX2", MODE_PRIVATE)
        com.armsx2.CoverArtStyle.load()
        setupComplete.value = prefs.getBoolean("setupComplete", false)
        systemDir.value = prefs.getString("systemDir", null)
        bios.value = prefs.getString("bios", null)
        biosDir.value = prefs.getString("biosDir", null)
        // Load roms folders. New format: JSON array under "romsDirs" pref.
        // Legacy format: single string under "roms" pref (pre-multi-dir).
        // Migration path: read legacy if present, hoist into the list, keep
        // both keys in sync until the user re-confirms in setup. Once the
        // user adds/removes via the new picker the legacy key is dropped.
        romsDirs.value = run {
            val newJson = prefs.getString("romsDirs", null)
            if (newJson != null) {
                runCatching {
                    val arr = org.json.JSONArray(newJson)
                    List(arr.length()) { arr.getString(it) }
                }.getOrDefault(emptyList())
            } else {
                val legacy = prefs.getString("roms", null)
                if (legacy != null) listOf(legacy) else emptyList()
            }
        }
        renderer.value = prefs.getString("renderer", "auto") ?: "auto"
        upscale.value = readUpscalePref()
        customDriverId.value = prefs.getString("customDriverId", null)?.takeIf { it.isNotEmpty() }
        allFilesAccessGranted.value = !needsAllFilesAccess()
        surface.value = SurfaceCallbacks(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        // Defer asset copy + emucore init until setup is complete. On the
        // first-ever run, `systemDir` isn't picked yet at onCreate time —
        // so initializeOnce would resolve to the app-private fallback and
        // wedge `EmuFolders::AppRoot` there for the rest of the process,
        // even after the user finishes the wizard. Memcards then read
        // from the wrong dir on first boot ("scary empty cards"); next
        // app launch picks up the correct path from prefs and saves
        // re-appear. Gating on setupComplete avoids the misroute.
        //
        // Idempotent guard: kickoffEmucoreInit checks emucoreInitDone so
        // setupComplete flipping multiple times (re-entry via cog button
        // doesn't toggle it back to false, but be defensive) only fires
        // the heavy init once.
        if (setupComplete.value) {
            kickoffEmucoreInit()
        }
        // else: setContent's LaunchedEffect(setupComplete.value) below
        // calls kickoffEmucoreInit when the wizard finishes.

        val glVersion = getSupportedGLESVersion(this)

        if (glVersion < 3.1) {
            eState.value = EmuState.RENDER_UNSUPPORTED
            println("RENDER_UNSUPPORTED")
        }

        if (isAndroidEmulator()) {
            eState.value = EmuState.EMULATOR_UNSUPPORTED
            println("DEVICE_UNSUPPORTED")
        }
        handleExternalLaunchIntent(intent)
        setContent {
            val ctx = androidx.compose.ui.platform.LocalContext.current

            // First-time setup deferral: when the wizard finishes and
            // setupComplete flips to true, kick off the heavy emucore
            // init now that `Main.systemDir` reflects the user's pick.
            // The kickoff helper is idempotent (emucoreInitDone latch),
            // so this firing AFTER an onCreate-time call (returning user
            // with setupComplete already true) is a no-op.
            androidx.compose.runtime.LaunchedEffect(setupComplete.value) {
                if (setupComplete.value) {
                    kickoffEmucoreInit()
                }
            }

            androidx.compose.runtime.LaunchedEffect(
                setupComplete.value,
                nativeReady.value,
                pendingExternalLaunch.value,
            ) {
                launchPendingExternalGameIfReady()
            }

            // Setup wizard runs once. After it persists prefs and flips
            // setupComplete the main emulator UI takes over. Re-entering
            // setup requires clearing app data (or wiping the prefs key).
            //
            // Permission gate: if the user picked a system folder outside
            // the app-private sandbox (e.g. /storage/emulated/0/ARMSX2/),
            // emucore's POSIX writes need MANAGE_EXTERNAL_STORAGE. Block
            // the main UI behind a grant-request screen until the user
            // toggles it on in Settings — onResume will flip the state
            // and let the main UI through automatically.
            val needsGate = setupComplete.value &&
                !allFilesAccessGranted.value &&
                !systemDirIsAppPrivate(ctx)
            if (!setupComplete.value || setupEditorVisible.value) {
                SetupImpl.SetupWindow()
            } else if (needsGate) {
                AllFilesAccessScreen(
                    onGrant = { requestAllFilesAccess(ctx) },
                    onUseAppPrivate = {
                        // Drop the user's chosen systemDir and fall back to
                        // app-private. emucore reverts to writing under
                        // getExternalFilesDir on next VM boot.
                        systemDir.value = null
                        prefs.edit().remove("systemDir").apply()
                    },
                )
            } else if (setupComplete.value) {
                WindowImpl.Window {
                    if (surface.value != null) {
                        // Pull Compose focus onto the surface as soon as it's
                        // composed. Without this the AndroidView starts
                        // un-focused, so onKeyEvent silently drops gamepad
                        // input until the user taps the screen / presses A
                        // to grant focus by hand. Also re-runs whenever
                        // surface.value changes (e.g. after orientation
                        // change rebuilds the SurfaceView).
                        androidx.compose.runtime.LaunchedEffect(surface.value) {
                            focusRequester.requestFocus()
                        }
                        // Controller menu nav: the embedded game SurfaceView holds
                        // Android-level focus, and while an embedded View has focus
                        // the D-pad bypasses Compose's focus system entirely (so the
                        // pause overlay can never receive it). When the overlay opens,
                        // drop the SurfaceView's View-level focusability + clear its
                        // focus so Android focus moves into the Compose tree and the
                        // overlay's requestFocus can take it. Restore it (and re-grab
                        // game input) when the overlay closes.
                        androidx.compose.runtime.LaunchedEffect(WindowImpl.overlayVisible.value) {
                            val sv = surface.value
                            if (WindowImpl.overlayVisible.value) {
                                sv?.isFocusableInTouchMode = false
                                sv?.isFocusable = false
                                sv?.clearFocus()
                            } else {
                                sv?.isFocusable = true
                                sv?.isFocusableInTouchMode = true
                                if (eState.value == EmuState.RUNNING)
                                    runCatching { focusRequester.requestFocus() }
                            }
                        }
                        AndroidView(factory = { surface.value!! }, modifier = Modifier
                            // Drop the surface from the focus system while the
                            // pause overlay is open so it can't hold/steal focus
                            // away from the overlay's controller navigation.
                            .focusable(!WindowImpl.overlayVisible.value)
                            .focusRequester(focusRequester)
                            .fillMaxSize()
                            .pointerInput(Unit) {
                                // In-game pausing moved OFF the surface-wide
                                // long-press: it fired on accidental presses in
                                // empty screen space. The pause overlay now opens
                                // via long-press on the invisible PAUSE hotspot
                                // widget between the DPad and face buttons (see
                                // TouchControlsOverlay.PauseWidget). Long-press
                                // here only toggles the toolbar when no game is
                                // up (games-list screen).
                                //
                                // onPress fires on every initial pointer down on
                                // the surface (events that don't land on a touch
                                // button — the buttons consume their own touches).
                                // Any such tap means the user is using the screen,
                                // so unlatch any controller-mode hide so the touch
                                // controls reappear. onPress doesn't consume the
                                // gesture; long-press detection continues to run.
                                detectTapGestures(
                                    onPress = {
                                        com.armsx2.ui.touch.TouchControls.onSurfaceTouched()
                                    },
                                    onLongPress = {
                                        if (eState.value != EmuState.RUNNING &&
                                            eState.value != EmuState.PAUSED) {
                                            WindowImpl.toolbarVisible.value = !WindowImpl.toolbarVisible.value
                                        }
                                    },
                                )
                            }
                            .onKeyEvent { event ->
                                if (eState.value != EmuState.RUNNING)
                                    return@onKeyEvent false
                                // Note: the physical menu button is handled in
                                // Main.dispatchKeyEvent (so it can catch BACK /
                                // back-paddle keys); it never reaches here.
                                val target = ControllerMappings.targetForPhysical(event.key.nativeKeyCode)
                                    ?: return@onKeyEvent false
                                sendKeyAction(event.type, target)
                                true
                            })
                    }

                    if (eState.value == EmuState.STOPPED || eState.value == EmuState.RENDER_UNSUPPORTED || eState.value == EmuState.EMULATOR_UNSUPPORTED) {
                        Box(Modifier
                            .fillMaxSize()
                            .background(Colors.surface.value)) {
                            if (eState.value == EmuState.EMULATOR_UNSUPPORTED) {
                                Box(Modifier.align(Alignment.Center)) {
                                    Column {
                                        Image(LineAwesomeIcons.Android, "",
                                            colorFilter = ColorFilter.tint(Colors.pasx2_blue),
                                            modifier = Modifier
                                                .size(150.dp)
                                                .align(Alignment.CenterHorizontally)
                                        )
                                        Text(
                                            "Android Emulator is not supported", fontSize = 22.sp, color = Colors.pasx2_blue,
                                            modifier = Modifier.align(Alignment.CenterHorizontally)
                                        )
                                        Text(
                                            "Please use a physical device", fontSize = 22.sp, color = Colors.pasx2_blue,
                                            modifier = Modifier.align(Alignment.CenterHorizontally)
                                        )
                                    }
                                }
                            } else {
                                // Games list — replaces the old runtime-test panel.
                                // The tests still run automatically on first composition
                                // (above); their results are now available via the bug
                                // toolbar button instead of taking up the main screen.
                                com.armsx2.ui.GamesList.GamesRow()
                            }
                        }
                    }
                }
            }
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val kc = event.keyCode
        // System-hotkey capture (from PadTab). Handled here, not in Compose, so it
        // can capture KEYCODE_BACK and back-paddle keys (the back dispatcher
        // swallows those before they'd reach a focusable's onPreviewKeyEvent).
        val capturing = ControllerMappings.captureHotkey.value
        if (capturing != null) {
            if (event.action == KeyEvent.ACTION_DOWN && kc != KeyEvent.KEYCODE_UNKNOWN) {
                ControllerMappings.bindHotkey(capturing, kc)
                ControllerMappings.captureHotkey.value = null
                ControllerMappings.hotkeyBindTick.value++
            }
            return true // swallow down + up while capturing
        }
        // Memory-card dialog (opened from the library). Touch mode blocks Compose
        // D-pad focus, so it's driven by the manual nav model (same as the
        // settings tabs). Any direction steps the control list; A activates; B closes.
        if (com.armsx2.ui.MemoryCardManager.visible.value) {
            val nav = com.armsx2.ui.settings.SettingsControllerNav
            when (kc) {
                KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK -> {
                    if (event.action == KeyEvent.ACTION_DOWN)
                        com.armsx2.ui.MemoryCardManager.visible.value = false
                    return true
                }
                KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER,
                KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> {
                    if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                        nav.confirm()
                    return true
                }
                KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_LEFT -> {
                    if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                        nav.move(-1)
                    return true
                }
                KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                    if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                        nav.move(1)
                    return true
                }
                else -> return super.dispatchKeyEvent(event)
            }
        }
        if (WindowImpl.overlayVisible.value) {
            val handled = when (kc) {
                KeyEvent.KEYCODE_DPAD_LEFT -> when (event.action) {
                    KeyEvent.ACTION_UP -> {
                        InGameOverlay.handleControllerHorizontalRelease()
                        true
                    }
                    KeyEvent.ACTION_DOWN -> {
                        if (event.repeatCount != 0) {
                            true
                        } else {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                if (!InGameOverlay.handleControllerMove(-1, 0))
                                    dispatchSyntheticUiKey(kc)
                            }
                            true
                        }
                    }
                    else -> true
                }
                KeyEvent.KEYCODE_DPAD_RIGHT -> when (event.action) {
                    KeyEvent.ACTION_UP -> {
                        InGameOverlay.handleControllerHorizontalRelease()
                        true
                    }
                    KeyEvent.ACTION_DOWN -> {
                        if (event.repeatCount != 0) {
                            true
                        } else {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                if (!InGameOverlay.handleControllerMove(1, 0))
                                    dispatchSyntheticUiKey(kc)
                            }
                            true
                        }
                    }
                    else -> true
                }
                KeyEvent.KEYCODE_DPAD_UP -> event.action != KeyEvent.ACTION_DOWN || run {
                    if (event.repeatCount != 0) return@run true
                    val now = SystemClock.uptimeMillis()
                    if (shouldSuppressUiNav(kc, fromAxis = false, now)) return@run true
                    recordUiNav(kc, fromAxis = false)
                    if (!InGameOverlay.handleControllerMove(0, -1)) dispatchSyntheticUiKey(kc)
                    true
                }
                KeyEvent.KEYCODE_DPAD_DOWN -> event.action != KeyEvent.ACTION_DOWN || run {
                    if (event.repeatCount != 0) return@run true
                    val now = SystemClock.uptimeMillis()
                    if (shouldSuppressUiNav(kc, fromAxis = false, now)) return@run true
                    recordUiNav(kc, fromAxis = false)
                    if (!InGameOverlay.handleControllerMove(0, 1)) dispatchSyntheticUiKey(kc)
                    true
                }
                KeyEvent.KEYCODE_BUTTON_L1 -> event.action != KeyEvent.ACTION_DOWN ||
                    InGameOverlay.handleControllerTab(-1)
                KeyEvent.KEYCODE_BUTTON_R1 -> event.action != KeyEvent.ACTION_DOWN ||
                    InGameOverlay.handleControllerTab(1)
                KeyEvent.KEYCODE_BUTTON_Y -> event.action != KeyEvent.ACTION_DOWN || run {
                    InGameOverlay.openAchievements()
                    true
                }
                KeyEvent.KEYCODE_BUTTON_A,
                KeyEvent.KEYCODE_DPAD_CENTER,
                KeyEvent.KEYCODE_ENTER,
                KeyEvent.KEYCODE_NUMPAD_ENTER -> event.action != KeyEvent.ACTION_DOWN || run {
                    if (!InGameOverlay.handleControllerConfirm())
                        dispatchSyntheticUiKey(KeyEvent.KEYCODE_DPAD_CENTER)
                    true
                }
                KeyEvent.KEYCODE_BUTTON_B,
                KeyEvent.KEYCODE_BACK -> event.action != KeyEvent.ACTION_DOWN ||
                    InGameOverlay.handleControllerBack()
                else -> false
            }
            if (handled) return true
        }
        if (controllerDrivesFrontend()) {
            if (!WindowImpl.overlayVisible.value && GamesList.controllerActive()) {
                // Square button (or the Menu hotkey) opens settings for the
                // highlighted cover — the controller equivalent of long-press.
                if (ControllerMappings.hotkeyFor(kc) == ControllerMappings.SysHotkey.MENU ||
                    kc == KeyEvent.KEYCODE_BUTTON_X
                ) {
                    if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                        GamesList.openSelectedGameSettings()
                    return true
                }
                val handled = when (kc) {
                    KeyEvent.KEYCODE_DPAD_LEFT -> event.action != KeyEvent.ACTION_DOWN || run {
                        if (event.repeatCount == 0) {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                GamesList.handleControllerMove(-1, 0)
                            }
                        }
                        true
                    }
                    KeyEvent.KEYCODE_DPAD_RIGHT -> event.action != KeyEvent.ACTION_DOWN || run {
                        if (event.repeatCount == 0) {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                GamesList.handleControllerMove(1, 0)
                            }
                        }
                        true
                    }
                    KeyEvent.KEYCODE_DPAD_UP -> event.action != KeyEvent.ACTION_DOWN || run {
                        if (event.repeatCount == 0) {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                GamesList.handleControllerMove(0, -1)
                            }
                        }
                        true
                    }
                    KeyEvent.KEYCODE_DPAD_DOWN -> event.action != KeyEvent.ACTION_DOWN || run {
                        if (event.repeatCount == 0) {
                            val now = SystemClock.uptimeMillis()
                            if (!shouldSuppressUiNav(kc, fromAxis = false, now)) {
                                recordUiNav(kc, fromAxis = false)
                                GamesList.handleControllerMove(0, 1)
                            }
                        }
                        true
                    }
                    KeyEvent.KEYCODE_BUTTON_A,
                    KeyEvent.KEYCODE_DPAD_CENTER,
                    KeyEvent.KEYCODE_ENTER,
                    KeyEvent.KEYCODE_NUMPAD_ENTER -> event.action != KeyEvent.ACTION_DOWN ||
                        GamesList.handleControllerConfirm()
                    KeyEvent.KEYCODE_BUTTON_B,
                    KeyEvent.KEYCODE_BACK -> event.action != KeyEvent.ACTION_DOWN ||
                        GamesList.handleControllerBack()
                    else -> false
                }
                if (handled) return true
            }
            if (kc == KeyEvent.KEYCODE_BACK && WindowImpl.showLibrary.value) {
                if (event.action == KeyEvent.ACTION_DOWN) WindowImpl.showLibrary.value = false
                return true
            }
            if (kc == KeyEvent.KEYCODE_DPAD_LEFT ||
                kc == KeyEvent.KEYCODE_DPAD_RIGHT ||
                kc == KeyEvent.KEYCODE_DPAD_UP ||
                kc == KeyEvent.KEYCODE_DPAD_DOWN
            ) {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    dispatchSyntheticUiKey(kc)
                }
                return true
            }
            if (kc == KeyEvent.KEYCODE_BUTTON_A) {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    dispatchSyntheticUiKey(KeyEvent.KEYCODE_DPAD_CENTER)
                }
                return true
            }
            if (kc == KeyEvent.KEYCODE_BUTTON_B) {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    if (WindowImpl.showLibrary.value) {
                        WindowImpl.showLibrary.value = false
                    } else {
                        dispatchSyntheticUiKey(KeyEvent.KEYCODE_BACK)
                    }
                }
                return true
            }
        }
        // Runtime: bound system hotkeys. Caught here so back-button bindings work
        // (and aren't eaten by the back handler).
        if (eState.value == EmuState.RUNNING && !controllerDrivesFrontend()) {
            val down = event.action == KeyEvent.ACTION_DOWN
            when (ControllerMappings.hotkeyFor(kc)) {
                ControllerMappings.SysHotkey.MENU -> {
                    if (down) com.armsx2.ui.InGameOverlay.toggle()
                    return true
                }
                ControllerMappings.SysHotkey.SAVE_STATE -> {
                    if (down) {
                        val slot = Main.currentSaveSlot.value
                        kotlin.concurrent.thread { runCatching { NativeApp.saveStateToSlot(slot) } }
                    }
                    return true
                }
                ControllerMappings.SysHotkey.LOAD_STATE -> {
                    if (down) {
                        val slot = Main.currentSaveSlot.value
                        kotlin.concurrent.thread { runCatching { NativeApp.loadStateFromSlot(slot) } }
                    }
                    return true
                }
                ControllerMappings.SysHotkey.CYCLE_SLOT -> {
                    if (down) cycleSaveSlot()
                    return true
                }
                ControllerMappings.SysHotkey.TEXTURE_DUMP -> {
                    if (down) {
                        val on = runCatching { NativeApp.toggleTextureDumping() }.getOrDefault(false)
                        android.widget.Toast.makeText(
                            this,
                            if (on) "Texture dumping ON" else "Texture dumping OFF",
                            android.widget.Toast.LENGTH_SHORT,
                        ).show()
                    }
                    return true
                }
                ControllerMappings.SysHotkey.FAST_FORWARD -> {
                    // Hold to fast-forward (Turbo), release to return to normal.
                    if (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP) {
                        if (event.repeatCount == 0)
                            runCatching { NativeApp.speedhackLimitermode(if (down) 1 else 0) }
                    }
                    return true
                }
                ControllerMappings.SysHotkey.RES_UP -> {
                    if (down) stepResolution(1)
                    return true
                }
                ControllerMappings.SysHotkey.RES_DOWN -> {
                    if (down) stepResolution(-1)
                    return true
                }
                ControllerMappings.SysHotkey.ACHIEVEMENTS -> {
                    if (down) com.armsx2.ui.InGameOverlay.openAchievements()
                    return true
                }
                ControllerMappings.SysHotkey.CLOSE_GAME -> {
                    if (down) Main.stop()
                    return true
                }
                null -> {}
            }
        }
        return super.dispatchKeyEvent(event)
    }

    /** Cycle the active quick save/load slot 0→9→0 with a brief on-screen note. */
    private fun cycleSaveSlot() {
        val next = (Main.currentSaveSlot.value + 1) % 10
        Main.currentSaveSlot.value = next
        android.widget.Toast.makeText(this, "Save slot $next", android.widget.Toast.LENGTH_SHORT).show()
    }

    /** Step the internal resolution multiplier up/down (1x..5x), apply live. */
    private fun stepResolution(dir: Int) {
        val next = (Main.upscale.value.toInt() + dir).coerceIn(1, 5)
        val nf = next.toFloat()
        Main.upscale.value = nf
        runCatching { NativeApp.renderUpscalemultiplier(nf) }
        runCatching { Main.prefs.edit().putFloat("upscaleFloat", nf).apply() }
        android.widget.Toast.makeText(this, "Resolution ${next}x", android.widget.Toast.LENGTH_SHORT).show()
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if (com.armsx2.ui.MemoryCardManager.visible.value) {
            handleMemcardControllerMotion(ev)
            return true
        }
        if (controllerDrivesFrontend() && handleControllerUiMotion(ev)) {
            return true
        }
        if (eState.value == EmuState.RUNNING) {
            // SOURCE_TOUCHSCREEN motion events go through dispatchTouchEvent,
            // not here — generic motion is gamepad / mouse / stylus. So any
            // event reaching this method means a controller (or similar
            // pointing device) is being used; latch touch controls off.
            com.armsx2.ui.touch.TouchControls.onControllerInputDetected()
            sendAxis(ev, MotionEvent.AXIS_X,     posCode = 111, negCode = 113) // L right / left
            sendAxis(ev, MotionEvent.AXIS_Y,     posCode = 112, negCode = 110) // L down  / up
            sendAxis(ev, MotionEvent.AXIS_Z,     posCode = 121, negCode = 123) // R right / left
            sendAxis(ev, MotionEvent.AXIS_RZ,    posCode = 122, negCode = 120) // R down  / up
            sendAxis(ev, MotionEvent.AXIS_HAT_X, posCode = 22,  negCode = 21)  // D right / left
            sendAxis(ev, MotionEvent.AXIS_HAT_Y, posCode = 20,  negCode = 19)  // D down  / up
            // Analog triggers (L2/R2). Xbox / DualShock / most modern pads
            // report these as 0..1 motion-axis values, not Key.ButtonL2/R2
            // key events, so the onKeyEvent path above never sees them.
            // AXIS_LTRIGGER/RTRIGGER is the modern path; some controllers
            // (older Moga, certain BT mappings) report via AXIS_BRAKE/GAS
            // instead — take the max so we handle whichever the device
            // actually emits without double-driving when both are present.
            sendTrigger(ev, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE,
                KeyEvent.KEYCODE_BUTTON_L2)
            sendTrigger(ev, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS,
                KeyEvent.KEYCODE_BUTTON_R2)
            return true
        }
        return super.dispatchGenericMotionEvent(ev)
    }

    private fun controllerDrivesFrontend(): Boolean =
        WindowImpl.overlayVisible.value ||
            GamesList.controllerActive()

    // --- Controller menu nav hold-to-repeat ---------------------------------
    // The per-frame stick handlers below are edge-triggered (one move per push),
    // which makes holding a direction feel dead/clunky. While a direction is
    // held we run a repeat loop so the selection keeps travelling, matching
    // normal D-pad-menu behaviour.
    private var navRepeatJob: kotlinx.coroutines.Job? = null
    private var navRepeatDx = 0
    private var navRepeatDy = 0

    private fun directionKeyCode(dx: Int, dy: Int): Int = when {
        dx < 0 -> KeyEvent.KEYCODE_DPAD_LEFT
        dx > 0 -> KeyEvent.KEYCODE_DPAD_RIGHT
        dy < 0 -> KeyEvent.KEYCODE_DPAD_UP
        dy > 0 -> KeyEvent.KEYCODE_DPAD_DOWN
        else -> 0
    }

    private fun fireNavMove(dx: Int, dy: Int) {
        if (WindowImpl.overlayVisible.value) {
            // Fall back to synthetic D-pad (Compose focus) for overlay screens
            // the manual model doesn't handle (e.g. the save-state slot grid).
            if (!InGameOverlay.handleControllerMove(dx, dy)) {
                val kc = directionKeyCode(dx, dy)
                if (kc != 0) dispatchSyntheticUiKey(kc)
            }
        } else if (GamesList.controllerActive()) {
            GamesList.handleControllerMove(dx, dy)
        } else {
            // Menu closed while a direction was held — stop repeating.
            stopNavRepeat()
        }
    }

    private fun startNavRepeat(dx: Int, dy: Int) {
        if (dx == 0 && dy == 0) {
            stopNavRepeat()
            return
        }
        if (navRepeatJob?.isActive == true && navRepeatDx == dx && navRepeatDy == dy) return
        stopNavRepeat()
        navRepeatDx = dx
        navRepeatDy = dy
        fireNavMove(dx, dy)
        navRepeatJob = lifecycleScope.launch {
            kotlinx.coroutines.delay(NAV_REPEAT_INITIAL_MS)
            while (true) {
                fireNavMove(navRepeatDx, navRepeatDy)
                kotlinx.coroutines.delay(NAV_REPEAT_INTERVAL_MS)
            }
        }
    }

    private fun stopNavRepeat() {
        navRepeatJob?.cancel()
        navRepeatJob = null
        navRepeatDx = 0
        navRepeatDy = 0
    }

    private fun handleControllerUiMotion(ev: MotionEvent): Boolean {
        if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
            !ev.isFromSource(InputDevice.SOURCE_GAMEPAD)
        ) {
            return false
        }

        com.armsx2.ui.touch.TouchControls.onControllerInputDetected()
        return if (WindowImpl.overlayVisible.value) {
            handleOverlayControllerMotion(ev)
        } else {
            handleLibraryControllerMotion(ev)
        }
    }

    private fun handleLibraryControllerMotion(ev: MotionEvent): Boolean {
        val scrollY = uiScrollValue(ev.getAxisValue(MotionEvent.AXIS_RZ))
        handleControllerUiScroll(scrollY)

        // Accept BOTH the left stick and the D-pad (HAT axis on this hardware) so
        // handhelds with or without a stick can browse the library.
        val (stickDx, stickDy) = uiDominantStickDirection(
            ev.getAxisValue(MotionEvent.AXIS_X),
            ev.getAxisValue(MotionEvent.AXIS_Y),
        )
        val dx = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
            .let { if (it != 0) it else stickDx }
        val dy = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
            .let { if (it != 0) it else stickDy }
        if (dx == 0 && dy == 0) {
            if (libraryAxisX != 0 || libraryAxisY != 0) stopNavRepeat()
            libraryAxisX = 0
            libraryAxisY = 0
            return true
        }

        if (dx != libraryAxisX || dy != libraryAxisY) {
            libraryAxisX = dx
            libraryAxisY = dy
            startNavRepeat(dx, dy)
        }
        return true
    }

    private fun handleOverlayControllerMotion(ev: MotionEvent): Boolean {
        // The overlay accepts BOTH the D-pad and the left analog stick, so
        // handhelds with or without a stick work. On this hardware the D-pad is a
        // HAT axis (not KEYCODE_DPAD_*); the stick is AXIS_X/Y. The adjust
        // skip/stuck bug was in the settings registry (now fixed), not the input
        // layer, so the stick is safe to use again. Right stick scrolls lists.
        handleControllerUiScroll(uiScrollValue(ev.getAxisValue(MotionEvent.AXIS_RZ)))

        val (stickDx, stickDy) = uiDominantStickDirection(
            ev.getAxisValue(MotionEvent.AXIS_X),
            ev.getAxisValue(MotionEvent.AXIS_Y),
        )
        val dirX = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
            .let { if (it != 0) it else stickDx }
        val dirY = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
            .let { if (it != 0) it else stickDy }

        // Up/down: move between settings, with hold-to-repeat for long lists.
        if (dirY == 0) {
            if (overlayAxisY != 0) stopNavRepeat()
            overlayAxisY = 0
        } else if (dirY != overlayAxisY) {
            overlayAxisY = dirY
            startNavRepeat(0, dirY)
        }

        // Left/right: adjust the focused setting, one step per press (edge-
        // triggered; re-armed when the input returns to centre).
        if (dirX == 0) {
            overlayAxisX = 0
        } else if (dirX != overlayAxisX) {
            overlayAxisX = dirX
            InGameOverlay.handleControllerMove(dirX, 0)
        }
        return true
    }

    private var memcardAxisX = 0
    private var memcardAxisY = 0

    /** Routes the controller stick / D-pad (HAT) to the memory-card dialog's
     *  manual nav (SettingsControllerNav). Touch mode kills Compose D-pad focus,
     *  so the dialog uses the same state-driven model as the settings tabs. Any
     *  direction steps the flat control list; edge-triggered (one move per push). */
    private fun handleMemcardControllerMotion(ev: MotionEvent) {
        val (stickDx, stickDy) = uiDominantStickDirection(
            ev.getAxisValue(MotionEvent.AXIS_X),
            ev.getAxisValue(MotionEvent.AXIS_Y),
        )
        val dirX = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
            .let { if (it != 0) it else stickDx }
        val dirY = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
            .let { if (it != 0) it else stickDy }
        if (dirY != memcardAxisY) {
            memcardAxisY = dirY
            if (dirY != 0) com.armsx2.ui.settings.SettingsControllerNav.move(dirY)
        }
        if (dirX != memcardAxisX) {
            memcardAxisX = dirX
            if (dirX != 0) com.armsx2.ui.settings.SettingsControllerNav.move(dirX)
        }
    }

    private fun handleControllerUiScroll(velocityY: Float) {
        if (WindowImpl.overlayVisible.value) {
            InGameOverlay.handleControllerScroll(velocityY)
        } else if (GamesList.controllerActive()) {
            GamesList.handleControllerScroll(velocityY)
        }
    }

    private fun uiHatDirection(value: Float): Int = when {
        value > UI_HAT_DEAD -> 1
        value < -UI_HAT_DEAD -> -1
        else -> 0
    }

    private fun uiDominantStickDirection(x: Float, y: Float): Pair<Int, Int> {
        val absX = abs(x)
        val absY = abs(y)
        if (absX < UI_NAV_DEAD && absY < UI_NAV_DEAD)
            return 0 to 0
        return if (absX >= absY)
            (if (x > 0f) 1 else -1) to 0
        else
            0 to (if (y > 0f) 1 else -1)
    }

    private fun uiAxisDirection(value: Float): Int = when {
        value > UI_NAV_DEAD -> 1
        value < -UI_NAV_DEAD -> -1
        else -> 0
    }

    private fun uiScrollValue(value: Float): Float {
        val dead = 0.18f
        return when {
            value > dead -> ((value - dead) / (1f - dead)).coerceIn(0f, 1f)
            value < -dead -> ((value + dead) / (1f - dead)).coerceIn(-1f, 0f)
            else -> 0f
        }
    }

    private fun recordUiNav(keyCode: Int, fromAxis: Boolean) {
        lastUiNavCode = keyCode
        lastUiNavAt = SystemClock.uptimeMillis()
        lastUiNavWasAxis = fromAxis
    }

    private fun shouldSuppressUiNav(keyCode: Int, fromAxis: Boolean, now: Long): Boolean {
        if (lastUiNavCode != keyCode) return false
        val age = now - lastUiNavAt
        return lastUiNavWasAxis != fromAxis && age <= UI_KEY_AXIS_SUPPRESS_MS
    }

    private fun dispatchSyntheticUiKey(keyCode: Int): Boolean {
        val now = SystemClock.uptimeMillis()
        val flags = KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY
        val source = InputDevice.SOURCE_KEYBOARD or InputDevice.SOURCE_DPAD
        val down = KeyEvent(
            now, now, KeyEvent.ACTION_DOWN, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags, source
        )
        val up = KeyEvent(
            now, now, KeyEvent.ACTION_UP, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags, source
        )
        val downHandled = super.dispatchKeyEvent(down)
        val upHandled = super.dispatchKeyEvent(up)
        return downHandled || upHandled
    }

    private fun sendAxis(event: MotionEvent, axis: Int, posCode: Int, negCode: Int) {
        val v = event.getAxisValue(axis)
        val posVal = if (v > STICK_DEAD) v else 0f
        val negVal = if (v < -STICK_DEAD) -v else 0f
        NativeApp.setPadButton(posCode, (posVal * 32767).toInt(), posVal > 0f)
        NativeApp.setPadButton(negCode, (negVal * 32767).toInt(), negVal > 0f)
    }

    private fun sendTrigger(event: MotionEvent, axisA: Int, axisB: Int, code: Int) {
        val v = maxOf(event.getAxisValue(axisA), event.getAxisValue(axisB))
        val clamped = if (v > STICK_DEAD) v.coerceAtMost(1f) else 0f
        NativeApp.setPadButton(code, (clamped * 32767).toInt(), clamped > 0f)
    }

    override fun onPause() {
        if (eState.value == EmuState.RUNNING)
            pause()
        // Persist Vulkan pipeline cache before Android can reap the process.
        // ~VKShaderCache only fires on a clean device teardown, but swipe-kill
        // / OOM-kill skip that path — every cold launch would otherwise
        // re-compile every TFX pipeline from scratch. No-op on OpenGL.
        NativeApp.flushShaderCache()
        super.onPause()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleExternalLaunchIntent(intent)
    }

    override fun onResume() {
        // Refresh the All-Files-Access state — the user may have just
        // returned from Settings after granting (or revoking) the
        // MANAGE_EXTERNAL_STORAGE toggle. The setContent block reads
        // this to decide whether to show the grant screen.
        allFilesAccessGranted.value = !needsAllFilesAccess()
        super.onResume()
    }

    override fun onDestroy() {
        NativeApp.shutdown()
        super.onDestroy()

        val appPid = Process.myPid()
        Process.killProcess(appPid)
    }

    private fun handleExternalLaunchIntent(intent: Intent?) {
        val uri = extractLaunchUri(intent) ?: return
        persistReadGrant(intent, uri)
        currentGame.value = null
        pendingExternalLaunch.value = uri.toString()
        launchPendingExternalGameIfReady()
    }

    private fun extractLaunchUri(intent: Intent?): Uri? {
        if (intent == null)
            return null

        intent.data?.let { return it }

        val stream: Uri? = if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri
        }
        stream?.let { return it }

        intent.clipData?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri?.let { return it }

        for (key in listOf("path", "game", "rom", "uri", "android.intent.extra.STREAM")) {
            val value = intent.getStringExtra(key)?.takeIf { it.isNotBlank() } ?: continue
            return Uri.parse(value)
        }

        return null
    }

    private fun persistReadGrant(intent: Intent?, uri: Uri) {
        if (uri.scheme != "content" || intent == null)
            return

        val flags = intent.flags
        if ((flags and Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0 ||
            (flags and Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) == 0)
            return

        runCatching {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
        }
    }
}
