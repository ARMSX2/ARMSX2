package com.armsx2.input

import android.view.InputDevice

/**
 * Auto-assigns physical controllers to PS2 pad slots for local multiplayer.
 *
 *  - Multitap OFF (default): first distinct gamepad = Player 1 (slot 0); next = Player 2
 *    (slot 1, hot-plugged via [onPlayer2Joined]); any further pads fold into Player 1.
 *  - Multitap ON: up to 8 distinct gamepads claim unified slots 0..7. The unified layout
 *    matches the native engine (Sio.h): slots 0,1 = the two port mains; 2,3,4 = port-0
 *    taps; 5,6,7 = port-1 taps. Filling the next free slot yields (P1, P2, port-0 taps,
 *    port-1 taps) — the natural unified order — so no per-slot mapping is needed here.
 *
 * The tap slots (2-7) are armed natively at game boot / by the Multitap toggle, NOT from
 * this router (mirrors how [onPlayer2Joined] only hot-plugs the P2 main). The on-screen
 * touch controls and all menu navigation always use slot 0 — they never go through here.
 * Reset on VM start AND stop ([reset]) so each session re-pairs deterministically.
 *
 * Called only from the in-game input dispatch where a real `event.deviceId` is live.
 */
object PadRouter {
    // Nintendo USB/Bluetooth vendor id. A Joy-Con pair enumerates as TWO InputDevices
    // (L + R) under this vendor, but they are one physical controller — so both halves
    // are routed to a single port instead of being split across players.
    private const val NINTENDO_VENDOR_ID = 0x057E
    /** Unified slot count: 2 port mains + 6 multitap slots. */
    const val MAX_PADS = 8
    // Unified slot -> claimed Android deviceId (-1 = unclaimed).
    private val slots = IntArray(MAX_PADS) { -1 }
    @Volatile private var pad2Enabled = false

    /** Master gate. OFF (default) => classic 2-slot co-op (3rd+ pad folds to P1). ON =>
     *  up to 8 distinct gamepads claim slots 0..7. Seeded at app start from
     *  ControllerMappings.multitapEnabled() and updated by the Pad-tab toggle. */
    @Volatile var multitapEnabled = false

    /** How many slots the current Multitap setting actually arms. A pin beyond this is
     *  inert until Multitap is turned on -- honouring it would route a pad at an un-armed
     *  PS2 port, where its input goes nowhere at all (the #394 failure shape). */
    private fun activeSlotCount(): Int = if (multitapEnabled) MAX_PADS else 2

    /**
     * Player slots the user pinned by hand, descriptor -> port.
     *
     * Keyed by InputDevice.descriptor, not deviceId: ids are reassigned on reconnect (the very
     * thing [forgetDevice] exists for), so an id-keyed pin would survive exactly one sleep/wake.
     * The descriptor is derived from the device's identity and is stable across reconnects.
     *
     * Auto-assignment is first-to-press, which is fine until two people care WHICH player they
     * are -- an external pad and a handheld's built-in controller cannot be told apart by press
     * order alone. A pin overrides that for the pads it names and leaves the rest automatic.
     */
    private val pinned = LinkedHashMap<String, Int>()
    private const val KEY_PINNED = "pad_router_pinned"

    /**
     * Where a controller's rumble should go.
     *
     * Needed because a pad can ADVERTISE motors it cannot drive. A handheld that bridges an
     * external controller through its own HID node presents that node with a full vibrator
     * inventory -- ids, hasVibrator, the lot -- accepts every vibrate call without error, and
     * moves nothing, because it never forwards force feedback back to the pad. Nothing in the
     * API distinguishes that from a working motor, so the choice has to be the user's.
     */
    enum class RumbleMode { AUTO, CONTROLLER, DEVICE, OFF }

    private val rumbleModes = LinkedHashMap<String, RumbleMode>()
    private const val KEY_RUMBLE = "pad_router_rumble"

