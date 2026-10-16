// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.remoteplay

import android.graphics.Matrix
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import kr.co.iefriends.pcsx2.NativeApp
import org.webrtc.ArmsxNV12Buffer
import org.webrtc.ArmsxVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.GlUtil
import org.webrtc.TextureBufferImpl
import org.webrtc.VideoFrame
import org.webrtc.YuvConverter
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

private const val TAG = "RemotePlay"

/**
 * The host's frames on their way from the core to the encoder.
 *
 * Where the device allows it, a frame is a slot of the core's capture ring: an AHardwareBuffer the
 * GPU filled with the frame. Each slot is imported once as a GL texture, on a GL thread of its own
 * whose context shares textures with the encoder's, and handed over as a texture frame the encoder
 * draws straight into its input. When WebRTC releases the frame the slot goes back to the core. The
 * frame never passes through the CPU.
 *
 * Otherwise frames arrive as NV12, copied out of the core once into pooled buffers. A buffer that
 * cannot be imported, or an encoder that cannot take textures, switches to that for good.
 */
internal class RemotePlayFrames(textureInput: Boolean) {
    sealed interface Poll {
        data class Frame(val frame: VideoFrame) : Poll
        data object Timeout : Poll
        data object Stopped : Poll
    }

    @Volatile
    private var hardware = textureInput

    @Volatile
    private var released = false

    private val pool = ConcurrentLinkedQueue<ByteBuffer>()
    private val info = IntArray(6)

    // Texture frames: the GL thread, and per slot its texture, EGL image and the buffer generation
    // that image is of. The arrays belong to the GL thread, except generations (rp-video thread).
    private var glThread: HandlerThread? = null

    @Volatile
    private var gl: Handler? = null
    private var egl: EglBase? = null
    private var yuvConverter: YuvConverter? = null
    private val textures = IntArray(SLOTS)
    private val images = LongArray(SLOTS)
    private val generations = IntArray(SLOTS) { -1 }

    init {
        if (hardware) {
            val thread = HandlerThread("rp-gl").also { it.start() }
            glThread = thread
            gl = Handler(thread.looper)
            hardware = onGl {
                egl = EglBase.create(RemotePlayRtc.eglBase.eglBaseContext, EglBase.CONFIG_PIXEL_BUFFER).apply {
                    createDummyPbufferSurface()
                    makeCurrent()
                }
                yuvConverter = YuvConverter()
                true
            } == true
            if (!hardware) Log.w(TAG, "remote play: no GL context for texture frames")
        }
        NativeApp.remotePlaySetHardware(hardware)
        Log.i(TAG, "remote play: frames go to the encoder as " + if (hardware) "GPU textures" else "NV12 copies")
    }

    /** rp-video thread: the next frame, which the caller hands to WebRTC and then releases. */
    fun poll(timeoutMs: Int): Poll {
        if (hardware && ArmsxVideoEncoderFactory.textureInputFailed()) disableHardware()

        val buffer = pool.poll() ?: ByteBuffer.allocateDirect(FRAME_BYTES)
        val result = NativeApp.remotePlayPollVideo(buffer, info, timeoutMs)
        if (result != 1) pool.offer(buffer)

        return when (result) {
            2 -> textureFrame(slot = info[2], generation = info[3], seq = info[4], width = info[0], height = info[1])
            1 -> Poll.Frame(VideoFrame(ArmsxNV12Buffer(info[0], info[1], buffer) { pool.offer(buffer) }, 0, System.nanoTime()))
            -1 -> Poll.Stopped
            else -> Poll.Timeout
        }
    }

    private fun textureFrame(slot: Int, generation: Int, seq: Int, width: Int, height: Int): Poll {
        val handler = gl
        val converter = yuvConverter
        if (!hardware || released || handler == null || converter == null || slot !in 0 until SLOTS) {
            NativeApp.remotePlayReleaseFrame(slot, seq)
            return Poll.Timeout
        }

        if (generations[slot] != generation) {
            if (onGl { import(slot) } != true) {
                NativeApp.remotePlayReleaseFrame(slot, seq)
                disableHardware()
                return Poll.Timeout
            }
            generations[slot] = generation
        }

        val texture = TextureBufferImpl(
            width, height, VideoFrame.TextureBuffer.Type.OES, textures[slot], Matrix(FLIP), handler, converter,
        ) { NativeApp.remotePlayReleaseFrame(slot, seq) }
        return Poll.Frame(VideoFrame(texture, 0, System.nanoTime()))
    }

    // GL thread.
    private fun import(slot: Int): Boolean {
        if (images[slot] != 0L) {
            NativeApp.remotePlayDestroyImage(images[slot])
            images[slot] = 0L
        }
        if (textures[slot] == 0) textures[slot] = GlUtil.generateTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES)

        val image = NativeApp.remotePlayImportFrame(slot, textures[slot])
        if (image == 0L) {
            Log.w(TAG, "remote play: frame buffer $slot could not be imported")
            return false
        }
        images[slot] = image
        // The encoder's context sees the new binding once this one has finished with it.
        GLES20.glFinish()
        return true
    }

    /**
     * Runs [block] on the GL thread and waits for it, for at most a second. Null when the thread is
     * gone, the block threw or it did not finish in time: never a wait that cannot end, which a
     * post to a thread that has already quit would be.
     */
    private fun <T> onGl(block: () -> T): T? {
        val handler = gl ?: return null
        var value: T? = null
        val done = CountDownLatch(1)
        val posted = handler.post {
            try {
                value = block()
            } catch (e: RuntimeException) {
                Log.w(TAG, "remote play: GL work failed", e)
            } finally {
                done.countDown()
            }
        }
        if (!posted) return null
        return try {
            if (done.await(1, TimeUnit.SECONDS)) value else null
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            null
        }
    }

    private fun disableHardware() {
        if (!hardware) return
        hardware = false
        NativeApp.remotePlaySetHardware(false)
        Log.w(TAG, "remote play: texture frames off, sending NV12 copies")
    }

    /** Once nothing can draw the textures any more: after the peer, and with it the encoder, is gone. */
    fun release() {
        released = true
        val handler = gl ?: return
        gl = null
        handler.post {
            for (slot in 0 until SLOTS) {
                if (images[slot] != 0L) NativeApp.remotePlayDestroyImage(images[slot])
                if (textures[slot] != 0) GLES20.glDeleteTextures(1, intArrayOf(textures[slot]), 0)
                images[slot] = 0L
                textures[slot] = 0
            }
            yuvConverter?.release()
            egl?.release()
        }
        glThread?.quitSafely()
    }

    private companion object {
        // The core's capture ring (Emu/RSX/remote_capture.h).
        const val SLOTS = 4
        const val FRAME_BYTES = RemotePlayProtocol.MAX_WIDTH * RemotePlayProtocol.MAX_HEIGHT * 3 / 2

        // The buffer's first row is the top of the picture, and GL puts texture row 0 at the bottom.
        val FLIP = Matrix().apply {
            preTranslate(0f, 1f)
            preScale(1f, -1f)
        }
    }
}
