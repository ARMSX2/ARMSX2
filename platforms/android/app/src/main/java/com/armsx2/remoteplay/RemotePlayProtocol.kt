// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.view.KeyEvent
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Base64
import java.util.zip.Deflater
import java.util.zip.Inflater

/**
 * Remote play: the host's running game streamed to a friend's device, where the friend plays as
 * player 2. Only the host emulates; the viewer shows video, plays audio and sends its pad.
 *
 * Wire format, shared by [RemotePlayHost] and [RemotePlayViewerActivity]:
 *  - Signaling, two ways. On the same Wi-Fi: newline-delimited JSON over TCP on [PORT],
 *    advertised over DNS-SD as [SERVICE_TYPE] so a viewer finds the host without typing anything.
 *    Online: the same offer and answer as connection codes ([encodeCode]) that the players send
 *    each other over any chat, so no server is involved. Messages: hello (viewer, Wi-Fi only),
 *    offer (host), answer (viewer), bye. Offers and answers carry every ICE candidate (no
 *    trickle), since a pasted code is a single message.
 *  - Video: a WebRTC video track, host to viewer.
 *  - Audio and player 2's pad: WebRTC data channels, unordered and never retransmitted, so a lost
 *    packet is skipped rather than delaying everything behind it. The game's audio does not go
 *    through WebRTC's audio stack, which is built around the microphone. Online it is Opus when
 *    the viewer's answer says it plays it ("audio": "opus"), each packet carrying the one before
 *    as well ([encodeOpus]); on the same Wi-Fi, or with no Opus encoder on the host, plain 16-bit
 *    samples ([encodeAudio]). Once a second the audio channel also carries the host's side of the
 *    stream stats ([HostStats]), and every 200 ms player 2's rumble ([encodeRumble]).
 */
internal object RemotePlayProtocol {
    const val VERSION = 1
    const val PORT = 38433
    const val SERVICE_TYPE = "_armsx2rp._tcp"

    const val CHANNEL_INPUT = "input"
    const val CHANNEL_AUDIO = "audio"

    /** The stream is at most this size; the presenter scales larger frames down on the GPU. */
    const val MAX_WIDTH = 1280
    const val MAX_HEIGHT = 720

    /** Video bitrate bounds in bits per second. WebRTC moves between them as the network allows. */
    class Bitrate(val min: Int, val start: Int, val max: Int)

    /**
     * Same Wi-Fi: a fixed 10 Mbps, as Moonlight streams on a home network. Left to adapt, WebRTC's
     * estimate sagged while the picture was still and the encoder's target moved with it, so frame
     * sizes swung and the first frames after standing still came out large. A home Wi-Fi link
     * carries 10 Mbps many times over.
     */
    val WIFI_BITRATE = Bitrate(min = 10_000_000, start = 10_000_000, max = 10_000_000)

    /** Online: the host's upload decides, so start where most connections cope. */
    val ONLINE_BITRATE = Bitrate(min = 1_000_000, start = 4_000_000, max = 12_000_000)

    const val SAMPLE_RATE = 48_000
    const val AUDIO_CHUNK_FRAMES = 480 // 10 ms

    /**
     * Online audio: stereo Opus at this rate, against 1.5 Mbps for plain samples. Every packet
     * also carries the packet before it, so it costs twice this on the wire, still about a sixth.
     */
    const val OPUS_BITRATE = 128_000

    // The most one 20 ms Opus packet can hold: a frame's 1,275 bytes and its table of contents.
    private const val MAX_OPUS_FRAME = 1_276

    private const val PACKET_PAD: Byte = 1
    private const val PACKET_AUDIO: Byte = 2
    private const val PACKET_BYE: Byte = 3
    private const val PACKET_HOST_STATS: Byte = 4
    private const val PACKET_AUDIO_OPUS: Byte = 5
    private const val PACKET_RUMBLE: Byte = 6
    private const val PACKET_WAITING: Byte = 7
    private const val RUMBLE_PACKET_SIZE = 8

    // Why a session ended, in the second byte of a bye.
    private const val BYE_ENDED = 0
    private const val BYE_DENIED = 1
    private const val BYE_NO_ANSWER = 2
    private const val ZERO: Byte = 0
    private const val PAD_PACKET_SIZE = 16
    const val AUDIO_HEADER_SIZE = 8

