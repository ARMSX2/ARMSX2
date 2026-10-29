/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  This file is WebRTC's sdk/android/src/java/org/webrtc/HardwareVideoEncoder.java, changed for
 *  ARMSX2's remote play (the changes are listed in the class comment below). It is used under
 *  WebRTC's BSD license, reproduced here from WebRTC's LICENSE file. WebRTC's PATENTS file
 *  (https://webrtc.googlesource.com/src/+/main/PATENTS) holds an additional grant of
 *  intellectual property rights.
 *
 *  Copyright (c) 2011, The WebRTC project authors. All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *
 *    * Neither the name of Google nor the names of its contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package org.webrtc;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.opengl.GLES20;
import android.os.Build;
import android.os.Bundle;
import android.view.Surface;
import androidx.annotation.Nullable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.BlockingDeque;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Remote play (com.armsx2.remoteplay): the hardware H.264 encoder, set up for a live game stream.
 *
 * WebRTC's HardwareVideoEncoder (license above), with the parts that held the stream under 30 fps
 * changed. Frames come in two ways: as GL textures (remote play's hardware frames), drawn straight
 * into the codec's input surface so they never pass through the CPU, or as NV12 or I420 bytes,
 * copied into the codec's input buffers. The changes:
 *  - HardwareVideoEncoder throws a frame away whenever more than two are inside the codec. A
 *    Snapdragon encoder left in its default mode holds about three, so close to every other frame
 *    was lost. Here a frame is only dropped once {@link #MAX_FRAMES_IN_CODEC} are waiting.
 *  - The codec is asked for low latency (each frame out before the next goes in), real-time
 *    priority and no B-frames, plus Qualcomm's own low-latency switch. A codec that refuses any
 *    of it is configured again without them, and the implementation name says which one runs.
 *  - Frames carry their real capture times into the codec, so its rate control sees the actual
 *    frame rate, which for a game moves between 30 and 60.
 */
class ArmsxH264Encoder implements VideoEncoder {
  private static final String TAG = "ArmsxH264Encoder";

  // About 100 ms of video at 60 fps. Past this the codec has stalled, and a frame is dropped
  // rather than adding more delay.
  private static final int MAX_FRAMES_IN_CODEC = 6;
  private static final int FRAME_RATE = 60;
  // Quantizer floors (see format()): key frames, then the rest.
  private static final int MIN_QP_KEY_FRAME = 26;
  private static final int MIN_QP_FRAME = 22;
  private static final int DEQUEUE_OUTPUT_BUFFER_TIMEOUT_US = 100_000;
  private static final int MEDIA_CODEC_RELEASE_TIMEOUT_MS = 5000;

  static final AtomicInteger framesIn = new AtomicInteger();
  static final AtomicInteger framesOut = new AtomicInteger();
  static final AtomicInteger framesDropped = new AtomicInteger();

  /** The running encoder's implementation name, for the stream's stats; empty before the first. */
  static volatile String activeName = "";

  /**
   * The longest a frame took from capture to out of the encoder (ms) and the largest frame
   * (bytes) since the stats last took them: a stall in the stream shows up here if it starts in
   * the encoder.
   */
  static final AtomicInteger peakEncodeMs = new AtomicInteger();
  static final AtomicInteger peakFrameBytes = new AtomicInteger();

  /**
   * Set when the codec's input surface could not be set up. From then on texture frames go through
   * the byte path, and remote play stops sending them (see ArmsxVideoEncoderFactory).
   */
  static volatile boolean surfaceFailed;

  private final String codecName;
  private final int colorFormat;
  private final @Nullable Integer surfaceColorFormat;
  private final int keyFrameIntervalSec;
  private final @Nullable EglBase14.Context sharedContext;
  private final BlockingDeque<EncodedImage.Builder> outputBuilders = new LinkedBlockingDeque<>();
  private final BusyCount outputBuffersBusyCount = new BusyCount();

  // Encode thread.
  private @Nullable Callback callback;
  private int width;
  private int height;
  private int stride;
  private int sliceHeight;
  private int frameSizeBytes;
  private boolean semiPlanar;
  private int targetBitrateBps;
  private int appliedBitrateBps;
  private long lastPresentationUs;
  private boolean lowLatency;
  private @Nullable Thread outputThread;

  // Surface mode (texture frames): the codec's input surface, drawn with GL on the encode thread.
  private boolean useSurfaceMode;
  private @Nullable EglBase14 textureEglBase;
  private @Nullable Surface textureInputSurface;
  private final GlRectDrawer textureDrawer = new GlRectDrawer();
  private final VideoFrameDrawer videoFrameDrawer = new VideoFrameDrawer();

  // Shared with the output thread.
  private volatile @Nullable MediaCodec codec;
  private volatile boolean running;
  private volatile @Nullable Exception shutdownException;

  // Output thread: H.264's SPS and PPS, put in front of every key frame.
  private @Nullable ByteBuffer configBuffer;

  ArmsxH264Encoder(String codecName, int colorFormat, @Nullable Integer surfaceColorFormat,
      int keyFrameIntervalSec, @Nullable EglBase14.Context sharedContext) {
    this.codecName = codecName;
    this.colorFormat = colorFormat;
    this.surfaceColorFormat = surfaceColorFormat;
    this.keyFrameIntervalSec = keyFrameIntervalSec;
    this.sharedContext = sharedContext;
  }

  private boolean canUseSurface() {
    return sharedContext != null && surfaceColorFormat != null && !surfaceFailed;
  }

  @Override
  public VideoCodecStatus initEncode(Settings settings, Callback callback) {
    this.callback = callback;
    if (settings.width % 2 != 0 || settings.height % 2 != 0) {
      Logging.e(TAG, "MediaCodec requires 2x2 alignment.");
      return VideoCodecStatus.ERR_SIZE;
    }
    width = settings.width;
    height = settings.height;
    // Texture frames when they can be drawn, as WebRTC's own encoder assumes; the first frame of
    // the other kind switches over.
    useSurfaceMode = canUseSurface();
    if (settings.startBitrate != 0) {
      targetBitrateBps = settings.startBitrate * 1000;
    }
    Logging.d(TAG, "initEncode " + codecName + " " + width + "x" + height + " at " + targetBitrateBps
            + " bps");
    return initEncodeInternal();
  }

  private VideoCodecStatus initEncodeInternal() {
    lastPresentationUs = -1;
    MediaCodec created;
    try {
      created = MediaCodec.createByCodecName(codecName);
    } catch (IOException | IllegalArgumentException e) {
      Logging.e(TAG, "Cannot create media encoder " + codecName);
      return VideoCodecStatus.FALLBACK_SOFTWARE;
    }
    codec = created;
    try {
      try {
        created.configure(format(true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        lowLatency = true;
      } catch (IllegalArgumentException | IllegalStateException e) {
        Logging.w(TAG, "Low-latency setup refused, configuring the plain one: " + e);
        created.reset();
        created.configure(format(false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        lowLatency = false;
      }
      if (useSurfaceMode) {
        try {
          textureEglBase = EglBase.createEgl14(sharedContext, EglBase.CONFIG_RECORDABLE);
          textureInputSurface = created.createInputSurface();
          textureEglBase.createSurface(textureInputSurface);
          textureEglBase.makeCurrent();
        } catch (RuntimeException e) {
          // Start over in byte mode for good; texture frames get converted until they stop coming.
          Logging.e(TAG, "Input surface failed, frames go through buffers from here on", e);
          surfaceFailed = true;
          release();
          useSurfaceMode = false;
          return initEncodeInternal();
        }
      } else {
        updateInputFormat(created.getInputFormat());
      }
      created.start();
    } catch (RuntimeException e) {
      Logging.e(TAG, "initEncodeInternal failed", e);
      release();
      return VideoCodecStatus.FALLBACK_SOFTWARE;
    }
    appliedBitrateBps = targetBitrateBps;
    activeName = getImplementationName();
    running = true;
    outputThread = createOutputThread();
    outputThread.start();
    return VideoCodecStatus.OK;
  }

  private MediaFormat format(boolean lowLatency) {
    MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
    format.setInteger(MediaFormat.KEY_BIT_RATE, targetBitrateBps);
    format.setInteger(
        MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
    format.setInteger(MediaFormat.KEY_COLOR_FORMAT, useSurfaceMode ? surfaceColorFormat : colorFormat);
    format.setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE);
    format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, keyFrameIntervalSec);
    if (lowLatency) {
      format.setInteger(MediaFormat.KEY_LATENCY, 1);
      format.setInteger(MediaFormat.KEY_PRIORITY, 0);
      format.setInteger(MediaFormat.KEY_OPERATING_RATE, FRAME_RATE);
      format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
      if (codecName.startsWith("c2.qti.") || codecName.startsWith("OMX.qcom.")) {
        format.setInteger("vendor.qti-ext-enc-low-latency.enable", 1);
      }
      // A floor on the quantizer, so no single frame grows far past the average. While the
      // picture is still, CBR spends its budget on ever finer detail; the first frames after
      // movement, and key frames, then come out several times larger than usual and every
      // frame behind one waits for it. Still scenes lose detail nobody sees at 720p; busy
      // ones sit above the floor anyway.
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        format.setInteger(MediaFormat.KEY_VIDEO_QP_I_MIN, MIN_QP_KEY_FRAME);
        format.setInteger(MediaFormat.KEY_VIDEO_QP_P_MIN, MIN_QP_FRAME);
      }
    }
    return format;
  }

  @Override
  public VideoCodecStatus release() {
    VideoCodecStatus status = VideoCodecStatus.OK;
    if (outputThread != null) {
      running = false;
      if (!ThreadUtils.joinUninterruptibly(outputThread, MEDIA_CODEC_RELEASE_TIMEOUT_MS)) {
        Logging.e(TAG, "Media encoder release timeout");
        status = VideoCodecStatus.TIMEOUT;
      } else if (shutdownException != null) {
        Logging.e(TAG, "Media encoder release exception", shutdownException);
        status = VideoCodecStatus.ERROR;
      }
    } else {
      // Never started, so no output thread to release it.
      MediaCodec unstarted = codec;
      if (unstarted != null) {
        try {
          unstarted.release();
        } catch (Exception e) {
          Logging.e(TAG, "Media encoder release failed", e);
        }
      }
    }
    // GL goes on the encode thread, where its context is current.
    textureDrawer.release();
    videoFrameDrawer.release();
    if (textureEglBase != null) {
      textureEglBase.release();
      textureEglBase = null;
    }
    if (textureInputSurface != null) {
      textureInputSurface.release();
      textureInputSurface = null;
    }
    outputBuilders.clear();
    codec = null;
    outputThread = null;
    shutdownException = null;
    return status;
  }

  @Override
  public VideoCodecStatus encode(VideoFrame videoFrame, EncodeInfo encodeInfo) {
    MediaCodec current = codec;
    if (current == null) {
      return VideoCodecStatus.UNINITIALIZED;
    }
    VideoFrame.Buffer buffer = videoFrame.getBuffer();
    int frameWidth = buffer.getWidth();
    int frameHeight = buffer.getHeight();
    boolean surfaceFrame = canUseSurface() && buffer instanceof VideoFrame.TextureBuffer;
    if (frameWidth != width || frameHeight != height || surfaceFrame != useSurfaceMode) {
      VideoCodecStatus status = resetCodec(frameWidth, frameHeight, surfaceFrame);
      if (status != VideoCodecStatus.OK) {
        return status;
      }
      current = codec;
    }

    if (outputBuilders.size() >= MAX_FRAMES_IN_CODEC) {
      Logging.e(TAG, "Dropped frame, encoder queue full");
      framesDropped.incrementAndGet();
      return VideoCodecStatus.NO_OUTPUT;
    }

    boolean keyFrameRequested = false;
    for (EncodedImage.FrameType frameType : encodeInfo.frameTypes) {
      if (frameType == EncodedImage.FrameType.VideoFrameKey) {
        keyFrameRequested = true;
      }
    }
    if (keyFrameRequested) {
      requestKeyFrame(current);
    }

    EncodedImage.Builder builder = EncodedImage.builder()
            .setCaptureTimeNs(videoFrame.getTimestampNs())
            .setEncodedWidth(frameWidth)
            .setEncodedHeight(frameHeight)
            .setRotation(videoFrame.getRotation());
    outputBuilders.offer(builder);

    long presentationUs = videoFrame.getTimestampNs() / 1000;
    if (presentationUs <= lastPresentationUs) {
      presentationUs = lastPresentationUs + 1;
    }
    lastPresentationUs = presentationUs;

    VideoCodecStatus status = useSurfaceMode
        ? encodeTextureBuffer(videoFrame, presentationUs)
        : encodeByteBuffer(current, videoFrame, presentationUs);
    if (status != VideoCodecStatus.OK) {
      // Nothing went into the codec, so nothing will come out for this builder.
      outputBuilders.pollLast();
      framesDropped.incrementAndGet();
    } else {
      framesIn.incrementAndGet();
    }
    return status;
  }

  private VideoCodecStatus encodeTextureBuffer(VideoFrame videoFrame, long presentationUs) {
    EglBase14 eglBase = textureEglBase;
    if (eglBase == null) {
      return VideoCodecStatus.UNINITIALIZED;
    }
    try {
      GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
      // Drawn upright. This wraps the caller's buffer and is not released here.
      VideoFrame derotated = new VideoFrame(videoFrame.getBuffer(), 0, videoFrame.getTimestampNs());
      videoFrameDrawer.drawFrame(derotated, textureDrawer, null);
      eglBase.swapBuffers(TimeUnit.MICROSECONDS.toNanos(presentationUs));
      // The texture goes back to the presenter as soon as WebRTC releases this frame, right after
      // this returns, and the GPU has to be done reading it by then.
      GLES20.glFinish();
    } catch (RuntimeException e) {
      Logging.e(TAG, "encodeTexture failed", e);
      return VideoCodecStatus.ERROR;
    }
    return VideoCodecStatus.OK;
  }

  private VideoCodecStatus encodeByteBuffer(
      MediaCodec current, VideoFrame videoFrame, long presentationUs) {
    int index;
    try {
      index = current.dequeueInputBuffer(0);
    } catch (IllegalStateException e) {
      Logging.e(TAG, "dequeueInputBuffer failed", e);
      return VideoCodecStatus.ERROR;
    }
    if (index == -1) {
      Logging.d(TAG, "Dropped frame, no input buffers available");
      return VideoCodecStatus.NO_OUTPUT;
    }

    ByteBuffer input;
    try {
      input = current.getInputBuffer(index);
    } catch (IllegalStateException e) {
      Logging.e(TAG, "getInputBuffer with index=" + index + " failed", e);
      return VideoCodecStatus.ERROR;
    }
    if (input == null || input.capacity() < frameSizeBytes) {
      Logging.e(TAG, "Input buffer is smaller than frame size " + frameSizeBytes);
      return VideoCodecStatus.ERROR;
    }
    fillInputBuffer(input, videoFrame.getBuffer());

    try {
      current.queueInputBuffer(index, 0, frameSizeBytes, presentationUs, 0);
    } catch (IllegalStateException e) {
      Logging.e(TAG, "queueInputBuffer failed", e);
      return VideoCodecStatus.ERROR;
    }
    return VideoCodecStatus.OK;
  }

  private void fillInputBuffer(ByteBuffer input, VideoFrame.Buffer buffer) {
    if (semiPlanar && buffer instanceof ArmsxNV12Buffer) {
      // Remote play's own frames are NV12 already: a plane copy each, nothing converted.
      ArmsxNV12Buffer nv12 = (ArmsxNV12Buffer) buffer;
      int ySize = nv12.width * nv12.height;
      YuvHelper.copyPlane(slice(nv12.data, 0, ySize), nv12.width, slice(input, 0, stride * sliceHeight),
          stride, nv12.width, nv12.height);
      YuvHelper.copyPlane(slice(nv12.data, ySize, ySize / 2), nv12.width,
          slice(input, stride * sliceHeight, input.capacity() - stride * sliceHeight), stride,
          nv12.width, nv12.height / 2);
      return;
    }

    VideoFrame.I420Buffer i420 = buffer.toI420();
    if (semiPlanar) {
      YuvHelper.I420ToNV12(i420.getDataY(), i420.getStrideY(), i420.getDataU(), i420.getStrideU(),
          i420.getDataV(), i420.getStrideV(), input, i420.getWidth(), i420.getHeight(), stride,
          sliceHeight);
    } else {
      YuvHelper.I420Copy(i420.getDataY(), i420.getStrideY(), i420.getDataU(), i420.getStrideU(),
          i420.getDataV(), i420.getStrideV(), input, i420.getWidth(), i420.getHeight(), stride,
          sliceHeight);
    }
    i420.release();
  }

  private static ByteBuffer slice(ByteBuffer buffer, int offset, int length) {
    ByteBuffer view = buffer.duplicate();
    view.position(offset);
    view.limit(offset + length);
    return view.slice();
  }

  private void requestKeyFrame(MediaCodec current) {
    try {
      Bundle params = new Bundle();
      params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
      current.setParameters(params);
    } catch (IllegalStateException e) {
      Logging.e(TAG, "requestKeyFrame failed", e);
    }
  }

  @Override
  public VideoCodecStatus setRateAllocation(BitrateAllocation bitrateAllocation, int framerate) {
    return applyBitrate(bitrateAllocation.getSum());
  }

  @Override
  public VideoCodecStatus setRates(RateControlParameters rcParameters) {
    return applyBitrate(rcParameters.bitrate.getSum());
  }

  // The codec's rate follows WebRTC's target as it moves; the frame rate is left to the frames'
  // own timestamps.
  private VideoCodecStatus applyBitrate(int bitrateBps) {
    targetBitrateBps = bitrateBps;
    MediaCodec current = codec;
    if (current == null || bitrateBps == appliedBitrateBps || bitrateBps <= 0) {
      return VideoCodecStatus.OK;
    }
    try {
      Bundle params = new Bundle();
      params.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrateBps);
      current.setParameters(params);
      appliedBitrateBps = bitrateBps;
      return VideoCodecStatus.OK;
    } catch (IllegalStateException e) {
      Logging.e(TAG, "updateBitrate failed", e);
      return VideoCodecStatus.ERROR;
    }
  }

  // Never resized by how complex the picture is: remote play keeps one resolution throughout,
  // since each change restarts this encoder and the viewer's decoder (see RemotePlayHost).
  @Override
  public ScalingSettings getScalingSettings() {
    return ScalingSettings.OFF;
  }

  @Override
  public String getImplementationName() {
    String name = lowLatency ? codecName + " low-latency" : codecName;
    return useSurfaceMode ? name + " zero-copy" : name;
  }

  @Override
  public EncoderInfo getEncoderInfo() {
    return new EncoderInfo(2, false);
  }

  private VideoCodecStatus resetCodec(int newWidth, int newHeight, boolean newUseSurfaceMode) {
    VideoCodecStatus status = release();
    if (status != VideoCodecStatus.OK) {
      return status;
    }
    if (newWidth % 2 != 0 || newHeight % 2 != 0) {
      Logging.e(TAG, "MediaCodec requires 2x2 alignment.");
      return VideoCodecStatus.ERR_SIZE;
    }
    width = newWidth;
    height = newHeight;
    useSurfaceMode = newUseSurfaceMode;
    return initEncodeInternal();
  }

  private void updateInputFormat(@Nullable MediaFormat format) {
    stride = width;
    sliceHeight = height;
    if (format != null) {
      if (format.containsKey(MediaFormat.KEY_STRIDE)) {
        stride = Math.max(format.getInteger(MediaFormat.KEY_STRIDE), width);
      }
      if (format.containsKey(MediaFormat.KEY_SLICE_HEIGHT)) {
        sliceHeight = Math.max(format.getInteger(MediaFormat.KEY_SLICE_HEIGHT), height);
      }
    }
    semiPlanar = isSemiPlanar(colorFormat);
    if (semiPlanar) {
      int chromaHeight = (height + 1) / 2;
      frameSizeBytes = sliceHeight * stride + chromaHeight * stride;
    } else {
      int chromaStride = (stride + 1) / 2;
      int chromaSliceHeight = (sliceHeight + 1) / 2;
      frameSizeBytes = sliceHeight * stride + chromaSliceHeight * chromaStride * 2;
    }
  }

  private static boolean isSemiPlanar(int colorFormat) {
    switch (colorFormat) {
      case MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar:
        return false;
      case MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar:
      case MediaCodecUtils.COLOR_QCOM_FORMATYUV420PackedSemiPlanar32m:
      case 0x7FA30C00: // OMX_QCOM_COLOR_FormatYUV420SemiPlanar
        return true;
      default:
        throw new IllegalArgumentException("Unsupported colorFormat: " + colorFormat);
    }
  }

  private Thread createOutputThread() {
    return new Thread("ArmsxH264Encoder.outputThread") {
      @Override
      public void run() {
        while (running) {
          deliverEncodedImage();
        }
        releaseCodecOnOutputThread();
      }
    };
  }

  // Output thread.
  private void deliverEncodedImage() {
    final MediaCodec current = codec;
    if (current == null) {
      return;
    }
    try {
      MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
      int index = current.dequeueOutputBuffer(info, DEQUEUE_OUTPUT_BUFFER_TIMEOUT_US);
      if (index < 0) {
        if (index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
          outputBuffersBusyCount.waitForZero();
        }
        return;
      }

      ByteBuffer output = current.getOutputBuffer(index);
      if (output == null) {
        current.releaseOutputBuffer(index, false);
        return;
      }
      output.position(info.offset);
      output.limit(info.offset + info.size);

      if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
        if (info.size > 0) {
          // SPS and PPS. Some encoders send IDR frames without them, so they go in front of
          // every key frame.
          configBuffer = ByteBuffer.allocateDirect(info.size);
          configBuffer.put(output);
        }
        current.releaseOutputBuffer(index, false);
        return;
      }

      boolean isKeyFrame = (info.flags & MediaCodec.BUFFER_FLAG_SYNC_FRAME) != 0;
      ByteBuffer frameBuffer;
      Runnable releaseCallback;
      if (isKeyFrame && configBuffer != null) {
        frameBuffer = ByteBuffer.allocateDirect(info.size + configBuffer.capacity());
        configBuffer.rewind();
        frameBuffer.put(configBuffer);
        frameBuffer.put(output);
        frameBuffer.rewind();
        current.releaseOutputBuffer(index, false);
        releaseCallback = null;
      } else {
        // Handed over without a copy; the codec gets the buffer back when WebRTC is done with it.
        frameBuffer = output.slice();
        outputBuffersBusyCount.increment();
        final int releaseIndex = index;
        releaseCallback = () -> {
          try {
            current.releaseOutputBuffer(releaseIndex, false);
          } catch (Exception e) {
            Logging.e(TAG, "releaseOutputBuffer failed", e);
          }
          outputBuffersBusyCount.decrement();
        };
      }

      EncodedImage.Builder builder = outputBuilders.poll();
      if (builder == null) {
        // More out than went in; nothing to attach it to.
        if (releaseCallback != null) {
          releaseCallback.run();
        }
        return;
      }
      EncodedImage image = builder.setBuffer(frameBuffer, releaseCallback)
              .setFrameType(isKeyFrame ? EncodedImage.FrameType.VideoFrameKey
                                       : EncodedImage.FrameType.VideoFrameDelta)
              .createEncodedImage();
      int encodeMs = (int) TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - image.captureTimeNs);
      peakEncodeMs.accumulateAndGet(encodeMs, Math::max);
      peakFrameBytes.accumulateAndGet(frameBuffer.remaining(), Math::max);
      Callback target = callback;
      if (target != null) {
        target.onEncodedFrame(image, new CodecSpecificInfo());
      }
      image.release();
      framesOut.incrementAndGet();
    } catch (IllegalStateException e) {
      Logging.e(TAG, "deliverOutput failed", e);
    }
  }

  // Output thread.
  private void releaseCodecOnOutputThread() {
    outputBuffersBusyCount.waitForZero();
    MediaCodec current = codec;
    if (current != null) {
      try {
        current.stop();
      } catch (Exception e) {
        Logging.e(TAG, "Media encoder stop failed", e);
      }
      try {
        current.release();
      } catch (Exception e) {
        Logging.e(TAG, "Media encoder release failed", e);
        shutdownException = e;
      }
    }
    configBuffer = null;
  }

  /** Output buffers WebRTC still holds; the codec is only stopped once it has them all back. */
  private static class BusyCount {
    private final Object countLock = new Object();
    private int count;

    void increment() {
      synchronized (countLock) {
        count++;
      }
    }

    void decrement() {
      synchronized (countLock) {
        count--;
        if (count == 0) {
          countLock.notifyAll();
        }
      }
    }

    void waitForZero() {
      boolean wasInterrupted = false;
      synchronized (countLock) {
        while (count > 0) {
          try {
            countLock.wait();
          } catch (InterruptedException e) {
            Logging.e(TAG, "Interrupted while waiting on busy count", e);
            wasInterrupted = true;
          }
        }
      }
      if (wasInterrupted) {
        Thread.currentThread().interrupt();
      }
    }
  }
}
