package org.webrtc;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaCrypto;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Bundle;
import android.view.Surface;
import androidx.annotation.Nullable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Locale;

/**
 * Remote play (com.armsx2.remoteplay): the viewer's decoders.
 *
 * With a screen to decode onto ({@link ArmsxVideoOutput}), each codec's own MediaCodec decoder,
 * hardware first, draws straight onto it ({@link ArmsxSurfaceDecoder}). Otherwise, and when that
 * fails, the hardware H.264 decoder runs the stock way in low-latency mode, and everything else as
 * WebRTC's DefaultVideoDecoderFactory.
 *
 * Left in its default mode a decoder may hold several frames before it shows the first, which is
 * fine for a film and adds a few frames of lag to a game. The decoder is asked for low latency and
 * real-time priority, plus each vendor's own switch for the same; one that refuses is configured
 * again exactly as WebRTC would have.
 *
 * It lives in org.webrtc because AndroidVideoDecoder and the codec helpers are package-private.
 */
public class ArmsxVideoDecoderFactory implements VideoDecoderFactory {
  private static final String TAG = "ArmsxVideoDecoder";

  private final @Nullable EglBase.Context sharedContext;
  private final @Nullable ArmsxVideoOutput output;
  private final VideoDecoderFactory stock;
  private final @Nullable MediaCodecInfo h264;

  /** {@code output}: the viewer's screen to decode straight onto, or null for the stock way. */
  public ArmsxVideoDecoderFactory(
      @Nullable EglBase.Context sharedContext, @Nullable ArmsxVideoOutput output) {
    this.sharedContext = sharedContext;
    this.output = output;
    this.stock = new DefaultVideoDecoderFactory(sharedContext);
    this.h264 = findH264Decoder();
  }

