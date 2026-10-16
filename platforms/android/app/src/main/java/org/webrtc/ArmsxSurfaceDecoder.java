package org.webrtc;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Process;
import android.os.SystemClock;
import android.view.Surface;
import androidx.annotation.Nullable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.concurrent.TimeUnit;

/**
 * Remote play (com.armsx2.remoteplay): the viewer's decoder, which puts each frame straight onto
 * the screen ({@link ArmsxVideoOutput}).
 *
 * WebRTC's own decoder hands every frame over as a GL texture for the viewer's renderer to draw:
 * one more pass on the GPU and a wait for it, and a frame that comes out while the one before is
 * still waiting to be drawn is thrown away. Here MediaCodec draws onto the screen's surface itself,
 * as a video player does, and the display shows the decoded picture as it is. Frames never queue
 * up behind each other: when more than one is done, the newest is shown and the older ones are
 * skipped, so the picture is never behind. The viewer asks the screen for 60 Hz while it streams,
 * so each frame stays up for exactly one refresh.
 *
 * WebRTC still hears of each frame shown, as a frame with a size and no pixels, so its statistics
 * stay true. If the codec will not start on the screen, the stock decoder takes over for the rest
 * of the session and the viewer draws its frames the usual way.
 */
public class ArmsxSurfaceDecoder implements VideoDecoder {
  private static final String TAG = "ArmsxSurfaceDecoder";

  // How long decode() waits for the codec to take a frame. A hardware decoder takes input at once;
  // one that has not within this is stuck.
  private static final long INPUT_TIMEOUT_US = 50_000;
  // How often the output thread looks up from waiting, to see whether it should stop.
  private static final long OUTPUT_TIMEOUT_US = 20_000;
  private static final long STOP_TIMEOUT_MS = 1_000;
  // Codecs in a row that may fail, without ever putting a frame on the screen, before the stock
  // decoder takes over. Failing covers not starting, refusing frames, an error, and taking this
  // many frames without showing one.
  private static final int MAX_FAILURES = 3;
  private static final int MAX_FRAMES_UNSHOWN = 120;
  // Frames the codec dropped never come out; their records go after this many.
  private static final int MAX_IN_CODEC = 32;

  // MediaFormat's crop keys, named there only from Android 13.
  private static final String CROP_LEFT = "crop-left";
  private static final String CROP_RIGHT = "crop-right";
  private static final String CROP_TOP = "crop-top";
  private static final String CROP_BOTTOM = "crop-bottom";

  // For the stream's stats: the slowest frame through the decoder (ms) and the longest wait between
  // two frames put on the screen (ms) since the stats last took them. A stall shows up in the gap;
  // the decode time says whether it was this decoder's.
  private static final java.util.concurrent.atomic.AtomicInteger peakDecodeMs =
      new java.util.concurrent.atomic.AtomicInteger();
  private static final java.util.concurrent.atomic.AtomicInteger peakGapMs =
      new java.util.concurrent.atomic.AtomicInteger();
  private static volatile long lastShownMs;

  /** Both peaks since the last call, which starts them over: {decode ms, gap ms}. */
  public static int[] takePeaks() {
    return new int[] {peakDecodeMs.getAndSet(0), peakGapMs.getAndSet(0)};
  }

  /** Makes the stock decoder for the same stream, or null when there is none. */
  interface StockDecoder {
    @Nullable
    VideoDecoder create();
  }

  private final String codecName;
  private final String mime;
  private final ArmsxVideoOutput output;
  private final StockDecoder stockDecoder;

  // WebRTC's decoder thread.
  private @Nullable Settings settings;
  private @Nullable Callback callback;
  private int width = 1280;
  private int height = 720;
  // Once the stock decoder has taken over, it stays, also through a reset by WebRTC.
  private boolean useStock;
  private @Nullable VideoDecoder stock;

  // Guarded by this: the decoder thread starts and replaces the codec, and the main thread stops it
  // when the surface goes.
  private @Nullable Running running;
  private boolean keyFrameRequired = true;
  private int failures;

  ArmsxSurfaceDecoder(
      String codecName, String mime, ArmsxVideoOutput output, StockDecoder stockDecoder) {
    this.codecName = codecName;
    this.mime = mime;
    this.output = output;
    this.stockDecoder = stockDecoder;
  }

  @Override
  public VideoCodecStatus initDecode(Settings settings, Callback callback) {
    this.settings = settings;
    this.callback = callback;
    if (useStock) {
      return startStock();
    }
    synchronized (this) {
      keyFrameRequired = true;
    }
    output.attach(this);
    return VideoCodecStatus.OK;
  }

