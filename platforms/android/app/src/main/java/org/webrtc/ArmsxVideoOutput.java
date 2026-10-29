// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package org.webrtc;

import android.view.Surface;
import androidx.annotation.Nullable;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Remote play (com.armsx2.remoteplay): the viewer's screen, which {@link ArmsxSurfaceDecoder}
 * decodes onto. The viewer passes in its view's surface as the view comes and goes, and hears back
 * when the first frame is up, what size the picture is, and whether it has to draw the frames
 * itself after all.
 */
public final class ArmsxVideoOutput {
  /** Called on the decoder's threads. */
  public interface Listener {
    void onFirstFrame();

    /** The picture's size as decoded, for fitting the view to it. */
    void onVideoSize(int width, int height);

    /**
     * Decoding onto the screen did not work out. From now on decoded frames reach the video track
     * the usual way, for a renderer to draw. Called at most once.
     */
    void onFallback();
  }

  // Set once decoding onto the screen failed on this device, so later sessions start the usual way.
  private static volatile boolean failedBefore;

  private final Listener listener;
  private volatile @Nullable Surface surface;
  private volatile @Nullable ArmsxSurfaceDecoder decoder;
  private final AtomicBoolean firstFrameShown = new AtomicBoolean();
  private final AtomicBoolean fellBack = new AtomicBoolean();

  public ArmsxVideoOutput(Listener listener) {
    this.listener = listener;
  }

  /** Whether to decode onto the screen at all: false once it has failed on this device. */
  public static boolean available() {
    return !failedBefore;
  }

  /**
   * Main thread, from the view's surface callbacks: the surface, or null once it is being
   * destroyed. A surface on its way out is no longer drawn on by the time this returns, as the
   * view requires.
   */
  public void setSurface(@Nullable Surface surface) {
    this.surface = surface;
    ArmsxSurfaceDecoder current = decoder;
    if (surface == null && current != null) {
      current.surfaceLost();
    }
  }

  @Nullable
  Surface surface() {
    return surface;
  }

  void attach(ArmsxSurfaceDecoder decoder) {
    this.decoder = decoder;
  }

  void detach(ArmsxSurfaceDecoder decoder) {
    if (this.decoder == decoder) {
      this.decoder = null;
    }
  }

  void frameShown() {
    if (firstFrameShown.compareAndSet(false, true)) {
      listener.onFirstFrame();
    }
  }

  void videoSize(int width, int height) {
    listener.onVideoSize(width, height);
  }

  static void markFailed() {
    failedBefore = true;
  }

  void fallBack() {
    if (fellBack.compareAndSet(false, true)) {
      listener.onFallback();
    }
  }
}
