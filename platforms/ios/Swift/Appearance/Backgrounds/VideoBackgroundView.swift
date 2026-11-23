// VideoBackgroundView.swift — Looping video background renderer
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import AVKit
import VideoToolbox

/// Retains only the playhead for each configured local background video while
/// iOS temporarily dismantles the inactive renderer. Gameplay teardown clears
/// the entry so menu-only media state does not survive in emulation memory.
@MainActor
private final class BackgroundVideoPlaybackPositionStore {
    static let shared = BackgroundVideoPlaybackPositionStore()
    private var positions: [URL: CMTime] = [:]

    func save(_ time: CMTime, for url: URL) {
        let seconds = time.seconds
        guard time.isValid, seconds.isFinite, seconds >= 0 else { return }
        positions[url.standardizedFileURL] = time
    }

    func position(for url: URL) -> CMTime? {
        positions[url.standardizedFileURL]
    }

    func clear(for url: URL) {
        positions.removeValue(forKey: url.standardizedFileURL)
    }
}

struct VideoBackgroundView: View {
    let url: URL
    let muted: Bool
    let fitMode: BackgroundFitMode

    var body: some View { VideoBackgroundPlayer(url: url, muted: muted, fitMode: fitMode) }
}

private struct VideoBackgroundPlayer: UIViewRepresentable {
    let url: URL
    let muted: Bool
    let fitMode: BackgroundFitMode

    func makeUIView(context: Context) -> LoopingVideoView { LoopingVideoView() }
    func updateUIView(_ uiView: LoopingVideoView, context: Context) { uiView.configure(url: url, muted: muted, fitMode: fitMode) }

	static func dismantleUIView(_ uiView: LoopingVideoView, coordinator: ()) {
		MainActor.assumeIsolated { uiView.teardown() }
	}
}

private final class LoopingVideoView: UIView {
    private var playerLayer: AVPlayerLayer?
    private var playerLooper: AVPlayerLooper?
    private var player: AVQueuePlayer?
    private var currentURL: URL?
    private var inactiveFrameView: UIImageView?
    private var inactiveFrameOrientation = UIImage.Orientation.up
    private var loopingItemsObservation: NSKeyValueObservation?
    private var releasedForGameplay = false

