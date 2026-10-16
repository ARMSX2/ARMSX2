package com.armsx2.remoteplay

import org.webrtc.PeerConnection
import org.webrtc.RTCStats
import org.webrtc.RTCStatsReport
import java.util.Locale
import kotlin.math.roundToInt

/**
 * One line of what the stream is doing, from WebRTC's own statistics: codec and which encoder or
 * decoder runs it, picture size, frame rate, bitrate and round trip, plus why quality is limited
 * (host) or how long frames wait and take to decode (viewer). The host logs it; the viewer shows
 * it when its stats are on.
 *
 * Times and rates cover the time since the previous sample, not the whole session, so a slow
 * start does not hang over the numbers for minutes.
 */
internal class RemotePlayStats(private val sending: Boolean) {
    private var last: Map<String, Double> = emptyMap()
    private var lastTimeUs = 0.0

    /** Any thread; [onLine] runs on WebRTC's thread. */
    fun sample(peer: PeerConnection, onLine: (String) -> Unit) {
        runCatching { peer.getStats { report -> onLine(describe(report)) } }
    }

    @Synchronized
    private fun describe(report: RTCStatsReport): String {
        val all = report.statsMap.values
        val rtp = all.firstOrNull { it.type == (if (sending) "outbound-rtp" else "inbound-rtp") && it.members["kind"] == "video" }
            ?: return "no video yet"
        val m = rtp.members

        val codec = (m["codecId"] as? String)?.let { report.statsMap[it] }?.members?.get("mimeType")?.toString()?.substringAfter('/') ?: "?"
        val implementation = m[if (sending) "encoderImplementation" else "decoderImplementation"]?.toString()
        val width = (m["frameWidth"] as? Number)?.toInt() ?: 0
        val height = (m["frameHeight"] as? Number)?.toInt() ?: 0
        val fps = (m["framesPerSecond"] as? Number)?.toDouble() ?: 0.0

        // Running totals, kept for the next sample.
        val totals = listOf(
            "bytesSent", "bytesReceived", "totalDecodeTime", "framesDecoded", "framesReceived",
            "jitterBufferDelay", "jitterBufferEmittedCount", "framesDropped", "packetsLost",
            "nackCount", "pliCount", "keyFramesDecoded",
        ).mapNotNull { key -> (m[key] as? Number)?.toDouble()?.let { key to it } }.toMap()
        val seconds = if (lastTimeUs > 0 && rtp.timestampUs > lastTimeUs) (rtp.timestampUs - lastTimeUs) / 1e6 else 0.0
        fun delta(key: String): Double? {
            val now = totals[key] ?: return null
            val before = last[key] ?: return null
            return (now - before).takeIf { seconds > 0 && it >= 0 }
        }
        // A running total in seconds over a frame count, as milliseconds per frame in this window.
        fun perFrameMs(total: String, count: String): Double? {
            val frames = delta(count)?.takeIf { it > 0 } ?: return null
            return delta(total)?.let { it * 1000 / frames }
        }

        val bytes = delta(if (sending) "bytesSent" else "bytesReceived")
        val mbps = if (bytes != null) bytes * 8 / seconds / 1e6 else 0.0

        val pair = all.firstOrNull { it.type == "candidate-pair" && it.members["nominated"] == true && it.members["state"] == "succeeded" }
        val rttMs = (pair?.members?.get("currentRoundTripTime") as? Number)?.toDouble()?.times(1000)

        val parts = mutableListOf(
            if (implementation.isNullOrBlank()) codec else "$codec ($implementation)",
            "${width}x$height",
            String.format(Locale.US, "%.0f fps", fps),
        )
        // Frames that arrived, beside the frames decoded: a dip in both started before this device.
        if (!sending) delta("framesReceived")?.let { parts += String.format(Locale.US, "recv %.0f", it / seconds) }
        parts += String.format(Locale.US, "%.1f Mbps", mbps)
        rttMs?.let { parts += String.format(Locale.US, "rtt %.0f ms", it) }
        if (sending) {
            m["qualityLimitationReason"]?.toString()?.takeIf { it != "none" }?.let { parts += "limited by $it" }
            (pair?.members?.get("availableOutgoingBitrate") as? Number)?.let { parts += String.format(Locale.US, "bwe %.1f Mbps", it.toDouble() / 1e6) }
            (m["targetBitrate"] as? Number)?.let { parts += String.format(Locale.US, "target %.1f Mbps", it.toDouble() / 1e6) }
        } else {
            perFrameMs("jitterBufferDelay", "jitterBufferEmittedCount")?.let { parts += String.format(Locale.US, "buffer %.0f ms", it) }
            perFrameMs("totalDecodeTime", "framesDecoded")?.let { parts += String.format(Locale.US, "decode %.1f ms", it) }
            delta("framesDropped")?.takeIf { it > 0 }?.let { parts += String.format(Locale.US, "dropped %.0f", it) }
            delta("packetsLost")?.takeIf { it > 0 }?.let { parts += String.format(Locale.US, "lost %.0f", it) }
            // Key frames in the last second: a large frame is one of these, or a busy ordinary one.
            delta("keyFramesDecoded")?.takeIf { it > 0 }?.let { parts += String.format(Locale.US, "key %.0f", it) }
            // How unevenly packets arrive (RFC 3550 interarrival jitter, smoothed).
            (m["jitter"] as? Number)?.let { parts += String.format(Locale.US, "jitter %.0f ms", it.toDouble() * 1000) }
        }
        // Lost packets asked for again (NACK) and new key frames asked for (PLI): what recovering
        // from losses cost, sent by the viewer, received by the host.
        delta("nackCount")?.takeIf { it > 0 }?.let { parts += String.format(Locale.US, "nack %.0f", it) }
        delta("pliCount")?.takeIf { it > 0 }?.let { parts += String.format(Locale.US, "pli %.0f", it) }
        // How the devices reach each other, by the host address WebRTC settled on (viewer side).
        if (!sending) route(report, pair)?.let { parts += it }

        last = totals
        lastTimeUs = rtp.timestampUs
        return parts.joinToString(" · ")
    }

