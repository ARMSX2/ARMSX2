// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.content.Context
import android.net.ConnectivityManager
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import org.json.JSONObject
import java.io.IOException
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

private const val TAG = "RemotePlay"

/**
 * One signaling link: newline-delimited JSON over a TCP socket. Reads on its own thread and
 * writes on another, so [send] is safe from any thread, the main thread included.
 *
 * This is the same-Wi-Fi transport. A lobby code over the internet only replaces how the two
 * ends find each other; the messages stay the same.
 */
internal class SignalingConnection(
    private val socket: Socket,
    private val onMessage: (JSONObject) -> Unit,
    private val onClosed: () -> Unit,
) {
    private val writer = socket.getOutputStream().bufferedWriter()
    private val sender = Executors.newSingleThreadExecutor { Thread(it, "rp-signal-write") }

    private val closed = AtomicBoolean(false)

    fun start() {
        thread(name = "rp-signal-read", isDaemon = true) {
            try {
                socket.getInputStream().bufferedReader().use { reader ->
                    while (true) {
                        val line = readLine(reader) ?: break
                        val message = runCatching { JSONObject(line) }.getOrNull() ?: continue
                        onMessage(message)
                    }
                }
            } catch (_: IOException) {
            } finally {
                close()
            }
        }
    }

    /**
     * One line, or null at the end. Anything on the Wi-Fi can connect to the port, and a line with
     * no end would otherwise grow until the app ran out of memory, taking the running game with it:
     * a line longer than any message ever sent ends the connection instead.
     */
    private fun readLine(reader: java.io.Reader): String? {
        val line = StringBuilder()
        while (true) {
            val c = reader.read()
            if (c < 0) return if (line.isEmpty()) null else line.toString()
            if (c == '\n'.code) return line.toString()
            if (line.length >= MAX_LINE) throw IOException("signaling line over $MAX_LINE characters")
            line.append(c.toChar())
        }
    }

    fun send(message: JSONObject) {
        if (closed.get()) return
        runCatching {
            sender.execute {
                try {
                    writer.write(message.toString())
                    writer.write("\n")
                    writer.flush()
                } catch (_: IOException) {
                    close()
                }
            }
        }
    }

    /** Sends [message] (a bye, typically) and then closes, in that order. */
    fun sendAndClose(message: JSONObject) {
        send(message)
        close()
    }

    fun close() {
        if (!closed.compareAndSet(false, true)) return
        // Queued behind any pending write, so a final message still goes out first.
        runCatching { sender.execute { runCatching { socket.close() } } }
        sender.shutdown()
        onClosed()
    }

    companion object {
        // Well above the largest message (an offer: a few KB), as decodeCode allows.
        private const val MAX_LINE = 64 * 1024

        /** Connects to a host. Blocking: call it off the main thread. Throws IOException. */
        fun connect(host: String, port: Int, onMessage: (JSONObject) -> Unit, onClosed: () -> Unit): SignalingConnection {
            if (port !in 1..65535) throw IOException("no such port: $port")
            val socket = Socket()
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(host, port), 5_000)
            return SignalingConnection(socket, onMessage, onClosed)
        }
    }
}

/**
 * How a session's offer, answer and bye travel. Same Wi-Fi: a [SignalingConnection]. Online: the
 * players themselves, as connection codes, so sending a message means showing its code.
 */
internal interface SignalLink {
    fun send(message: JSONObject)

    /** Ends the link, sending [last] first where the link can. */
    fun close(last: JSONObject?)
}

internal class TcpLink(private val connection: SignalingConnection) : SignalLink {
    override fun send(message: JSONObject) = connection.send(message)

    override fun close(last: JSONObject?) {
        if (last != null) connection.sendAndClose(last) else connection.close()
    }
}

/** Online, with no server: offers and answers become codes for the player to send. */
internal class CodeLink(private val show: (String) -> Unit) : SignalLink {
    override fun send(message: JSONObject) {
        when (message.optString("t")) {
            "offer", "answer" -> show(RemotePlayProtocol.encodeCode(message))
        }
    }

    // A code session ends over the data channels (RemotePlayProtocol.encodeBye).
    override fun close(last: JSONObject?) {}
}