    deinit {
        MainActor.assumeIsolated {
			teardown()
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        backgroundColor = .clear
    }

    func configure(url: URL, muted: Bool, fitMode: BackgroundFitMode) {
        guard !releasedForGameplay else { return }
        let gravity = videoGravity(for: fitMode)

        // Recreate playback when the resolved source changes, including
        // orientation-specific assets.
        if player != nil, currentURL != url {
			teardown()
        }

        if player != nil {
            player?.isMuted = muted
            playerLayer?.videoGravity = gravity
            return
        }

        currentURL = url
        let item = AVPlayerItem(url: url)
        let queuePlayer = AVQueuePlayer(playerItem: item)
        queuePlayer.isMuted = muted
        queuePlayer.allowsExternalPlayback = false
        let looper = AVPlayerLooper(player: queuePlayer, templateItem: item)
        // The looper plays replicas of the template, made once it is ready.
        loopingItemsObservation = looper.observe(\.status, options: [.initial]) { @Sendable [weak self] _, _ in
            Task { @MainActor in self?.attachVideoOutputs() }
        }
        // The output hands frames over as stored; the layer applies the track transform.
        Task { @MainActor [weak self] in
            let track = try? await AVURLAsset(url: url).loadTracks(withMediaType: .video).first
            guard let transform = try? await track?.load(.preferredTransform),
                  let self, currentURL == url else { return }
            inactiveFrameOrientation = switch (transform.a, transform.b) {
            case (_, 1): .right
            case (_, -1): .left
            case (-1, _): .down
            default: .up
            }
        }
        let layer = AVPlayerLayer(player: queuePlayer)
        layer.videoGravity = gravity
        layer.backgroundColor = UIColor.clear.cgColor
        self.playerLayer?.removeFromSuperlayer()
        self.layer.addSublayer(layer)
        self.playerLayer = layer
        self.playerLooper = looper
        self.player = queuePlayer
        layoutIfNeeded()

        NotificationCenter.default.addObserver(self, selector: #selector(pause), name: UIApplication.didEnterBackgroundNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(captureInactiveFrame), name: UIScene.willDeactivateNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(pause), name: UIScene.willDeactivateNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(resumeIfReady), name: UIScene.didActivateNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(releaseResourcesForGameplay), name: AppState.releaseMenuBackgroundResourcesNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(adaptToPowerState), name: .NSProcessInfoPowerStateDidChange, object: nil)
        if let resumeTime = BackgroundVideoPlaybackPositionStore.shared.position(
            for: url
        ), resumeTime.seconds > 0.01 {
            queuePlayer.seek(
                to: resumeTime,
                toleranceBefore: .zero,
                toleranceAfter: .zero
            ) { [weak self] _ in
                Task { @MainActor in
                    self?.resumeIfReady()
                }
            }
        } else {
            applyPowerState()
        }
    }

	func teardown(preservePlaybackPosition: Bool = true) {
        if preservePlaybackPosition,
           let currentURL,
           let player {
            BackgroundVideoPlaybackPositionStore.shared.save(
                player.currentTime(),
                for: currentURL
            )
        }
        player?.pause()
        NotificationCenter.default.removeObserver(self)
        playerLayer?.removeFromSuperlayer()
        loopingItemsObservation = nil
        inactiveFrameView?.removeFromSuperview()
        inactiveFrameView = nil
        player = nil
        playerLooper = nil
        playerLayer = nil
		currentURL = nil
    }

    @objc private func pause() {
        player?.pause()
        preserveCurrentPlaybackPosition()
    }

    @objc private func resumeIfReady() {
        guard UIApplication.shared.applicationState != .background, !ProcessInfo.processInfo.isLowPowerModeEnabled else { return }
        inactiveFrameView?.removeFromSuperview()
        inactiveFrameView = nil
        // A paused AVQueuePlayer keeps its item time while this view stays mounted,
        // so a temporary pause such as Low Power Mode resumes instead of restarting.
        // A full inactive-scene teardown in BackgroundContainerView starts anew.
        player?.play()
    }

    /// AVPlayerLayer content is not guaranteed to appear in a UIKit hierarchy
    /// snapshot. Rasterize the current local-video frame into a normal UIImageView
    /// first so the persistent background host captures video just like image and
    /// dynamic backgrounds.
    @objc private func captureInactiveFrame(_ notification: Notification) {
        guard notification.object as? UIScene === window?.windowScene,
              let currentURL, let player, bounds.width > 0, bounds.height > 0 else {
            return
        }
        BackgroundVideoPlaybackPositionStore.shared.save(
            player.currentTime(),
            for: currentURL
        )

        guard let output = player.currentItem?.outputs.lazy.compactMap({ $0 as? AVPlayerItemVideoOutput }).first,
              let buffer = output.copyPixelBuffer(forItemTime: player.currentTime(), itemTimeForDisplay: nil) else {
            return
        }
        var image: CGImage?
        VTCreateCGImageFromCVPixelBuffer(buffer, options: nil, imageOut: &image)
        guard let image else { return }
        let decoded = UIImage(cgImage: image, scale: 1, orientation: inactiveFrameOrientation)
        let fit = min(1, bounds.width * traitCollection.displayScale / decoded.size.width,
                      bounds.height * traitCollection.displayScale / decoded.size.height)
        let size = CGSize(width: (decoded.size.width * fit).rounded(), height: (decoded.size.height * fit).rounded())
        let format = UIGraphicsImageRendererFormat.preferred()
        format.scale = 1
        let still = UIGraphicsImageRenderer(size: size, format: format).image { _ in
            decoded.draw(in: CGRect(origin: .zero, size: size))
        }
        inactiveFrameView?.removeFromSuperview()
        let frameView = UIImageView(image: still)
        frameView.frame = bounds
        frameView.clipsToBounds = true
        switch playerLayer?.videoGravity {
        case .resizeAspect:
            frameView.contentMode = .scaleAspectFit
        case .resize:
            frameView.contentMode = .scaleToFill
        default:
            frameView.contentMode = .scaleAspectFill
        }
        addSubview(frameView)
        inactiveFrameView = frameView
        NotificationCenter.default.post(name: .captureMenuBackgroundSnapshot, object: self)
    }

    private func attachVideoOutputs() {
        for item in playerLooper?.loopingPlayerItems ?? [] where item.outputs.isEmpty {
            item.add(AVPlayerItemVideoOutput(outputSettings: nil))
        }
    }

    @objc private func releaseResourcesForGameplay() {
        let releasedURL = currentURL
        releasedForGameplay = true
        teardown(preservePlaybackPosition: false)
        if let releasedURL {
            BackgroundVideoPlaybackPositionStore.shared.clear(for: releasedURL)
        }
    }

    // Unlike the UIKit notifications here, Foundation posts the power-state change
    // from any queue, and under Swift 6 a main-actor @objc entry traps off main. So
    // take it nonisolated and hop, the way the seek handler above already does.
    @objc private nonisolated func adaptToPowerState() {
        Task { @MainActor [weak self] in
            self?.applyPowerState()
        }
    }

    // Weakly, because a strong capture could leave the last release on that Task's
    // thread, and deinit assumes it is on main.
    private func applyPowerState() {
        if ProcessInfo.processInfo.isLowPowerModeEnabled {
            pause()
        } else {
            resumeIfReady()
        }
    }

    private func videoGravity(for fitMode: BackgroundFitMode) -> AVLayerVideoGravity {
        switch fitMode {
        case .fill: return .resizeAspectFill
        case .fit: return .resizeAspect
        case .stretch: return .resize
        }
    }

    private func preserveCurrentPlaybackPosition() {
        guard let currentURL, let player else { return }
        BackgroundVideoPlaybackPositionStore.shared.save(
            player.currentTime(),
            for: currentURL
        )
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let resolved = bounds.isEmpty ? superview?.bounds ?? bounds : bounds
        if let playerLayer, playerLayer.frame != resolved {
            playerLayer.frame = resolved
        }
        if inactiveFrameView?.frame != resolved {
            inactiveFrameView?.frame = resolved
        }
    }
}

private extension CGRect {
    var isEmpty: Bool { width <= 0 || height <= 0 }
}