    // Player 2's buttons, one bit each, in the order of PAD_KEYCODES. L2 and R2 travel as analog
    // bytes in PadState instead.
    const val PAD_UP = 1 shl 0
    const val PAD_RIGHT = 1 shl 1
    const val PAD_DOWN = 1 shl 2
    const val PAD_LEFT = 1 shl 3
    const val PAD_SELECT = 1 shl 4
    const val PAD_START = 1 shl 5
    const val PAD_L3 = 1 shl 6
    const val PAD_R3 = 1 shl 7
    const val PAD_TRIANGLE = 1 shl 8
    const val PAD_CIRCLE = 1 shl 9
    const val PAD_CROSS = 1 shl 10
    const val PAD_SQUARE = 1 shl 11
    const val PAD_L1 = 1 shl 12
    const val PAD_R1 = 1 shl 13

    /** The Android keycodes RemotePlayCore.setPadButton takes for each bit, by bit index. */
    val PAD_KEYCODES = intArrayOf(
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_RIGHT,
        KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_THUMBL,
        KeyEvent.KEYCODE_BUTTON_THUMBR,
        KeyEvent.KEYCODE_BUTTON_Y,
        KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_A,
        KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_L1,
        KeyEvent.KEYCODE_BUTTON_R1,
    )

    /**
     * Player 2's whole pad. Sent whole on every change and as a heartbeat, so a lost packet is
     * corrected by the next one. Sticks are 0..255 with 128 at rest, the DualShock's own range.
     */
    data class PadState(
        val buttons: Int = 0,
        val lx: Int = 128,
        val ly: Int = 128,
        val rx: Int = 128,
        val ry: Int = 128,
        val l2: Int = 0,
        val r2: Int = 0,
    )

    fun encodePad(seq: Int, pad: PadState): ByteBuffer {
        val out = ByteBuffer.allocate(PAD_PACKET_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        out.put(PACKET_PAD)
        out.putInt(seq)
        out.putInt(pad.buttons)
        out.put(pad.lx.toByte()).put(pad.ly.toByte()).put(pad.rx.toByte()).put(pad.ry.toByte())
        out.put(pad.l2.toByte()).put(pad.r2.toByte()).put(ZERO)
        out.flip()
        return out
    }

    fun decodePad(data: ByteBuffer): Pair<Int, PadState>? {
        val buf = data.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() < PAD_PACKET_SIZE || buf.get() != PACKET_PAD) return null
        val seq = buf.int
        val buttons = buf.int
        fun u8() = buf.get().toInt() and 0xff
        return seq to PadState(buttons, u8(), u8(), u8(), u8(), u8(), u8())
    }

    /** An audio packet: header, then [frames] stereo s16 frames copied from [pcm]. */
    fun encodeAudio(seq: Int, pcm: ByteBuffer, frames: Int): ByteBuffer {
        val bytes = frames * 4
        val out = ByteBuffer.allocate(AUDIO_HEADER_SIZE + bytes).order(ByteOrder.LITTLE_ENDIAN)
        out.put(PACKET_AUDIO).put(ZERO).put(ZERO).put(ZERO)
        out.putInt(seq)
        val src = pcm.duplicate()
        src.position(0).limit(bytes)
        out.put(src)
        out.flip()
        return out
    }

    fun decodeAudio(data: ByteBuffer): ByteArray? {
        val buf = data.duplicate()
        if (buf.remaining() <= AUDIO_HEADER_SIZE || buf.get() != PACKET_AUDIO) return null
        buf.position(buf.position() - 1 + AUDIO_HEADER_SIZE)
        val pcm = ByteArray(buf.remaining() and 3.inv())
        buf.get(pcm)
        return pcm
    }

    /** One Opus packet ([seq]) and, when there was one, the packet before it. */
    class OpusPacket(val seq: Int, val frame: ByteArray, val previous: ByteArray?)

    /**
     * A compressed audio packet: header (type, 0, the frame's length, sequence number), the Opus
     * frame, then the frame before it again. The channel never resends, so a packet that goes
     * missing is played from the copy in the next one instead.
     */
    fun encodeOpus(seq: Int, frame: ByteArray, previous: ByteArray?): ByteBuffer {
        val out = ByteBuffer.allocate(AUDIO_HEADER_SIZE + frame.size + (previous?.size ?: 0)).order(ByteOrder.LITTLE_ENDIAN)
        out.put(PACKET_AUDIO_OPUS).put(ZERO)
        out.putShort(frame.size.toShort())
        out.putInt(seq)
        out.put(frame)
        previous?.let(out::put)
        out.flip()
        return out
    }