    /**
     * The host's address in use, in words: on the local network, across the internet through the
     * port the host's router opened for the session (UPnP; RemotePlayUpnp marks that address with
     * a foundation starting "u"), or across the internet otherwise. Null before there is a route.
     */
    private fun route(report: RTCStatsReport, pair: RTCStats?): String? {
        val id = pair?.members?.get("remoteCandidateId")?.toString() ?: return null
        val remote = report.statsMap[id]?.members ?: return null
        return when (remote["candidateType"]?.toString()) {
            "host" -> "via local network"
            "srflx", "prflx" ->
                if (remote["foundation"]?.toString()?.startsWith("u") == true) "via internet, router port (UPnP)" else "via internet"
            "relay" -> "via relay"
            else -> null
        }
    }
}

/**
 * The host's own WebRTC numbers for [RemotePlayProtocol.HostStats], per second: frames sent, the
 * bandwidth estimate to the viewer and the bitrate asked of the encoder, and the viewer's requests
 * to resend lost packets (NACK) and for a new key frame (PLI). Frames encoded but not sent, or a
 * low estimate that holds the encoder back, show up here and nowhere else.
 */
internal class HostRtcSampler {
    class Sample(val sentFps: Int, val bweKbps: Int, val targetKbps: Int, val nacks: Int, val plis: Int, val pacerMs: Int)

    private var last: Map<String, Double> = emptyMap()
    private var lastTimeUs = 0.0

    /** The newest sample, about a second old; null until there are two to compare. */
    @Volatile
    var latest: Sample? = null
        private set

    /** Any thread; [latest] is updated on WebRTC's thread. */
    fun sample(peer: PeerConnection) {
        runCatching { peer.getStats { report -> update(report) } }
    }