/**
 * Online through the lobby service ([RemotePlayLobby]): the offer goes up under a six-character
 * code and the answer comes back by polling, so the players swap one short code instead of two
 * pasted ones. If the service cannot be reached, [onFallback] gets a pasted code instead, so an
 * invite still works.
 */
internal class LobbyHostLink(
    private val onCode: (String) -> Unit,
    private val onFallback: (String) -> Unit,
    private val onAnswer: (JSONObject) -> Unit,
    private val onExpired: () -> Unit,
) : SignalLink {
    @Volatile
    private var lobby: RemotePlayLobby.Lobby? = null

    @Volatile
    private var closed = false

    override fun send(message: JSONObject) {
        if (message.optString("t") != "offer" || closed) return
        thread(name = "rp-lobby", isDaemon = true) {
            val opened = RemotePlayLobby.open(message.toString())
            if (opened == null) {
                if (!closed) onFallback(RemotePlayProtocol.encodeCode(message))
                return@thread
            }
            lobby = opened
            if (closed) {
                RemotePlayLobby.close(opened)
                return@thread
            }
            onCode(opened.code)

            val opensAt = SystemClock.uptimeMillis()
            val deadline = opensAt + RemotePlayLobby.LIFETIME_MS
            while (!closed && SystemClock.uptimeMillis() < deadline) {
                try {
                    // Often while the friend is typing the code, so an answer is picked up within
                    // a second; less often once the invite has waited a while.
                    Thread.sleep(if (SystemClock.uptimeMillis() - opensAt < FAST_POLLING_MS) FAST_POLL_MS else SLOW_POLL_MS)
                } catch (_: InterruptedException) {
                    return@thread
                }
                if (closed) return@thread
                when (val result = RemotePlayLobby.collectAnswer(opened)) {
                    is RemotePlayLobby.Result.Ok -> {
                        val answer = runCatching { JSONObject(result.value) }.getOrNull()
                        if (answer != null) onAnswer(answer) else onExpired()
                        return@thread
                    }
                    RemotePlayLobby.Result.NotFound -> {
                        onExpired()
                        return@thread
                    }
                    // Still waiting, or a network blip: keep polling.
                    else -> Unit
                }
            }
            if (!closed) onExpired()
        }
    }

    override fun close(last: JSONObject?) {
        if (closed) return
        closed = true
        lobby?.let { opened -> thread(isDaemon = true) { RemotePlayLobby.close(opened) } }
    }

    private companion object {
        const val FAST_POLL_MS = 1_000L
        const val SLOW_POLL_MS = 3_000L
        const val FAST_POLLING_MS = 2 * 60_000L
    }
}

/** The friend's side of a lobby: the answer goes up to the service, where the host collects it. */
internal class LobbyViewerLink(
    private val code: String,
    private val onFailed: (RemotePlayLobby.Result) -> Unit,
) : SignalLink {
    override fun send(message: JSONObject) {
        if (message.optString("t") != "answer") return
        thread(name = "rp-lobby", isDaemon = true) {
            val result = RemotePlayLobby.answer(code, message.toString())
            if (result !is RemotePlayLobby.Result.Ok) onFailed(result)
        }
    }

    override fun close(last: JSONObject?) {}
}

/** The host's listening socket. Takes [RemotePlayProtocol.PORT] when free, any port otherwise. */
internal class SignalingServer(private val onClient: (Socket) -> Unit) {
    private var server: ServerSocket? = null

    val port: Int get() = server?.localPort ?: 0

    fun start(): Boolean {
        val socket = runCatching { ServerSocket(RemotePlayProtocol.PORT) }.getOrNull()
            ?: runCatching { ServerSocket(0) }.getOrNull()
            ?: return false
        server = socket
        thread(name = "rp-accept", isDaemon = true) {
            while (!socket.isClosed) {
                val client = try {
                    socket.accept()
                } catch (_: IOException) {
                    break
                }
                // A peer that connects and resets at once makes this throw. Uncaught on this thread
                // it took the app down, and the host's game with it: drop that peer, keep listening.
                try {
                    client.tcpNoDelay = true
                } catch (_: IOException) {
                    runCatching { client.close() }
                    continue
                }
                onClient(client)
            }
        }
        return true
    }

    fun stop() {
        runCatching { server?.close() }
        server = null
    }
}

