// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.util.Log
import kr.co.iefriends.pcsx2.NativeApp
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

private const val TAG = "RemotePlay"

/**
 * Online hosting: the UDP port WebRTC listens on, opened on the host's router (UPnP) for the
 * session, so a friend whose own network lets nothing in (a phone on mobile data, say) can still
 * reach the host directly. The router's public address for it goes into the offer as one more
 * candidate. Not a relay: the router only forwards the friend's packets to the host. With no router
 * that speaks UPnP, or one behind a second NAT, the offer goes out as it was. One per session.
 */
internal class RemotePlayUpnp {
    private val found = CountDownLatch(1)
    private val ended = CountDownLatch(1)

    @Volatile
    private var lanAddress: String? = null

    // Router port for each local port opened this session. Guarded by this.
    private val opened = LinkedHashMap<Int, Int>()

    /** Starts looking for the router (about two seconds) while WebRTC gathers its candidates. */
    fun start() {
        thread(name = "rp-upnp", isDaemon = true) {
            lanAddress = runCatching { NativeApp.remotePlayUpnpPrepare() }.getOrNull()
            found.countDown()
        }
    }

    /**
     * Background thread: [sdp] with a router candidate after each of its UDP host candidates on the
     * router's network, or [sdp] as it was when there is no router, it refused, or the search is
     * still going after [waitMs].
     */
    fun addCandidates(sdp: String, waitMs: Long): String {
        if (!found.await(waitMs, TimeUnit.MILLISECONDS)) return sdp
        val lan = lanAddress ?: return sdp

        var added = 0
        val lines = sdp.split("\r\n").flatMap { line ->
            val candidate = routerCandidate(line, lan)
            if (candidate != null) added++
            listOfNotNull(line, candidate)
        }
        if (added == 0) return sdp
        Log.i(TAG, "remote play: offer carries the router's public address ($added candidate(s))")
        keepOpen()
        return lines.joinToString("\r\n")
    }

    /**
     * For "a=candidate:F 1 udp P <lan> <port> typ host ...", the router's public address for that
     * port as a server-reflexive candidate, the kind STUN finds, after opening the port; else null.
     */
    private fun routerCandidate(line: String, lan: String): String? {
        if (!line.startsWith(CANDIDATE)) return null
        val fields = line.removePrefix(CANDIDATE).split(' ')
        if (fields.size < 8 || fields[1] != "1" || !fields[2].equals("udp", ignoreCase = true)) return null
        if (fields[4] != lan || fields[6] != "typ" || fields[7] != "host") return null
        val port = fields[5].toIntOrNull() ?: return null
        val hostPriority = fields[3].toLongOrNull() ?: return null

        val public = open(port) ?: return null
        val colon = public.lastIndexOf(':')
        val address = public.substring(0, colon)
        val publicPort = public.substring(colon + 1)

        // Server-reflexive type preference (100), the host candidate's local preference, component 1.
        val priority = (100L shl 24) or (((hostPriority shr 8) and 0xffff) shl 8) or 255L
        val extensions = fields.drop(8).joinToString(" ")
        return "${CANDIDATE}u${fields[0]} 1 udp $priority $address $publicPort typ srflx raddr $lan rport $port $extensions".trimEnd()
    }

    /** The router's "public address:port" for local [port], or null. */
    private fun open(port: Int): String? {
        val public = runCatching { NativeApp.remotePlayUpnpMap(port, 0, LEASE_SECONDS) }.getOrNull() ?: return null
        val publicPort = public.substringAfterLast(':').toIntOrNull() ?: return null
        // The session may have ended while the router answered: then the port closes again at once.
        val kept = synchronized(this) {
            (ended.count > 0).also { if (it) opened[port] = publicPort }
        }
        if (!kept) {
            runCatching { NativeApp.remotePlayUpnpUnmap(publicPort) }
            return null
        }
        return public
    }

    // Renews the lease while the session lasts. A port left open by a crash closes with its lease.
    private fun keepOpen() {
        thread(name = "rp-upnp-lease", isDaemon = true) {
            while (!ended.await(RENEW_MS, TimeUnit.MILLISECONDS)) {
                val ports = synchronized(this) { opened.toMap() }
                for ((port, publicPort) in ports) {
                    runCatching { NativeApp.remotePlayUpnpMap(port, publicPort, LEASE_SECONDS) }
                }
            }
        }
    }

    /** The session is over: closes what was opened, in the background. */
    fun close() {
        ended.countDown()
        val ports = synchronized(this) { opened.values.toList().also { opened.clear() } }
        if (ports.isEmpty()) return
        thread(name = "rp-upnp-close", isDaemon = true) {
            ports.forEach { runCatching { NativeApp.remotePlayUpnpUnmap(it) } }
        }
    }

    private companion object {
        const val CANDIDATE = "a=candidate:"
        const val LEASE_SECONDS = 3_600
        const val RENEW_MS = 20 * 60_000L
    }
}