    @Synchronized
    private fun update(report: RTCStatsReport) {
        val all = report.statsMap.values
        val rtp = all.firstOrNull { it.type == "outbound-rtp" && it.members["kind"] == "video" } ?: return
        val m = rtp.members
        val totals = listOf("framesSent", "nackCount", "pliCount", "totalPacketSendDelay", "packetsSent")
            .mapNotNull { key -> (m[key] as? Number)?.toDouble()?.let { key to it } }.toMap()
        val seconds = if (lastTimeUs > 0 && rtp.timestampUs > lastTimeUs) (rtp.timestampUs - lastTimeUs) / 1e6 else 0.0
        fun delta(key: String): Double = ((totals[key] ?: 0.0) - (last[key] ?: 0.0)).coerceAtLeast(0.0)
        if (seconds > 0) {
            val pair = all.firstOrNull { it.type == "candidate-pair" && it.members["nominated"] == true && it.members["state"] == "succeeded" }
            val bwe = (pair?.members?.get("availableOutgoingBitrate") as? Number)?.toDouble() ?: 0.0
            val target = (m["targetBitrate"] as? Number)?.toDouble() ?: 0.0
            latest = Sample(
                sentFps = (delta("framesSent") / seconds).roundToInt(),
                bweKbps = (bwe / 1000).roundToInt(),
                targetKbps = (target / 1000).roundToInt(),
                nacks = delta("nackCount").roundToInt(),
                plis = delta("pliCount").roundToInt(),
                // Seconds summed over packets: the average wait of a packet in the last second.
                pacerMs = delta("packetsSent").takeIf { it > 0 }?.let { (delta("totalPacketSendDelay") * 1000 / it).roundToInt() } ?: 0,
            )
        }
        last = totals
        lastTimeUs = rtp.timestampUs
    }
}

/** The host's side of the stream ([RemotePlayProtocol.HostStats]) as one line of the stats. */
internal fun describeHost(stats: RemotePlayProtocol.HostStats): String {
    val parts = mutableListOf(
        "host: game ${stats.gameFps} fps",
        "captured ${stats.capturedFps}",
        "encoded ${stats.encodedFps}",
    )
    // WebRTC's side, from a host that reports it: what went out, and the bitrate it allowed.
    if (stats.bweKbps > 0 || stats.sentFps > 0) {
        parts += "sent ${stats.sentFps}"
        parts += String.format(Locale.US, "bwe %.1f Mbps", stats.bweKbps / 1000.0)
        parts += String.format(Locale.US, "target %.1f Mbps", stats.targetKbps / 1000.0)
        if (stats.nacks > 0) parts += "nack ${stats.nacks}"
        if (stats.plis > 0) parts += "pli ${stats.plis}"
        // Where a stall starts on the host: the game or capture (frames handed late), or WebRTC
        // holding packets back to its bitrate (pacer wait per packet).
        parts += "capture gap max ${stats.captureGapMs} ms"
        parts += "pacer ${stats.pacerMs} ms"
    }
    val dropped = listOf(
        "gpu busy" to stats.droppedGpu,
        "late" to stats.droppedLate,
        "encoder" to stats.droppedEncoder,
        "webrtc" to stats.droppedWebrtc,
    ).filter { it.second > 0 }
    if (dropped.isNotEmpty()) parts += "dropped " + dropped.joinToString(", ") { "${it.second} ${it.first}" }
    // The slowest frame from capture to out of the encoder, and the largest, in the last second.
    parts += "encode max ${stats.peakEncodeMs} ms"
    parts += "frame max ${stats.peakFrameKb} KB"
    if (stats.encoder.isNotEmpty()) parts += stats.encoder
    if (stats.wifiMhz > 0) parts += WifiLink(stats.wifiMhz, stats.wifiTxMbps, stats.wifiRxMbps, stats.wifiRssi).describe()
    return parts.joinToString(" · ")
}