  @Override
  public VideoCodecStatus decode(EncodedImage frame, DecodeInfo info) {
    if (useStock) {
      VideoDecoder decoder = stock;
      return decoder != null ? decoder.decode(frame, info) : VideoCodecStatus.UNINITIALIZED;
    }
    Callback callback = this.callback;
    if (callback == null) {
      return VideoCodecStatus.UNINITIALIZED;
    }
    if (frame.buffer == null || frame.buffer.remaining() == 0) {
      return VideoCodecStatus.ERR_PARAMETER;
    }
    boolean key = frame.frameType == EncodedImage.FrameType.VideoFrameKey;
    if (key && frame.encodedWidth > 0 && frame.encodedHeight > 0) {
      width = frame.encodedWidth;
      height = frame.encodedHeight;
    }

    boolean giveUp = false;
    synchronized (this) {
      Surface surface = output.surface();
      Running current = running;
      if (current != null) {
        boolean failed = current.broken
            || (!current.showedFrames && current.framesQueued >= MAX_FRAMES_UNSHOWN);
        if (failed || surface != current.surface
            || (key && (width != current.width || height != current.height))) {
          drop(current, failed);
          current = null;
        }
      }

      if (surface == null) {
        keyFrameRequired = true;
      } else if (failures >= MAX_FAILURES) {
        giveUp = true;
      } else {
        if (current == null) {
          if (!key) {
            // The codec can only start on a key frame: ask the host for one.
            keyFrameRequired = true;
            return VideoCodecStatus.NO_OUTPUT;
          }
          current = start(surface, callback);
          if (current == null) {
            if (++failures < MAX_FAILURES) {
              // Asks for another key frame, to try again with.
              return VideoCodecStatus.NO_OUTPUT;
            }
            giveUp = true;
          } else {
            running = current;
            keyFrameRequired = false;
          }
        }
        if (current != null) {
          if (keyFrameRequired && !key) {
            return VideoCodecStatus.NO_OUTPUT;
          }
          if (queue(current, frame)) {
            keyFrameRequired = false;
            return VideoCodecStatus.OK;
          }
          // Stuck or failed: start over from the next key frame.
          drop(current, true);
          if (failures < MAX_FAILURES) {
            return VideoCodecStatus.NO_OUTPUT;
          }
          giveUp = true;
        }
      }
    }
    if (giveUp) {
      return fallBack(frame, info);
    }

    // Nothing to show it on: the viewer is in the background. The frame is skipped, and once the
    // screen is back a key frame starts the picture again. WebRTC is told the frame is done with,
    // so it does not keep a record of each one for as long as the screen is away.
    VideoFrame skipped =
        new VideoFrame(new ShownBuffer(width, height), frame.rotation, frame.captureTimeNs);
    callback.onDecodedFrame(skipped, null, null);
    skipped.release();
    return VideoCodecStatus.OK;
  }

  @Override
  public VideoCodecStatus release() {
    // The codec stops before the screen lets go of this decoder: a surface destroyed in between
    // still waits for it.
    synchronized (this) {
      if (running != null) {
        stop(running);
        running = null;
      }
    }
    output.detach(this);
    callback = null;
    VideoDecoder decoder = stock;
    stock = null;
    return decoder != null ? decoder.release() : VideoCodecStatus.OK;
  }

  @Override
  public String getImplementationName() {
    VideoDecoder decoder = stock;
    return decoder != null ? decoder.getImplementationName() : codecName + " direct";
  }

  /** Main thread: the surface is being destroyed. Nothing is drawn on it once this returns. */
  void surfaceLost() {
    // Time in the background is no gap in the stream.
    lastShownMs = 0;
    synchronized (this) {
      if (running != null) {
        drop(running, false);
      }
      keyFrameRequired = true;
    }
  }

  // Under this lock. A codec that put frames on the screen worked, whatever ended it, so the count
  // of failures starts over.
  private void drop(Running codec, boolean failed) {
    stop(codec);
    running = null;
    keyFrameRequired = true;
    if (codec.showedFrames) {
      failures = 0;
    } else if (failed) {
      failures++;
    }
  }