    /** Load pins and rumble choices from prefs. Safe to call more than once. */
    fun loadPins() {
        pinned.clear()
        rumbleModes.clear()
        runCatching {
            val raw = com.armsx2.runtime.MainActivityRuntime.prefs.getString(KEY_PINNED, null)
            if (raw != null) {
                val obj = org.json.JSONObject(raw)
                for (key in obj.keys()) {
                    val port = obj.optInt(key, -1)
                    if (port in 0 until MAX_PADS) pinned[key] = port
                }
            }
        }
        runCatching {
            val raw = com.armsx2.runtime.MainActivityRuntime.prefs.getString(KEY_RUMBLE, null)
            if (raw != null) {
                val obj = org.json.JSONObject(raw)
                for (key in obj.keys()) {
                    val name = obj.optString(key, "")
                    RumbleMode.entries.firstOrNull { it.name == name }?.let { rumbleModes[key] = it }
                }
            }
        }
    }

    /** Where this controller's rumble goes. */
    fun rumbleMode(descriptor: String?): RumbleMode =
        descriptor?.let { rumbleModes[it] } ?: RumbleMode.AUTO

    /** Rumble choice for a live device id, for the push-driven motor path. */
    fun rumbleModeForDevice(deviceId: Int): RumbleMode =
        rumbleMode(descriptorOf(deviceId))

    fun setRumbleMode(descriptor: String, mode: RumbleMode) {
        if (mode == RumbleMode.AUTO) rumbleModes.remove(descriptor) else rumbleModes[descriptor] = mode
        val obj = org.json.JSONObject()
        rumbleModes.forEach { (d, m) -> obj.put(d, m.name) }
        com.armsx2.runtime.MainActivityRuntime.prefs.edit().putString(KEY_RUMBLE, obj.toString()).apply()
    }

    private fun savePins() {
        val obj = org.json.JSONObject()
        pinned.forEach { (descriptor, port) -> obj.put(descriptor, port) }
        com.armsx2.runtime.MainActivityRuntime.prefs.edit()
            .putString(KEY_PINNED, obj.toString()).apply()
    }

    /** The user's pins, descriptor -> port. */
    fun pins(): Map<String, Int> = LinkedHashMap(pinned)

    /** True when this controller is pinned to some OTHER player than [port]. */
    fun pinnedElsewhere(descriptor: String?, port: Int): Boolean {
        val pin = descriptor?.let { pinned[it] } ?: return false
        return pin != port
    }

    /**
     * Pin a controller to a player slot, or pass null to clear it.
     *
     * A slot holds one controller, so pinning displaces whatever else claimed it -- both the
     * pin and the live slot, otherwise the settings screen would disagree with where input
     * actually goes until the next reset.
     */
    fun setPin(descriptor: String, port: Int?) {
        if (port == null) {
            pinned.remove(descriptor)
        } else {
            if (port !in 0 until MAX_PADS) return
            pinned.entries.removeAll { it.value == port && it.key != descriptor }
            pinned[descriptor] = port
            val claimed = slots[port]
            if (claimed >= 0 && descriptorOf(claimed) != descriptor) slots[port] = -1
            for (i in slots.indices) {
                if (i != port && slots[i] >= 0 && descriptorOf(slots[i]) == descriptor) slots[i] = -1
            }
        }
        savePins()
    }

    private fun descriptorOf(deviceId: Int): String? =
        runCatching { InputDevice.getDevice(deviceId)?.descriptor }.getOrNull()

    /** One physical controller, as the settings screen lists it. */
    data class Pad(val descriptor: String, val name: String, val deviceId: Int)