/** A host found on this network. */
data class RemotePlayHostInfo(
    val name: String,
    val game: String,
    val address: String,
    val port: Int,
)

/** Announces the host on this network so viewers list it without typing an address. */
internal class HostAdvertiser(context: Context) {
    private val nsd = context.applicationContext.getSystemService(NsdManager::class.java)
    private var listener: NsdManager.RegistrationListener? = null

    fun register(name: String, port: Int, game: String) {
        unregister()
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = RemotePlayProtocol.SERVICE_TYPE
            this.port = port
            // TXT values are capped at 255 bytes.
            setAttribute("game", game.take(80))
            setAttribute("v", RemotePlayProtocol.VERSION.toString())
        }
        val registration = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "advertised as ${info.serviceName}")
            }

            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) {
                Log.w(TAG, "advertising failed: $error")
            }

            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {}
        }
        listener = registration
        runCatching { nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, registration) }
            .onFailure { Log.w(TAG, "advertising failed", it) }
    }

    fun unregister() {
        listener?.let { runCatching { nsd.unregisterService(it) } }
        listener = null
    }
}

/**
 * Lists hosts on this network. Results arrive on the main thread.
 *
 * Holds a multicast lock while browsing: some Wi-Fi drivers drop the multicast replies DNS-SD
 * depends on unless an app asks for them.
 */
internal class HostBrowser(context: Context, private val onChange: (List<RemotePlayHostInfo>) -> Unit) {
    private val nsd = context.applicationContext.getSystemService(NsdManager::class.java)
    private val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private val found = LinkedHashMap<String, RemotePlayHostInfo>()
    private val toResolve = ArrayDeque<NsdServiceInfo>()
    private var resolving = false
    private var discovery: NsdManager.DiscoveryListener? = null
    private var lock: WifiManager.MulticastLock? = null

    fun start() {
        if (discovery != null) return
        lock = runCatching {
            wifi?.createMulticastLock("armsx2-remote-play")?.apply {
                setReferenceCounted(false)
                acquire()
            }
        }.getOrNull()

        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, error: Int) {
                Log.w(TAG, "browsing failed: $error")
            }

            override fun onStopDiscoveryFailed(serviceType: String, error: Int) {}

            override fun onServiceFound(info: NsdServiceInfo) {
                main.post {
                    toResolve.addLast(info)
                    resolveNext()
                }
            }

            override fun onServiceLost(info: NsdServiceInfo) {
                main.post {
                    if (found.remove(info.serviceName) != null) onChange(found.values.toList())
                }
            }
        }
        discovery = listener
        runCatching { nsd.discoverServices(RemotePlayProtocol.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener) }
            .onFailure { Log.w(TAG, "browsing failed", it) }
    }

    fun stop() {
        discovery?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        discovery = null
        runCatching { lock?.release() }
        lock = null
        toResolve.clear()
    }

    // resolveService takes one request at a time, so found services queue up here.
    @Suppress("DEPRECATION")
    private fun resolveNext() {
        if (resolving || discovery == null) return
        val next = toResolve.removeFirstOrNull() ?: return
        resolving = true
        val listener = object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) {
                main.post {
                    resolving = false
                    resolveNext()
                }
            }

            override fun onServiceResolved(info: NsdServiceInfo) {
                val address = info.host?.hostAddress
                val game = info.attributes["game"]?.toString(Charsets.UTF_8).orEmpty()
                main.post {
                    resolving = false
                    if (address != null) {
                        found[info.serviceName] = RemotePlayHostInfo(info.serviceName, game, address, info.port)
                        onChange(found.values.toList())
                    }
                    resolveNext()
                }
            }
        }
        runCatching { nsd.resolveService(next, listener) }.onFailure {
            resolving = false
            main.post { resolveNext() }
        }
    }
}

/** This device's IPv4 address on its current network, for a viewer to type in. */
internal fun localIpv4(context: Context): String? {
    val connectivity = context.applicationContext.getSystemService(ConnectivityManager::class.java) ?: return null
    val network = connectivity.activeNetwork ?: return null
    return connectivity.getLinkProperties(network)?.linkAddresses
        ?.map { it.address }
        ?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
        ?.hostAddress
}