  // Under this lock.
  private @Nullable Running start(Surface surface, Callback callback) {
    MediaCodec codec = null;
    try {
      codec = MediaCodec.createByCodecName(codecName);
      ArmsxVideoDecoderFactory.configureLowLatency(
          codec, MediaFormat.createVideoFormat(mime, width, height), surface, null);
      codec.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT);
      codec.start();
    } catch (IOException | IllegalArgumentException | IllegalStateException e) {
      Logging.e(TAG, "Could not start " + codecName + " on the screen", e);
      if (codec != null) {
        codec.release();
      }
      return null;
    }
    Logging.d(TAG, codecName + " decoding " + width + "x" + height + " onto the screen");
    Running started = new Running(codec, surface, width, height, callback);
    started.thread.start();
    return started;
  }

  // Under this lock.
  private static boolean queue(Running codec, EncodedImage frame) {
    try {
      int index = codec.codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
      if (index < 0) {
        Logging.w(TAG, "Decoder took no frame for " + INPUT_TIMEOUT_US / 1000 + " ms");
        return false;
      }
      ByteBuffer input = codec.codec.getInputBuffer(index);
      int size = frame.buffer.remaining();
      if (input == null || input.capacity() < size) {
        Logging.e(TAG, "A frame of " + size + " bytes does not fit the decoder's input");
        return false;
      }
      input.clear();
      input.put(frame.buffer);
      long presentationUs = TimeUnit.NANOSECONDS.toMicros(frame.captureTimeNs);
      codec.queued(presentationUs, frame.rotation);
      codec.codec.queueInputBuffer(index, 0, size, presentationUs, 0);
      codec.framesQueued++;
      return true;
    } catch (IllegalStateException e) {
      Logging.e(TAG, "Decoder refused a frame", e);
      return false;
    }
  }

  // Under this lock. Once it returns, the codec is gone and nothing more of it reaches the screen.
  private static void stop(Running codec) {
    codec.stopping = true;
    try {
      codec.thread.join(STOP_TIMEOUT_MS);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
    try {
      codec.codec.stop();
    } catch (IllegalStateException e) {
      Logging.w(TAG, "Decoder stop failed: " + e);
    }
    codec.codec.release();
  }

  private VideoCodecStatus fallBack(EncodedImage frame, DecodeInfo info) {
    Logging.e(TAG, codecName + " shows nothing on the screen; the stock decoder takes over");
    ArmsxVideoOutput.markFailed();
    useStock = true;
    output.detach(this);
    output.fallBack();
    VideoCodecStatus status = startStock();
    VideoDecoder decoder = stock;
    return status == VideoCodecStatus.OK && decoder != null ? decoder.decode(frame, info) : status;
  }

  private VideoCodecStatus startStock() {
    Settings settings = this.settings;
    Callback callback = this.callback;
    VideoDecoder decoder = stockDecoder.create();
    if (decoder == null || settings == null || callback == null) {
      return VideoCodecStatus.ERROR;
    }
    VideoCodecStatus status = decoder.initDecode(settings, callback);
    if (status != VideoCodecStatus.OK) {
      decoder.release();
      return status;
    }
    stock = decoder;
    return VideoCodecStatus.OK;
  }

  /** One codec on one surface, and the thread that puts its frames on the screen. */
  private final class Running {
    final MediaCodec codec;
    final Surface surface;
    final int width;
    final int height;
    final Thread thread;
    volatile boolean stopping;
    volatile boolean broken;
    volatile boolean showedFrames;
    // Decoder thread.
    int framesQueued;

    private final Callback callback;
    // Frames in the codec, oldest first: presentation time (us), when it went in (ms), rotation.
    private final ArrayDeque<long[]> inCodec = new ArrayDeque<>();
    // The picture's size as the codec reports it, cropped. Written by the output thread.
    private volatile int shownWidth;
    private volatile int shownHeight;

    Running(MediaCodec codec, Surface surface, int width, int height, Callback callback) {
      this.codec = codec;
      this.surface = surface;
      this.width = width;
      this.height = height;
      this.callback = callback;
      this.shownWidth = width;
      this.shownHeight = height;
      this.thread = new Thread(this::show, "rp-video-out");
    }

    synchronized void queued(long presentationUs, int rotation) {
      inCodec.addLast(new long[] {presentationUs, SystemClock.elapsedRealtime(), rotation});
      while (inCodec.size() > MAX_IN_CODEC) {
        inCodec.pollFirst();
      }
    }

    // The frame that came out at this time. Older records go with it: frames skipped on the way.
    private synchronized @Nullable long[] finished(long presentationUs) {
      long[] frame;
      while ((frame = inCodec.pollFirst()) != null) {
        if (frame[0] == presentationUs) {
          return frame;
        }
      }
      return null;
    }

    private void show() {
      Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY);
      MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
      MediaCodec.BufferInfo newer = new MediaCodec.BufferInfo();
      try {
        while (!stopping) {
          int index = codec.dequeueOutputBuffer(info, OUTPUT_TIMEOUT_US);
          if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            formatChanged(codec.getOutputFormat());
            continue;
          }
          if (index < 0) {
            continue;
          }
          // More frames done behind this one: only the newest is worth showing.
          while (true) {
            int next = codec.dequeueOutputBuffer(newer, 0);
            if (next == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
              formatChanged(codec.getOutputFormat());
              continue;
            }
            if (next < 0) {
              break;
            }
            codec.releaseOutputBuffer(index, false);
            index = next;
            MediaCodec.BufferInfo swap = info;
            info = newer;
            newer = swap;
          }
          // Up at the screen's next refresh. A newer frame that reaches the screen before then
          // replaces it there instead of queueing behind it.
          codec.releaseOutputBuffer(index, System.nanoTime());
          shown(decoded(index, info.presentationTimeUs));
        }
      } catch (IllegalStateException e) {
        // MediaCodec.CodecException is one too.
        if (!stopping) {
          Logging.e(TAG, "Decoder failed", e);
          broken = true;
        }
      }
    }

    private void formatChanged(MediaFormat format) {
      int w = format.containsKey(MediaFormat.KEY_WIDTH)
          ? format.getInteger(MediaFormat.KEY_WIDTH) : width;
      int h = format.containsKey(MediaFormat.KEY_HEIGHT)
          ? format.getInteger(MediaFormat.KEY_HEIGHT) : height;
      if (format.containsKey(CROP_LEFT) && format.containsKey(CROP_RIGHT)) {
        w = format.getInteger(CROP_RIGHT) + 1 - format.getInteger(CROP_LEFT);
      }
      if (format.containsKey(CROP_TOP) && format.containsKey(CROP_BOTTOM)) {
        h = format.getInteger(CROP_BOTTOM) + 1 - format.getInteger(CROP_TOP);
      }
      if (w > 0 && h > 0) {
        shownWidth = w;
        shownHeight = h;
        output.videoSize(w, h);
      }
    }

    /**
     * Output thread: a frame just out of the codec, as {buffer index, presentation time (us),
     * decode time (ms, -1 when unknown), rotation}.
     */
    private long[] decoded(int index, long presentationUs) {
      long[] record = finished(presentationUs);
      long decodeMs = record != null ? SystemClock.elapsedRealtime() - record[1] : -1;
      return new long[] {index, presentationUs, decodeMs, record != null ? record[2] : 0};
    }

    /** Output thread: [frame] is on its way to the screen; tells WebRTC and the stats. */
    private void shown(long[] frame) {
      long presentationUs = frame[1];
      long now = SystemClock.elapsedRealtime();
      Integer decodeMs = frame[2] >= 0 ? (int) frame[2] : null;
      if (decodeMs != null) {
        peakDecodeMs.accumulateAndGet(decodeMs, Math::max);
      }
      long previous = lastShownMs;
      lastShownMs = now;
      if (previous != 0) {
        peakGapMs.accumulateAndGet((int) Math.min(now - previous, 60_000), Math::max);
      }
      VideoFrame shown = new VideoFrame(new ShownBuffer(shownWidth, shownHeight), (int) frame[3],
          TimeUnit.MICROSECONDS.toNanos(presentationUs));
      callback.onDecodedFrame(shown, decodeMs, null);
      shown.release();
      showedFrames = true;
      output.frameShown();
    }
  }

  /** A frame that is already on the screen: its size, for WebRTC's statistics, and no pixels. */
  private static final class ShownBuffer implements VideoFrame.Buffer {
    private final int width;
    private final int height;
    private final RefCountDelegate refCount = new RefCountDelegate(null);

    ShownBuffer(int width, int height) {
      this.width = Math.max(width, 1);
      this.height = Math.max(height, 1);
    }

    @Override
    public int getWidth() {
      return width;
    }

    @Override
    public int getHeight() {
      return height;
    }

    // Nothing reads the pixels of a frame that went straight to the screen. Anything that asks
    // gets black.
    @Override
    public VideoFrame.I420Buffer toI420() {
      JavaI420Buffer black = JavaI420Buffer.allocate(width, height);
      fill(black.getDataY(), (byte) 16);
      fill(black.getDataU(), (byte) 128);
      fill(black.getDataV(), (byte) 128);
      return black;
    }

    private static void fill(ByteBuffer plane, byte value) {
      for (int i = 0; i < plane.capacity(); i++) {
        plane.put(i, value);
      }
    }

    @Override
    public VideoFrame.Buffer cropAndScale(int cropX, int cropY, int cropWidth, int cropHeight,
        int scaleWidth, int scaleHeight) {
      return new ShownBuffer(scaleWidth, scaleHeight);
    }

    @Override
    public void retain() {
      refCount.retain();
    }

    @Override
    public void release() {
      refCount.release();
    }
  }
}