    /** Connected gamepads, deduped to one entry per physical controller. */
    fun connectedPads(): List<Pad> {
        val out = LinkedHashMap<String, Pad>()
        for (id in runCatching { InputDevice.getDeviceIds() }.getOrDefault(IntArray(0))) {
            val dev = InputDevice.getDevice(id) ?: continue
            val src = dev.sources
            val isPad = (src and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (src and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            if (!isPad) continue
            val descriptor = dev.descriptor ?: continue
            // A controller can enumerate as several nodes (a DualSense adds touchpad and motion
            // ones); keep the first GAMEPAD node, which is what claims a slot and carries the motor.
            out.getOrPut(descriptor) { Pad(descriptor, dev.name ?: "Controller", id) }
        }
        return out.values.toList()
    }

    /**
     * A pad we deliver ourselves, and the slot it belongs to.
     *
     * The USB takeover claims a controller's interface, which destroys its Android input device,
     * so its events are synthesised with an id no real device has. Without this they would fall
     * into the `deviceId < 0` case below and always play as player 1, silently ignoring the slot
     * the user pinned -- which is the one thing the pin exists to control.
     */
    @Volatile private var syntheticDeviceId: Int = 0
    @Volatile private var syntheticPort: Int = 0

    fun setSyntheticPad(deviceId: Int, port: Int) {
        syntheticDeviceId = deviceId
        syntheticPort = port.coerceIn(0, MAX_PADS - 1)
    }

    fun clearSyntheticPad() {
        syntheticDeviceId = 0
    }

    /**
     * The controller that should answer for [port] when nothing has claimed it yet.
     *
     * One pad must not be able to answer for every slot. Guessing used to mean "the pad you last
     * touched", and that names the SAME controller for every port -- so a single DualSense
     * swallowed both players' rumble and the other pad stayed silent no matter which slot it was
     * in. Instead, deal the controllers nobody has spoken for out to the players nobody has
     * spoken for, in order.
     *
     * Pinned pads and pinned ports are removed from both sides first, so an explicit assignment
     * is never part of the deal and never has a leftover handed to it. Returns -1 when this port
     * genuinely has no controller behind it -- buzzing someone else's pad is worse than silence.
     */
    fun fallbackDeviceIdForPort(port: Int): Int {
        if (port !in slots.indices) return -1
        val pins = pins()
        val free = connectedPads().filter { pins[it.descriptor] == null }
        if (free.isEmpty()) return -1
        val pinnedPorts = pins.values.toSet()
        var rank = 0
        for (p in 0 until port) if (p !in pinnedPorts) rank++
        return free.getOrNull(rank)?.deviceId ?: -1
    }

    /** Fired exactly once, the first time the 2nd controller (slot 1 = P2 main) joins,
     *  so the app can hot-plug the native Pad2 slot before any P2 input is sent. Tap
     *  slots (2-7) don't use this — they're armed at boot / by the Multitap toggle. */
    @Volatile var onPlayer2Joined: (() -> Unit)? = null

    fun reset() {
        for (i in slots.indices) slots[i] = -1
        pad2Enabled = false
    }

    /** Release the slot a now-departed device held, so a re-enumerated controller re-claims from the
     *  top. Called on device removal: AYANEO handhelds power-cycle the built-in pad on sleep/wake and
     *  it comes back with a NEW deviceId — the stale id otherwise keeps owning Player 1's slot, and
     *  the woken pad claims slot 1 (an un-armed PS2 port 2), so gameplay input goes nowhere (#394). */
    fun forgetDevice(deviceId: Int) {
        if (deviceId < 0) return
        for (i in slots.indices) if (slots[i] == deviceId) slots[i] = -1
    }

    /** Free every slot whose claimed device is no longer connected. Called on resume / focus regain
     *  as a backstop for [forgetDevice] when the remove event landed while we were paused.
     *  [activeDeviceIds] is `InputDevice.getDeviceIds()`. */
    fun pruneStale(activeDeviceIds: IntArray) {
        for (i in slots.indices) {
            val id = slots[i]
            if (id >= 0 && id !in activeDeviceIds) slots[i] = -1
        }
    }

    /** True once a second controller has joined this session (P2 main is live). */
    fun coopActive(): Boolean = slots[1] != -1

    /** Android InputDevice id assigned to a unified pad slot, or -1 if unclaimed.
     *  Lets per-slot PS2 rumble buzz the right pad. */
    fun deviceIdForPort(port: Int): Int {
        if (port !in slots.indices) return -1

        // A PIN OUTRANKS A CLAIM. Slots are filled first-to-press, so a pad can auto-claim a
        // port before the user ever states a preference -- on a handheld the built-in pad
        // usually claims 0, pushing an external controller to 1. Reading the slot first meant
        // the stale claim answered for the port the user had since assigned elsewhere, so a
        // DualSense pinned to player 1 still replied as player 2.
        //
        // Checked against connected pads, so a pin for a controller that is not plugged in
        // does not black-hole the port.
        if (pinned.isNotEmpty()) {
            for (pad in connectedPads()) {
                if (pinned[pad.descriptor] == port) return pad.deviceId
            }
        }

        val claimed = slots[port]
        if (claimed >= 0) {
            // Drop the claim if the device is gone (ids are reassigned on every reconnect) or
            // if it has since been pinned to somebody else. Self-healing here keeps a stale
            // slot from outliving the session that made it.
            val dev = runCatching { InputDevice.getDevice(claimed) }.getOrNull()
            if (dev != null && !pinnedElsewhere(dev.descriptor, port)) return claimed
            slots[port] = -1
        }
        return -1
    }

    /**
     * Map a physical input device to a unified PS2 pad slot (0..7, or 0..1 when multitap
     * is off), claiming the next free slot. Synthetic / virtual events (deviceId < 0)
     * and non-gamepad nodes never claim a slot — they're treated as Player 1.
     */
    fun portForDevice(deviceId: Int): Int {
        // The USB takeover's synthetic id, which belongs to the slot it was started for.
        if (deviceId != 0 && deviceId == syntheticDeviceId) return syntheticPort
        if (deviceId < 0) return 0
        // Fast path: already-claimed nodes (no InputDevice lookup).
        for (i in slots.indices) if (slots[i] == deviceId) return i
        val dev = InputDevice.getDevice(deviceId)
        // Nintendo Joy-Cons (vendor 0x057E) enumerate as TWO InputDevices — the L and R
        // halves of ONE physical controller. Collapse BOTH onto Player 1 (port 0) so a
        // pair drives a single PS2 pad instead of splitting across P1/P2 (which made a
        // 1-player game respond to only one half, and a co-op game see two controllers).
        // They never claim a slot, so onPlayer2Joined stays silent for a lone pair. Gated
        // strictly on the Nintendo vendor id — every other controller keeps its routing.
        if (dev?.vendorId == NINTENDO_VENDOR_ID) return 0
        // Only a real GAMEPAD/JOYSTICK node may claim a slot. One physical controller
        // (notably a DualSense over Bluetooth) enumerates as SEVERAL InputDevices — a
        // gamepad node PLUS a touchpad/mouse node. Gating claims to gamepad sources stops
        // a secondary node from eating a slot and splitting one pad across two players.
        val src = dev?.sources ?: 0
        val isPad = (src and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (src and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        if (!isPad) return 0 // touchpad / mouse / keyboard node → treat as P1, don't claim
        val maxSlots = activeSlotCount()
        // A hand-picked slot beats press order. Written into `slots` too, so everything reading
        // the live assignment agrees with the pin. A pin into a multitap slot is ignored while
        // Multitap is off: that PS2 port is not armed, so routing there drops the pad's input
        // entirely -- worse than the press-order slot it would otherwise get.
        val descriptor = dev?.descriptor
        if (descriptor != null) {
            pinned[descriptor]?.let { port ->
                if (port < maxSlots) {
                    slots[port] = deviceId
                    if (port == 1 && !pad2Enabled) { pad2Enabled = true; onPlayer2Joined?.invoke() }
                    return port
                }
            }
        }
        for (i in 0 until maxSlots) {
            // A pinned slot belongs to its controller even while that controller is away;
            // handing it to the next pad to press a button is what the pin exists to prevent.
            if (pinned.containsValue(i)) continue
            if (slots[i] == -1) {
                slots[i] = deviceId
                if (i == 1 && !pad2Enabled) { pad2Enabled = true; onPlayer2Joined?.invoke() }
                return i
            }
        }
        return 0 // all slots taken -> fold into Player 1
    }
}