  private static @Nullable MediaCodecInfo findH264Decoder() {
    String mime = VideoCodecMimeType.H264.mimeType();
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
      if (info.isEncoder() || isAlias(info) || !MediaCodecUtils.isHardwareAccelerated(info)) {
        continue;
      }
      if (!MediaCodecUtils.codecSupportsType(info, VideoCodecMimeType.H264)) continue;
      if (MediaCodecUtils.selectColorFormat(
              MediaCodecUtils.DECODER_COLOR_FORMATS, info.getCapabilitiesForType(mime)) == null) {
        continue;
      }
      return info;
    }
    return null;
  }

  /** The hardware decoder's name, for logs; null when H.264 decodes the stock way. */
  public @Nullable String hardwareH264Name() {
    return h264 != null ? h264.getName() : null;
  }

  @Override
  public VideoCodecInfo[] getSupportedCodecs() {
    return stock.getSupportedCodecs();
  }

  @Override
  public @Nullable VideoDecoder createDecoder(VideoCodecInfo info) {
    if (output != null) {
      VideoDecoder direct = ArmsxVideoOutput.available() ? createDirect(info) : null;
      if (direct != null) {
        return direct;
      }
      // Nothing can decode this stream onto the screen: the viewer draws its frames after all.
      output.fallBack();
    }
    if (h264 != null && VideoCodecMimeType.H264.name().equalsIgnoreCase(info.name)) {
      VideoDecoder decoder = textureDecoder(h264, VideoCodecMimeType.H264);
      if (decoder != null) {
        return decoder;
      }
    }
    return stock.createDecoder(info);
  }

  private @Nullable VideoDecoder createDirect(VideoCodecInfo info) {
    VideoCodecMimeType type = null;
    for (VideoCodecMimeType candidate : VideoCodecMimeType.values()) {
      if (candidate.name().equalsIgnoreCase(info.name)) {
        type = candidate;
        break;
      }
    }
    if (type == null) {
      return null;
    }
    MediaCodecInfo codec = findDirectDecoder(type);
    if (codec == null) {
      return null;
    }
    VideoCodecMimeType codecType = type;
    return new ArmsxSurfaceDecoder(codec.getName(), type.mimeType(), output,
        () -> textureDecoder(codec, codecType));
  }

  /**
   * A decoder that can draw this codec onto a surface: hardware first, then Android's own
   * software one, which every device has for the codecs WebRTC sends. None that needs a protected
   * or tunneled pipeline, which a plain surface is not. Works back to Android 8: before 10,
   * Android does not say which codecs are hardware, and WebRTC's helper goes by the name.
   */
  private static @Nullable MediaCodecInfo findDirectDecoder(VideoCodecMimeType type) {
    String mime = type.mimeType();
    MediaCodecInfo software = null;
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
      if (info.isEncoder() || isAlias(info) || !MediaCodecUtils.codecSupportsType(info, type)) {
        continue;
      }
      MediaCodecInfo.CodecCapabilities capabilities = info.getCapabilitiesForType(mime);
      if (capabilities.isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_SecurePlayback)
          || capabilities.isFeatureRequired(
              MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback)) {
        continue;
      }
      if (MediaCodecUtils.isHardwareAccelerated(info)) {
        return info;
      }
      if (software == null) {
        software = info;
      }
    }
    return software;
  }

  /** The stock decoder on this codec: frames out as GL textures, in low-latency mode. */
  private @Nullable VideoDecoder textureDecoder(MediaCodecInfo codec, VideoCodecMimeType type) {
    Integer colorFormat = MediaCodecUtils.selectColorFormat(
        MediaCodecUtils.DECODER_COLOR_FORMATS, codec.getCapabilitiesForType(type.mimeType()));
    if (colorFormat == null) {
      if (sharedContext == null) {
        return null;
      }
      // Frames that come out as textures never use the color format: AndroidVideoDecoder only
      // checks it is one it knows, and sets it on the codec only for frames in memory.
      colorFormat = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar;
    }
    return new AndroidVideoDecoder(
        new LowLatencyCodecs(), codec.getName(), type, colorFormat, sharedContext);
  }

  /** Android 10 added aliases, and asking for one before that fails. */
  static boolean isAlias(MediaCodecInfo info) {
    return Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && info.isAlias();
  }

  // Snapdragon 765, 765G and 768G (Adreno 620), where Moonlight found the decoder crashes when
  // asked for its fastest clocks. Before Android 12 only the platform's name says so: "lito" is
  // that family, and the 750G, which is left out with it to be safe.
  private static final boolean SNAPDRAGON_765 =
      (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && Build.SOC_MODEL.startsWith("SM7250"))
      || "lito".equalsIgnoreCase(Build.BOARD);

  /**
   * Configures a decoder for low latency: everything {@link #lowLatency} asks for, then Android's
   * own switch alone, then plainly, each only when the one before was refused. Most to least
   * risky, as Moonlight does.
   */
  static void configureLowLatency(MediaCodec codec, MediaFormat format, @Nullable Surface surface,
      @Nullable MediaCrypto crypto) {
    MediaFormat android = new MediaFormat(format);
    android.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
    MediaFormat[] attempts = {lowLatency(format, codec), android, format};
    String[] modes = {"low-latency mode", "Android's low-latency mode only", "default mode"};
    for (int i = 0;; i++) {
      try {
        codec.configure(attempts[i], surface, crypto, 0);
        Logging.d(TAG, codec.getName() + " in " + modes[i]);
        return;
      } catch (IllegalArgumentException | IllegalStateException e) {
        if (i == attempts.length - 1) {
          throw e;
        }
        Logging.w(TAG, "Low-latency setup refused, trying less: " + e);
        codec.reset();
      }
    }
  }

  /**
   * The format with low latency asked for the way Moonlight asks for it
   * (MediaCodecHelper.setDecoderLowLatencyOptions): Android's own switch, which is all a codec
   * that declares it honours it needs. Any other codec also gets the fastest clocks on Qualcomm
   * (realtime priority elsewhere) and each vendor's own switches.
   */
  static MediaFormat lowLatency(MediaFormat original, MediaCodec codec) {
    MediaFormat format = new MediaFormat(original);
    format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
    String mime = format.getString(MediaFormat.KEY_MIME);
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mime != null) {
      try {
        if (codec.getCodecInfo().getCapabilitiesForType(mime).isFeatureSupported(
                MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) {
          return format;
        }
      } catch (IllegalArgumentException e) {
        // No capabilities for that type: carry on with the rest.
      }
    }

    String name = codec.getName().toLowerCase(Locale.ROOT);
    boolean qualcomm = name.startsWith("omx.qcom") || name.startsWith("c2.qti");
    if (qualcomm && !SNAPDRAGON_765) {
      // Not realtime priority as well: a codec asked for both that cannot keep up crashes.
      format.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE);
    } else {
      format.setInteger(MediaFormat.KEY_PRIORITY, 0);
    }

    // Each vendor's own switches. A codec ignores keys from another vendor.
    if (qualcomm) {
      format.setInteger("vendor.qti-ext-dec-picture-order.enable", 1);
      format.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
    } else if (name.startsWith("omx.exynos") || name.startsWith("c2.exynos")) {
      format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1);
    } else if (name.startsWith("omx.mtk") || name.startsWith("c2.mtk")) {
      format.setInteger("vdec-lowlatency", 1);
    } else if (name.startsWith("omx.amlogic") || name.startsWith("c2.amlogic")) {
      format.setInteger("vdec-lowlatency", 1);
      format.setInteger("vendor.low-latency.enable", 1);
    } else if (name.startsWith("omx.hisi") || name.startsWith("c2.hisi")) {
      String hisi = "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-";
      format.setInteger(hisi + "req", 1);
      format.setInteger(hisi + "rdy", -1);
    }
    return format;
  }

  /** Opens codecs for AndroidVideoDecoder, adding the low-latency settings when it configures. */
  private static class LowLatencyCodecs implements MediaCodecWrapperFactory {
    @Override
    public MediaCodecWrapper createByCodecName(String name) throws IOException {
      return new LowLatencyCodec(MediaCodec.createByCodecName(name));
    }
  }

  private static class LowLatencyCodec implements MediaCodecWrapper {
    private final MediaCodec codec;

    LowLatencyCodec(MediaCodec codec) {
      this.codec = codec;
    }

    @Override
    public void configure(MediaFormat format, Surface surface, MediaCrypto crypto, int flags) {
      if (flags != 0) {
        codec.configure(format, surface, crypto, flags);
        return;
      }
      configureLowLatency(codec, format, surface, crypto);
    }

    @Override
    public void start() {
      codec.start();
    }

    @Override
    public void flush() {
      codec.flush();
    }

    @Override
    public void stop() {
      codec.stop();
    }

    @Override
    public void release() {
      codec.release();
    }

    @Override
    public int dequeueInputBuffer(long timeoutUs) {
      return codec.dequeueInputBuffer(timeoutUs);
    }

    @Override
    public void queueInputBuffer(
        int index, int offset, int size, long presentationTimeUs, int flags) {
      codec.queueInputBuffer(index, offset, size, presentationTimeUs, flags);
    }

    @Override
    public int dequeueOutputBuffer(MediaCodec.BufferInfo info, long timeoutUs) {
      return codec.dequeueOutputBuffer(info, timeoutUs);
    }

    @Override
    public void releaseOutputBuffer(int index, boolean render) {
      codec.releaseOutputBuffer(index, render);
    }

    @Override
    public MediaFormat getInputFormat() {
      return codec.getInputFormat();
    }

    @Override
    public MediaFormat getOutputFormat() {
      return codec.getOutputFormat();
    }

    @Override
    public MediaFormat getOutputFormat(int index) {
      return codec.getOutputFormat(index);
    }

    @Override
    public ByteBuffer getInputBuffer(int index) {
      return codec.getInputBuffer(index);
    }

    @Override
    public ByteBuffer getOutputBuffer(int index) {
      return codec.getOutputBuffer(index);
    }

    @Override
    public Surface createInputSurface() {
      return codec.createInputSurface();
    }

    @Override
    public void setParameters(Bundle params) {
      codec.setParameters(params);
    }

    @Override
    public MediaCodecInfo getCodecInfo() {
      return codec.getCodecInfo();
    }
  }
}
