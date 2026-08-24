package com.armsx2

import android.content.Context
import android.os.SystemClock
import java.io.File

/**
 * CPU / GPU / battery temperatures, for the panel's stat tiles.
 *
 * Asked for by two people at once (Cotcho: "temp sensor on applicable device as part of stats
 * OSD... maybe intervals in polling the sensors could help mitigate"; Mike22: "more info from
 * the OSD available on the second screen").
 *
 * Android has no supported API for this. HardwarePropertiesManager exists but is gated behind
 * DEVICE_POWER, which is signature-level, so an app cannot use it. What is left is the thermal
 * sysfs, which is readable without permission on essentially every device but is not a contract:
 * zone COUNT, zone ORDER and zone NAMING are all vendor-specific, and the unit is not fixed
 * either. So this discovers zones once by name, tolerates every failure by simply having no
 * reading, and never claims a value it could not actually read.
 *
 * "Not available on this device" is a normal outcome here, not an error.
 */
object Thermals {

    /** No reading. Distinguished from a real 0°C, which a phone will not be at. */
    const val NONE = Float.MIN_VALUE

    private const val ZONES = "/sys/class/thermal"

    /** Substrings that identify a zone, in preference order. Qualcomm, MediaTek, Exynos and
     *  Tensor all name theirs differently, and several expose a dozen CPU zones (one per
     *  cluster or core); the first match is taken because a single representative reading is
     *  what a stat tile wants, not the hottest-of-twelve. */
    private val CPU_HINTS = listOf("cpu-0-0", "cpuss", "mtktscpu", "cpu_thermal", "cpu")
    private val GPU_HINTS = listOf("gpuss", "mtktsgpu", "gpu_thermal", "gpu")

    private var scanned = false
    private var cpuZone: File? = null
    private var gpuZone: File? = null

    /** Last readings, and when they were taken. Kept so a caller polling faster than the
     *  interval gets the previous value rather than hitting sysfs on every frame. */
    @Volatile var cpu: Float = NONE; private set
    @Volatile var gpu: Float = NONE; private set
    @Volatile var battery: Float = NONE; private set
    private var lastPollMs = 0L

    /** True once a scan has happened and found nothing, so the UI can hide the tiles rather
     *  than showing three permanent dashes. */
    val available: Boolean get() = cpu != NONE || gpu != NONE || battery != NONE

    private fun scan() {
        if (scanned) return
        scanned = true
        val zones = runCatching {
            File(ZONES).listFiles { f -> f.name.startsWith("thermal_zone") }?.sortedBy { it.name }
        }.getOrNull().orEmpty()
        // type -> zone dir, read once. A zone whose type is unreadable is simply skipped.
        val named = zones.mapNotNull { z ->
            val type = runCatching { File(z, "type").readText().trim().lowercase() }.getOrNull()
            if (type.isNullOrEmpty()) null else type to z
        }
        fun pick(hints: List<String>): File? {
            for (h in hints) named.firstOrNull { it.first.contains(h) }?.let { return it.second }
            return null
        }
        cpuZone = pick(CPU_HINTS)
        gpuZone = pick(GPU_HINTS)
    }

    /**
     * Convert whatever the kernel wrote into degrees Celsius.
     *
     * The unit is genuinely not standard: most zones report millidegrees (45000), some report
     * tenths (450), a few report plain degrees (45). Rather than guess per vendor, the magnitude
     * decides — no phone runs at 1000°C, and none idles at 0.045°C, so the ranges do not overlap.
     */
    private fun toCelsius(raw: Long): Float = when {
        raw > 10_000 -> raw / 1000f
        raw > 1_000 -> raw / 100f
        raw > 200 -> raw / 10f
        else -> raw.toFloat()
    }

    private fun read(zone: File?): Float {
        val f = zone ?: return NONE
        val raw = runCatching { File(f, "temp").readText().trim().toLong() }.getOrNull() ?: return NONE
        val c = toCelsius(raw)
        // A plausibility gate. Some zones are not temperatures at all (fan RPM, a cooling-device
        // state), and a tile reading "912°C" is worse than a tile reading nothing.
        return if (c in -20f..150f) c else NONE
    }

    /**
     * Refresh if [intervalMs] has passed. Cheap to call often — the rate limit is the point,
     * since these are file reads and the caller is a UI tick.
     */
    fun poll(context: Context, intervalMs: Long) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastPollMs < intervalMs) return
        lastPollMs = now
        scan()
        cpu = read(cpuZone)
        gpu = read(gpuZone)
        // Battery is the one with a real API. Tenths of a degree, per the documented extra.
        battery = runCatching {
            val i = context.registerReceiver(null, android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED))
            val tenths = i?.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
                ?: Int.MIN_VALUE
            if (tenths == Int.MIN_VALUE) NONE else (tenths / 10f).takeIf { it in -20f..150f } ?: NONE
        }.getOrDefault(NONE)
    }

    // ---- Feeding the in-game overlay -----------------------------------------------------
    // The panel polls on its own tick, but the overlay runs whether or not a second screen
    // exists, so it needs a poll of its own. Same interval, same readings; the only extra cost
    // is the JNI push, and it stops entirely when the option is off.
    private const val PREF_OSD = "osd.showTemps"
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())
    private var feeding = false

    // Default ON. It reads as a normal part of the perf overlay next to CPU/GPU load, the poll
    // is one file read every couple of seconds, and a device with no readable zone shows nothing
    // rather than something wrong -- so there is no device this is worse for.
    val osdEnabled = androidx.compose.runtime.mutableStateOf(true)

    fun loadOsdEnabled(context: Context) {
        osdEnabled.value = runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean(PREF_OSD, true)
        }.getOrDefault(true)
        applyOsd(context)
    }

    fun setOsdEnabled(context: Context, on: Boolean) {
        osdEnabled.value = on
        runCatching {
            com.armsx2.runtime.MainActivityRuntime.prefs.edit().putBoolean(PREF_OSD, on).apply()
        }
        applyOsd(context)
    }

    private fun applyOsd(context: Context) {
        if (osdEnabled.value) start(context) else stop()
    }

    private fun start(context: Context) {
        if (feeding) return
        feeding = true
        val app = context.applicationContext
        val pump = object : Runnable {
            override fun run() {
                if (!feeding) return
                val interval = com.armsx2.SecondScreen.tempIntervalSec.value * 1000L
                poll(app, interval)
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setThermals(cpu, gpu, battery, true) }
                handler.postDelayed(this, interval)
            }
        }
        handler.post(pump)
    }

    private fun stop() {
        feeding = false
        handler.removeCallbacksAndMessages(null)
        // Tell the overlay to stop drawing them, rather than leaving the last values frozen there.
        runCatching { kr.co.iefriends.pcsx2.NativeApp.setThermals(NONE, NONE, NONE, false) }
    }

    /** "48°" or null when there is no reading. */
    fun format(c: Float): String? = if (c == NONE) null else "${c.toInt()}°"
}