    /** An Opus packet, or null for anything else, or for frames bigger than Opus makes. */
    fun decodeOpus(data: ByteBuffer): OpusPacket? {
        val buf = data.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() <= AUDIO_HEADER_SIZE || buf.get() != PACKET_AUDIO_OPUS) return null
        buf.get()
        val length = buf.short.toInt() and 0xffff
        val seq = buf.int
        if (length == 0 || length > MAX_OPUS_FRAME || length > buf.remaining()) return null
        val frame = ByteArray(length).also { buf.get(it) }
        val rest = buf.remaining()
        val previous = if (rest in 1..MAX_OPUS_FRAME) ByteArray(rest).also { buf.get(it) } else null
        return OpusPacket(seq, frame, previous)
    }

    /** Player 2's motors, 0..255 each, as the game drives them on the host, and whether the host paused. */
    data class Rumble(val seq: Int, val large: Int, val small: Int, val paused: Boolean)

    private const val FLAG_PAUSED = 1

    /**
     * Player 2's rumble, and the game's state with it: type, the large and small motor (0..255),
     * flags (1: the host paused the game), sequence number. Sent on every change and every 200 ms
     * besides, so a lost packet is corrected by the next.
     */
    fun encodeRumble(seq: Int, large: Int, small: Int, paused: Boolean): ByteBuffer {
        val out = ByteBuffer.allocate(RUMBLE_PACKET_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        out.put(PACKET_RUMBLE).put(large.toByte()).put(small.toByte()).put(if (paused) FLAG_PAUSED.toByte() else ZERO)
        out.putInt(seq)
        out.flip()
        return out
    }

    fun decodeRumble(data: ByteBuffer): Rumble? {
        val buf = data.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() < RUMBLE_PACKET_SIZE || buf.get() != PACKET_RUMBLE) return null
        val large = buf.get().toInt() and 0xff
        val small = buf.get().toInt() and 0xff
        val flags = buf.get().toInt() and 0xff
        return Rumble(buf.int, large, small, flags and FLAG_PAUSED != 0)
    }

    /**
     * Sent a few times on the way out, on the data channels, so the other side ends at once
     * instead of waiting for the connection to time out. A pasted-code session has no other way.
     */
    fun encodeBye(reason: Int = BYE_ENDED): ByteBuffer {
        val out = ByteBuffer.allocate(4)
        out.put(PACKET_BYE).put(reason.toByte()).put(ZERO).put(ZERO)
        out.flip()
        return out
    }

    fun isBye(data: ByteBuffer): Boolean = data.remaining() >= 1 && data.get(data.position()) == PACKET_BYE

    fun byeCode(reason: String): Int = when (reason) {
        "denied" -> BYE_DENIED
        "noanswer" -> BYE_NO_ANSWER
        else -> BYE_ENDED
    }

    /** The reason [byeCode] put in a bye, as the signalling bye names it: "denied", "noanswer" or "stopped". */
    fun byeReason(data: ByteBuffer): String = when (if (data.remaining() >= 2) data.get(data.position() + 1).toInt() else BYE_ENDED) {
        BYE_DENIED -> "denied"
        BYE_NO_ANSWER -> "noanswer"
        else -> "stopped"
    }

    /** The host is being asked whether to let this viewer in. Sent until it answers. */
    fun encodeWaiting(): ByteBuffer {
        val out = ByteBuffer.allocate(4)
        out.put(PACKET_WAITING).put(ZERO).put(ZERO).put(ZERO)
        out.flip()
        return out
    }

    fun isWaiting(data: ByteBuffer): Boolean = data.remaining() >= 1 && data.get(data.position()) == PACKET_WAITING

    /**
     * The host's side of the stream over the last second, for the viewer's stats. The dropped*
     * counts say where frames were lost: GPU still busy with every capture slot, replaced by a
     * newer frame before the app took them, refused by the encoder, skipped inside WebRTC.
     * peakEncodeMs is the slowest frame from capture to encoder output. sentFps through plis come
     * from WebRTC ([HostRtcSampler]; nacks are the viewer's resend requests, plis its key-frame
     * requests), the wifi fields from the host's link ([WifiLink]): MHz, Mbps, and wifiRssi in dBm
     * (negative). Zero from a host that does not send a field.
     */
    data class HostStats(
        val gameFps: Int,
        val capturedFps: Int,
        val encodedFps: Int,
        val droppedGpu: Int,
        val droppedLate: Int,
        val droppedEncoder: Int,
        val droppedWebrtc: Int,
        val peakEncodeMs: Int,
        val peakFrameKb: Int,
        val encoder: String,
        val sentFps: Int = 0,
        val bweKbps: Int = 0,
        val targetKbps: Int = 0,
        val nacks: Int = 0,
        val plis: Int = 0,
        val wifiMhz: Int = 0,
        val wifiTxMbps: Int = 0,
        val wifiRxMbps: Int = 0,
        val wifiRssi: Int = 0,
        val captureGapMs: Int = 0,
        val pacerMs: Int = 0,
    )

    // Type, layout version, two spare bytes, then 16-bit values: nine, fourteen from version 1,
    // eighteen from version 2, twenty from version 3.
    private const val HOST_STATS_HEADER_SIZE = 22
    private const val HOST_STATS_V1_HEADER_SIZE = 32
    private const val HOST_STATS_V2_HEADER_SIZE = 40
    private const val HOST_STATS_V3_HEADER_SIZE = 44
    private const val HOST_STATS_VERSION: Byte = 3
    private const val MAX_ENCODER_NAME_BYTES = 48

    fun encodeHostStats(stats: HostStats): ByteBuffer {
        val name = stats.encoder.toByteArray(Charsets.UTF_8).take(MAX_ENCODER_NAME_BYTES).toByteArray()
        val out = ByteBuffer.allocate(HOST_STATS_V3_HEADER_SIZE + name.size).order(ByteOrder.LITTLE_ENDIAN)
        out.put(PACKET_HOST_STATS).put(HOST_STATS_VERSION).put(ZERO).put(ZERO)
        val values = intArrayOf(
            stats.gameFps, stats.capturedFps, stats.encodedFps,
            stats.droppedGpu, stats.droppedLate, stats.droppedEncoder, stats.droppedWebrtc,
            stats.peakEncodeMs, stats.peakFrameKb,
            stats.sentFps, stats.bweKbps, stats.targetKbps, stats.nacks, stats.plis,
            stats.wifiMhz, stats.wifiTxMbps, stats.wifiRxMbps, -stats.wifiRssi,
            stats.captureGapMs, stats.pacerMs,
        )
        for (value in values) {
            out.putShort(value.coerceIn(0, 0xffff).toShort())
        }
        out.put(name)
        out.flip()
        return out
    }

    fun decodeHostStats(data: ByteBuffer): HostStats? {
        val buf = data.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() < HOST_STATS_HEADER_SIZE || buf.get() != PACKET_HOST_STATS) return null
        val version = buf.get()
        val size = when {
            version >= 3 -> HOST_STATS_V3_HEADER_SIZE
            version >= 2 -> HOST_STATS_V2_HEADER_SIZE
            version >= 1 -> HOST_STATS_V1_HEADER_SIZE
            else -> HOST_STATS_HEADER_SIZE
        }
        if (buf.remaining() < size - 2) return null
        buf.position(buf.position() + 2)
        fun u16() = buf.short.toInt() and 0xffff
        val gameFps = u16()
        val capturedFps = u16()
        val encodedFps = u16()
        val droppedGpu = u16()
        val droppedLate = u16()
        val droppedEncoder = u16()
        val droppedWebrtc = u16()
        val peakEncodeMs = u16()
        val peakFrameKb = u16()
        val rtc = if (version >= 1) IntArray(5) { u16() } else IntArray(5)
        val link = if (version >= 2) IntArray(4) { u16() } else IntArray(4)
        val timing = if (version >= 3) IntArray(2) { u16() } else IntArray(2)
        val name = ByteArray(minOf(buf.remaining(), MAX_ENCODER_NAME_BYTES))
        buf.get(name)
        return HostStats(
            gameFps, capturedFps, encodedFps, droppedGpu, droppedLate, droppedEncoder, droppedWebrtc,
            peakEncodeMs, peakFrameKb, String(name, Charsets.UTF_8),
            sentFps = rtc[0], bweKbps = rtc[1], targetKbps = rtc[2], nacks = rtc[3], plis = rtc[4],
            wifiMhz = link[0], wifiTxMbps = link[1], wifiRxMbps = link[2], wifiRssi = -link[3],
            captureGapMs = timing[0], pacerMs = timing[1],
        )
    }

    private const val CODE_PREFIX = "ARMSX2-RP1."
    private val CODE_PATTERN = Regex("ARMSX2-RP1\\.[A-Za-z0-9_-]+")
    private const val MAX_CODE_JSON = 64 * 1024

    // Typical SDP text, as it appears JSON-escaped inside a code, used as the deflate dictionary:
    // most of an offer is boilerplate both sides already know, so only the session's own values
    // (credentials, fingerprint, addresses) cost space. Both ends run the same version, so the
    // dictionary matches.
    private val CODE_DICTIONARY: ByteArray = JSONObject()
        .put("t", "offer")
        .put("v", VERSION)
        .put("sdp", listOf(
            "v=0", "o=- 0 2 IN IP4 127.0.0.1", "s=-", "t=0 0", "a=group:BUNDLE 0 1", "a=extmap-allow-mixed",
            "a=msid-semantic: WMS armsx2", "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101 102 103",
            "c=IN IP4 0.0.0.0", "a=rtcp:9 IN IP4 0.0.0.0",
            "a=candidate:1 1 udp 2122260223 192.168.1.2 50000 typ host generation 0 network-id 1 network-cost 10",
            "a=candidate:2 1 udp 1686052607 1.2.3.4 50000 typ srflx raddr 192.168.1.2 rport 50000 generation 0 network-id 1 network-cost 10",
            "a=candidate:3 1 udp 2122129151 fe80::1 50000 typ host generation 0 network-id 2 network-cost 10",
            "a=ice-ufrag:", "a=ice-pwd:", "a=ice-options:trickle renomination", "a=fingerprint:sha-256 ",
            "a=setup:actpass", "a=setup:active", "a=mid:0", "a=mid:1",
            "a=extmap:1 urn:ietf:params:rtp-hdrext:toffset",
            "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time",
            "a=extmap:3 urn:3gpp:video-orientation",
            "a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",
            "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay",
            "a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type",
            "a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing",
            "a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space",
            "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid",
            "a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id",
            "a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id",
            "a=sendonly", "a=recvonly", "a=msid:armsx2 game", "a=rtcp-mux", "a=rtcp-rsize",
            "a=rtpmap:96 H264/90000", "a=rtcp-fb:96 goog-remb", "a=rtcp-fb:96 transport-cc",
            "a=rtcp-fb:96 ccm fir", "a=rtcp-fb:96 nack", "a=rtcp-fb:96 nack pli",
            "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
            "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f",
            "a=rtpmap:97 rtx/90000", "a=fmtp:97 apt=96", "a=rtpmap:100 VP8/90000",
            "a=rtpmap:102 red/90000", "a=rtpmap:103 ulpfec/90000", "a=ssrc-group:FID ",
            "a=ssrc:1 cname:", "a=ssrc:1 msid:armsx2 game",
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel", "a=sctp-port:5000",
            "a=max-message-size:262144", "",
        ).joinToString("\r\n"))
        .put("game", "")
        .put("host", "")
        .put("name", "")
        .toString()
        .toByteArray(Charsets.UTF_8)

    fun encodeCode(message: JSONObject): String {
        val deflater = Deflater(Deflater.BEST_COMPRESSION)
        deflater.setDictionary(CODE_DICTIONARY)
        deflater.setInput(message.toString().toByteArray(Charsets.UTF_8))
        deflater.finish()
        val out = ByteArrayOutputStream()
        val chunk = ByteArray(4096)
        while (!deflater.finished()) {
            out.write(chunk, 0, deflater.deflate(chunk))
        }
        deflater.end()
        return CODE_PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(out.toByteArray())
    }

    /**
     * The message inside a code, or null. [text] can be the whole chat message the code arrived
     * in: the code is found wherever it is.
     */
    fun decodeCode(text: String): JSONObject? {
        val token = CODE_PATTERN.find(text)?.value ?: return null
        val bytes = runCatching {
            Base64.getUrlDecoder().decode(token.removePrefix(CODE_PREFIX))
        }.getOrNull() ?: return null

        val inflater = Inflater()
        val out = ByteArrayOutputStream()
        try {
            inflater.setInput(bytes)
            val chunk = ByteArray(4096)
            while (!inflater.finished()) {
                val count = inflater.inflate(chunk)
                if (count > 0) {
                    out.write(chunk, 0, count)
                    if (out.size() > MAX_CODE_JSON) return null
                } else if (inflater.needsDictionary()) {
                    inflater.setDictionary(CODE_DICTIONARY)
                } else if (inflater.needsInput()) {
                    return null
                }
            }
        } catch (_: java.util.zip.DataFormatException) {
            return null
        } catch (_: RuntimeException) {
            // setDictionary throws IllegalArgumentException for a code made with another
            // dictionary, which is what any other version's code is. Treat it as not a code; it
            // must not take the app down, and on a host the running game with it.
            return null
        } finally {
            inflater.end()
        }
        return runCatching { JSONObject(out.toString(Charsets.UTF_8.name())) }.getOrNull()
    }

    /** A stick axis in [-1, 1] as a DualShock stick byte: 0 at full up/left, 255 at full down/right. */
    fun axisByte(value: Float): Int {
        val v = value.coerceIn(-1f, 1f)
        return (128.5f + v * (if (v < 0f) 128f else 127f)).toInt().coerceIn(0, 255)
    }
}
