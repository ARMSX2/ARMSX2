package com.armsx2.input

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.Surface
import android.view.WindowManager
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.hypot

class AndroidGyroscopeInput(
    context: Context,
    private val onAnalog: (mode: Int, x: Float, y: Float) -> Unit
) : SensorEventListener {
    companion object {
        /** How the active sensor derives a stick value. Aim wants angular RATE, steering wants an
         *  absolute ANGLE, and the tilt fallback can only ever give an angle — see [KIND_TILT]. */
        const val KIND_NONE = 0

        /** TYPE_GYROSCOPE: angular velocity. True 1:1 aim. */
        const val KIND_GYRO = 1

        /** A fused rotation vector: absolute attitude including yaw. */
        const val KIND_ROTATION = 2

        /** TYPE_GRAVITY / TYPE_ACCELEROMETER: the gravity direction only. Present on virtually
         *  every phone, so this is what makes motion control work at all on gyro-less devices.
         *
         *  Physical limit worth knowing before reading the mapping below: gravity cannot observe
         *  YAW. Turning a device about the vertical axis leaves the gravity vector unchanged, so
         *  there is no way to recover "panned left/right" from it. Steering is unaffected (it is
         *  a roll about the screen normal, which gravity sees perfectly), but tilt-mode AIM has
         *  to drive X from twist instead of from turning. That is the same trade every tilt-only
         *  mobile game makes; it is not a bug, and it is why the UI labels the fallback. */
        const val KIND_TILT = 3

        private fun sensorChainFor(manager: SensorManager, mode: Int): Pair<Sensor, Int>? {
            // Ordered best-to-worst. Steering only needs an absolute roll, so it can start at the
            // fused rotation vector; aim prefers the raw gyro because rate integration is what
            // makes 1:1 look feel right.
            val chain: List<Pair<Int, Int>> = when (mode) {
                1 -> listOf(
                    Sensor.TYPE_GYROSCOPE to KIND_GYRO,
                    Sensor.TYPE_GRAVITY to KIND_TILT,
                    Sensor.TYPE_ACCELEROMETER to KIND_TILT,
                )
                2 -> listOf(
                    Sensor.TYPE_GAME_ROTATION_VECTOR to KIND_ROTATION,
                    // Magnetometer-fused; drifts in yaw but we only read roll, which is fine.
                    Sensor.TYPE_ROTATION_VECTOR to KIND_ROTATION,
                    Sensor.TYPE_GRAVITY to KIND_TILT,
                    Sensor.TYPE_ACCELEROMETER to KIND_TILT,
                )
                else -> emptyList()
            }
            for ((type, kind) in chain) {
                val sensor = manager.getDefaultSensor(type)
                if (sensor != null) return sensor to kind
            }
            return null
        }

        fun isModeAvailable(context: Context, mode: Int): Boolean =
            mode == 0 || resolveKind(context, mode) != KIND_NONE

        /** Which [KIND_GYRO]/[KIND_ROTATION]/[KIND_TILT] this device would actually use for
         *  [mode]. The Pad tab reads this so it can say "using the accelerometer" rather than
         *  claiming the mode is unavailable. */
        fun resolveKind(context: Context, mode: Int): Int {
            val manager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
            return sensorChainFor(manager, mode)?.second ?: KIND_NONE
        }
    }

    private val appContext = context.applicationContext
    private val sensorManager = appContext.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val windowManager = appContext.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private var activeSensor: Sensor? = null
    private var activeKind = KIND_NONE
    private var mode = 0
    private var sensitivity = 1f
    private var smoothing = 0.45f
    private var invertX = false
    private var invertY = false
    private var filteredX = 0f
    private var filteredY = 0f
    private var lastSentX = 0f
    private var lastSentY = 0f
    private var wasActive = false
    private var steeringCenter: Float? = null
    private var tiltCenterX: Float? = null
    private var tiltCenterY: Float? = null
    // Raw TYPE_ACCELEROMETER carries the player's hand movement on top of gravity. TYPE_GRAVITY is
    // already fused, so only smooth when we're reading the raw sensor.
    private var gravityX = 0f
    private var gravityY = 0f
    private var gravityZ = 0f
    private var haveGravity = false

    /** The sensor kind [start] settled on, or [KIND_NONE] when not running. */
    val kind: Int get() = activeKind

    fun start(mode: Int, sensitivityPercent: Int, smoothingPercent: Int, invertX: Boolean, invertY: Boolean): Boolean {
        stop()
        this.mode = mode
        this.sensitivity = sensitivityPercent.coerceIn(25, 300) / 100f
        this.smoothing = smoothingPercent.coerceIn(0, 90) / 100f
        this.invertX = invertX
        this.invertY = invertY
        filteredX = 0f
        filteredY = 0f
        lastSentX = 0f
        lastSentY = 0f
        wasActive = false
        steeringCenter = null
        tiltCenterX = null
        tiltCenterY = null
        haveGravity = false
        val resolved = sensorChainFor(sensorManager, mode) ?: return false
        activeSensor = resolved.first
        activeKind = resolved.second
        return sensorManager.registerListener(this, resolved.first, SensorManager.SENSOR_DELAY_GAME)
    }

    fun recenter() {
        steeringCenter = null
        tiltCenterX = null
        tiltCenterY = null
        filteredX = 0f
        filteredY = 0f
        wasActive = false
        onAnalog(mode, 0f, 0f)
    }

    fun stop() {
        activeSensor?.let { sensorManager.unregisterListener(this, it) }
        activeSensor = null
        activeKind = KIND_NONE
        steeringCenter = null
        tiltCenterX = null
        tiltCenterY = null
        filteredX = 0f
        filteredY = 0f
        wasActive = false
        haveGravity = false
        onAnalog(mode, 0f, 0f)
    }

    override fun onSensorChanged(event: SensorEvent) {
        val raw = when (activeKind) {
            KIND_GYRO -> aimValues(event)
            KIND_ROTATION -> steeringValues(event)
            KIND_TILT -> tiltValues(event)
            else -> return
        }
        var x = applyDeadzone(raw.first.coerceIn(-1f, 1f), 0.035f)
        var y = applyDeadzone(raw.second.coerceIn(-1f, 1f), 0.035f)
        if (invertX) x = -x
        if (invertY) y = -y
        val alpha = (1f - smoothing * 0.86f).coerceIn(0.16f, 1f)
        filteredX += (x - filteredX) * alpha
        filteredY += (y - filteredY) * alpha
        val outputX = filteredX.coerceIn(-1f, 1f)
        val outputY = filteredY.coerceIn(-1f, 1f)
        val active = kotlin.math.abs(outputX) > 0.004f || kotlin.math.abs(outputY) > 0.004f
        if (!active && !wasActive) return
        if (active && kotlin.math.abs(outputX - lastSentX) < 0.004f && kotlin.math.abs(outputY - lastSentY) < 0.004f) return
        val sentX = if (active) outputX else 0f
        val sentY = if (active) outputY else 0f
        lastSentX = sentX
        lastSentY = sentY
        wasActive = active
        onAnalog(mode, sentX, sentY)
    }

    private fun aimValues(event: SensorEvent): Pair<Float, Float> {
        if (event.sensor.type != Sensor.TYPE_GYROSCOPE) return 0f to 0f
        val rotation = @Suppress("DEPRECATION") windowManager.defaultDisplay.rotation
        val gx = event.values[0]
        val gy = event.values[1]
        val axes = when (rotation) {
            Surface.ROTATION_90 -> gx to gy
            Surface.ROTATION_270 -> -gx to -gy
            Surface.ROTATION_180 -> gy to -gx
            else -> -gy to -gx
        }
        return (axes.first * 0.72f * sensitivity) to (axes.second * 0.72f * sensitivity)
    }

    private fun steeringValues(event: SensorEvent): Pair<Float, Float> {
        val matrix = FloatArray(9)
        val remapped = FloatArray(9)
        SensorManager.getRotationMatrixFromVector(matrix, event.values)
        val rotation = @Suppress("DEPRECATION") windowManager.defaultDisplay.rotation
        val (axisX, axisY) = when (rotation) {
            Surface.ROTATION_90 -> SensorManager.AXIS_Y to SensorManager.AXIS_MINUS_X
            Surface.ROTATION_180 -> SensorManager.AXIS_MINUS_X to SensorManager.AXIS_MINUS_Y
            Surface.ROTATION_270 -> SensorManager.AXIS_MINUS_Y to SensorManager.AXIS_X
            else -> SensorManager.AXIS_X to SensorManager.AXIS_Y
        }
        SensorManager.remapCoordinateSystem(matrix, axisX, axisY, remapped)
        val roll = SensorManager.getOrientation(remapped, FloatArray(3))[2]
        val center = steeringCenter ?: roll.also { steeringCenter = it }
        val delta = wrapPi(roll - center)
        val steeringRange = Math.toRadians(32.0).toFloat()
        return (delta / steeringRange * sensitivity).coerceIn(-1f, 1f) to 0f
    }

    /** Gravity-only fallback. Both axes are relative to wherever the device was when the mode
     *  started (or since the last [recenter]), so it doesn't matter how the player holds it. */
    private fun tiltValues(event: SensorEvent): Pair<Float, Float> {
        if (event.sensor.type == Sensor.TYPE_GRAVITY) {
            gravityX = event.values[0]
            gravityY = event.values[1]
            gravityZ = event.values[2]
        } else {
            // Low-pass the raw accelerometer to separate gravity from hand movement. Seed on the
            // first sample so a hard tilt at startup isn't slewed in from zero.
            if (!haveGravity) {
                gravityX = event.values[0]
                gravityY = event.values[1]
                gravityZ = event.values[2]
            } else {
                val a = 0.2f
                gravityX += (event.values[0] - gravityX) * a
                gravityY += (event.values[1] - gravityY) * a
                gravityZ += (event.values[2] - gravityZ) * a
            }
        }
        haveGravity = true

        // Express gravity in SCREEN axes, matching the remapCoordinateSystem table above.
        val rotation = @Suppress("DEPRECATION") windowManager.defaultDisplay.rotation
        val (right, up) = when (rotation) {
            Surface.ROTATION_90 -> gravityY to -gravityX
            Surface.ROTATION_180 -> -gravityX to -gravityY
            Surface.ROTATION_270 -> -gravityY to gravityX
            else -> gravityX to gravityY
        }

        // Roll about the screen normal: what a player does to "steer". Held level, gravity runs
        // down the screen (up < 0) and this is 0.
        val roll = atan2(right, -up)
        // Pitch: tipping the top of the device toward or away from the player.
        val pitch = atan2(gravityZ, hypot(right, up))

        val centerRoll = tiltCenterX ?: roll.also { tiltCenterX = it }
        val centerPitch = tiltCenterY ?: pitch.also { tiltCenterY = it }
        val dRoll = wrapPi(roll - centerRoll)
        val dPitch = wrapPi(pitch - centerPitch)

        val range = Math.toRadians(32.0).toFloat()
        val rollOut = (dRoll / range * sensitivity).coerceIn(-1f, 1f)
        val pitchOut = (dPitch / range * sensitivity).coerceIn(-1f, 1f)

        // Steering is X-only, same contract as the rotation-vector path. Aim gets both axes, with
        // X coming from twist because yaw is unobservable from gravity (see KIND_TILT).
        return if (mode == 2) rollOut to 0f else rollOut to pitchOut
    }

    private fun wrapPi(value: Float): Float {
        var v = value
        while (v > PI) v -= (2 * PI).toFloat()
        while (v < -PI) v += (2 * PI).toFloat()
        return v
    }

    private fun applyDeadzone(value: Float, deadzone: Float): Float {
        val magnitude = kotlin.math.abs(value)
        if (magnitude <= deadzone) return 0f
        return kotlin.math.sign(value) * ((magnitude - deadzone) / (1f - deadzone))
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
}
