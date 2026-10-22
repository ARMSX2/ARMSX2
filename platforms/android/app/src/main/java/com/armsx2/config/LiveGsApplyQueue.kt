package com.armsx2.config

import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

object LiveGsApplyQueue {
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "GSLiveApply").apply { isDaemon = true }
    }

    private val latestSettings = AtomicReference<Settings?>(null)
    private val settingsRunning = AtomicBoolean(false)
    private val latestUpscale = AtomicReference<Float?>(null)
    private val upscaleRunning = AtomicBoolean(false)
    private val latestFramerate = AtomicReference<Pair<Float, Float>?>(null)
    private val framerateRunning = AtomicBoolean(false)

    // SAF ELF names can give the app a decoded key that differs from the native serial.
    data class VmIdentity(val generation: Long, val serial: String, val active: Boolean, val appKey: String?) {
        fun isCurrent(): Boolean = generation == MainActivityRuntime.vmSessionGeneration.get() &&
            !MainActivityRuntime.isVmStopInProgress() &&
            (active || !MainActivityRuntime.hasVmRunLoop()) &&
            active == NativeApp.hasActiveVM() &&
            (!active || serial == NativeApp.getGameSerial()) &&
            appKey == MainActivityRuntime.currentGame.value?.settingsKey &&
            generation == MainActivityRuntime.vmSessionGeneration.get()
    }

    fun captureVmIdentity() = VmIdentity(
        MainActivityRuntime.vmSessionGeneration.get(), NativeApp.getGameSerial(), NativeApp.hasActiveVM(),
        MainActivityRuntime.currentGame.value?.settingsKey,
    )

    // A new boot waits off main for any in-flight native apply from the previous session.
    val vmSettingsLock = Any()

    private class Task(val saveFor: VmIdentity?, val apply: () -> Unit)
    private val pending = java.util.ArrayDeque<Task>()
    private var draining = false
    private var appliedIdentity: VmIdentity? = null
    private var appliedSettings: Settings? = null

    @Synchronized
    fun previousSettings(identity: VmIdentity, initial: Settings): Settings {
        if (appliedIdentity != identity) {
            appliedIdentity = identity
            appliedSettings = initial
        }
        return appliedSettings ?: initial
    }

    @Synchronized
    fun recordAppliedSettings(settings: Settings) {
        appliedIdentity = captureVmIdentity()
        appliedSettings = settings
    }

    fun enqueue(apply: () -> Unit) = submit(Task(null, apply))

    fun enqueueForVm(identity: VmIdentity = captureVmIdentity(), apply: () -> Unit) {
        enqueue { if (identity.isCurrent()) apply() }
    }

    fun enqueueSave(identity: VmIdentity, apply: () -> Unit) {
        submit(Task(identity) { if (identity.isCurrent()) apply() })
    }

    private fun submit(task: Task) {
        synchronized(pending) {
            // Only adjacent, not-yet-started saves may replace each other. Everything else is a barrier.
            if (task.saveFor != null && pending.peekLast()?.saveFor == task.saveFor)
                pending.removeLast()
            pending.addLast(task)
            if (draining) return
            draining = true
        }
        executor.execute {
            while (true) {
                val next = synchronized(pending) {
                    if (pending.isEmpty()) {
                        draining = false
                        null
                    } else pending.removeFirst()
                } ?: break
                runCatching { synchronized(vmSettingsLock) { next.apply() } }
                    .onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ apply_error=${it.javaClass.simpleName}") }
            }
        }
    }

    fun applySettings(settings: Settings) {
        latestSettings.set(settings)
        if (settingsRunning.compareAndSet(false, true))
            executor.execute(::drainSettings)
    }

    fun applyUpscale(multiplier: Float) {
        latestUpscale.set(multiplier)
        if (upscaleRunning.compareAndSet(false, true))
            executor.execute(::drainUpscale)
    }

    /** Live per-region framerate (NTSC, PAL). Coalesced + run off the UI thread
     *  because applyFramerateLive parks the VM (UpdateVSyncRate touches EE-thread
     *  counters). Persists to the native base layer first so a later full
     *  ApplySettings reload can't revert it. */
    fun applyFramerate(ntsc: Float, pal: Float) {
        latestFramerate.set(ntsc to pal)
        if (framerateRunning.compareAndSet(false, true))
            executor.execute(::drainFramerate)
    }

    private fun drainSettings() {
        try {
            while (true) {
                val settings = latestSettings.getAndSet(null) ?: break
                val applied = runCatching { settings.applyGsLive() }
                    .onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ settings_error=${it.javaClass.simpleName}") }
                    .getOrDefault(false)
                println("@@ANDROID_GS_LIVE_QUEUE@@ settings_applied=${if (applied) 1 else 0}")
            }
        } finally {
            settingsRunning.set(false)
            if (latestSettings.get() != null && settingsRunning.compareAndSet(false, true))
                executor.execute(::drainSettings)
        }
    }

    private fun drainUpscale() {
        try {
            while (true) {
                val multiplier = latestUpscale.getAndSet(null) ?: break
                runCatching { NativeApp.renderUpscalemultiplier(multiplier) }
                    .onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ upscale_error=${it.javaClass.simpleName}") }
                println("@@ANDROID_GS_LIVE_QUEUE@@ upscale value=$multiplier")
            }
        } finally {
            upscaleRunning.set(false)
            if (latestUpscale.get() != null && upscaleRunning.compareAndSet(false, true))
                executor.execute(::drainUpscale)
        }
    }

    private fun drainFramerate() {
        try {
            while (true) {
                val (ntsc, pal) = latestFramerate.getAndSet(null) ?: break
                runCatching {
                    // Persist to the native base layer so a subsequent full
                    // ApplySettings (e.g. EE-cycle commit) keeps the new rate.
                    NativeApp.setSetting("EmuCore/GS", "FramerateNTSC", "float", ntsc.toString())
                    NativeApp.setSetting("EmuCore/GS", "FrameratePAL", "float", pal.toString())
                    NativeApp.applyFramerateLive(ntsc, pal)
                }.onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ framerate_error=${it.javaClass.simpleName}") }
                println("@@ANDROID_GS_LIVE_QUEUE@@ framerate ntsc=$ntsc pal=$pal")
            }
        } finally {
            framerateRunning.set(false)
            if (latestFramerate.get() != null && framerateRunning.compareAndSet(false, true))
                executor.execute(::drainFramerate)
        }
    }
}
