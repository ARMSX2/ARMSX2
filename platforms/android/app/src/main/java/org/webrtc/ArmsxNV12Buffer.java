// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package org.webrtc;

import androidx.annotation.Nullable;
import java.nio.ByteBuffer;

/**
 * Remote play (com.armsx2.remoteplay): one frame as the host's GPU drew it, NV12 with rows packed:
 * the Y plane (width x height), then the interleaved UV plane (width x height / 2).
 *
 * {@link ArmsxH264Encoder} copies it straight into the codec's input buffer. Anything else that
 * needs the pixels (scaling, a software encoder) gets them through WebRTC's own NV12Buffer.
 */
public class ArmsxNV12Buffer implements VideoFrame.Buffer {
  final int width;
  final int height;
  final ByteBuffer data;
  private final RefCountDelegate refCountDelegate;

  public ArmsxNV12Buffer(int width, int height, ByteBuffer data, @Nullable Runnable releaseCallback) {
    this.width = width;
    this.height = height;
    this.data = data;
    this.refCountDelegate = new RefCountDelegate(releaseCallback);
  }

  @Override
  public int getWidth() {
    return width;
  }

  @Override
  public int getHeight() {
    return height;
  }

  // A view of the same bytes for WebRTC's converters; it holds no reference of its own, so it is
  // only used inside the calls below.
  private NV12Buffer view() {
    return new NV12Buffer(width, height, width, height, data, null);
  }

  @Override
  public VideoFrame.I420Buffer toI420() {
    NV12Buffer view = view();
    VideoFrame.I420Buffer i420 = view.toI420();
    view.release();
    return i420;
  }

  @Override
  public VideoFrame.Buffer cropAndScale(
      int cropX, int cropY, int cropWidth, int cropHeight, int scaleWidth, int scaleHeight) {
    NV12Buffer view = view();
    VideoFrame.Buffer scaled =
        view.cropAndScale(cropX, cropY, cropWidth, cropHeight, scaleWidth, scaleHeight);
    view.release();
    return scaled;
  }

  @Override
  public void retain() {
    refCountDelegate.retain();
  }

  @Override
  public void release() {
    refCountDelegate.release();
  }
}
