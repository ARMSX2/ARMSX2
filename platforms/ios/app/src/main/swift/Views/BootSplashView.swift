// BootSplashView.swift - Fullscreen boot intro video
// SPDX-License-Identifier: GPL-3.0+

import AVFoundation
import SwiftUI
import UIKit

struct BootSplashView: View {
    private static let hardTimeout: UInt64 = 6_000_000_000
    private static let idleVMPrewarmResolved = Notification.Name(
        "ARMSX2iOSIdleVMPrewarmResolved"
    )
    nonisolated static let stopPlaybackForAudioHandoff = Notification.Name(
        "ARMSX2iOSBootSplashStopPlaybackForAudioHandoff"
    )

    let onFinished: () -> Void
    @State private var finished = false
    @State private var playbackReady = ARMSX2Bridge.isIdleVMPrewarmResolved()
    @State private var playbackStarted = false

    var body: some View {
        ZStack {
            Color.black
                .ignoresSafeArea()

            BootSplashPlayerView(
                shouldPlay: playbackReady,
                onPlaybackStarted: { playbackStarted = true },
                onFinished: finish
            )
                .ignoresSafeArea()
        }
        .contentShape(Rectangle())
        .onTapGesture {
            finish()
        }
        .onAppear {
            if ARMSX2Bridge.isIdleVMPrewarmResolved() {
                playbackReady = true
            }
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: Self.idleVMPrewarmResolved
            )
        ) { _ in
            playbackReady = true
        }
        // Match master's startup order: JIT/VM prewarming resolves first, then
        // the intro gets its own playback window.
        .task(id: playbackStarted) {
            guard playbackReady, playbackStarted else { return }
            try? await Task.sleep(nanoseconds: Self.hardTimeout)
            guard !Task.isCancelled else {
                return
            }
            finish()
        }
    }

    @MainActor
    private func finish() {
        guard !finished else {
            return
        }

        // A tap can dismiss the splash while AVPlayer is still actively using
        // the process audio session. Pause it synchronously before RootView
        // begins the menu-audio handoff.
        NotificationCenter.default.post(
            name: Self.stopPlaybackForAudioHandoff,
            object: nil
        )
        finished = true
        onFinished()
    }
}

private struct BootSplashPlayerView: UIViewRepresentable {
    let shouldPlay: Bool
    let onPlaybackStarted: () -> Void
    let onFinished: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onPlaybackStarted: onPlaybackStarted, onFinished: onFinished)
    }

    func makeUIView(context: Context) -> BootSplashPlayerUIView {
        let view = BootSplashPlayerUIView()
        view.backgroundColor = .black
        view.playerLayer.videoGravity = .resizeAspect

        guard let url = Bundle.main.url(forResource: "boot_intro", withExtension: "mp4") else {
            DispatchQueue.main.async {
                context.coordinator.finish()
            }
            return view
        }

        let item = AVPlayerItem(url: url)
        let player = AVPlayer(playerItem: item)
        player.actionAtItemEnd = .pause
        view.playerLayer.player = player
        context.coordinator.configure(player: player, item: item, view: view)
        context.coordinator.setPlaybackAllowed(shouldPlay)

        return view
    }

    func updateUIView(_ uiView: BootSplashPlayerUIView, context: Context) {
        context.coordinator.setPlaybackAllowed(shouldPlay)
    }

    static func dismantleUIView(_ uiView: BootSplashPlayerUIView, coordinator: Coordinator) {
        uiView.onLayoutReady = nil
        uiView.playerLayer.player?.pause()
        uiView.playerLayer.player = nil
        coordinator.stopObserving()
    }

    final class Coordinator: @unchecked Sendable {
        var player: AVPlayer?
        private weak var playerView: BootSplashPlayerUIView?
        private let onPlaybackStarted: () -> Void
        private let onFinished: () -> Void
        private var endToken: NSObjectProtocol?
        private var errorToken: NSObjectProtocol?
        private var audioHandoffToken: NSObjectProtocol?
        private var statusObservation: NSKeyValueObservation?
        private var displayObservation: NSKeyValueObservation?
        private var hasStarted = false
        private var playbackStopped = false
        private var prerollRequested = false
        private var prerollFinished = false
        private var playbackAllowed = false

        init(onPlaybackStarted: @escaping () -> Void, onFinished: @escaping () -> Void) {
            self.onPlaybackStarted = onPlaybackStarted
            self.onFinished = onFinished
        }

        deinit {
            stopObserving()
        }

        @MainActor
        func configure(player: AVPlayer, item: AVPlayerItem, view: BootSplashPlayerUIView) {
            stopObserving()
            self.player = player
            playerView = view
            playbackStopped = false
            view.onLayoutReady = { [weak self] in self?.scheduleStartIfReady() }
            statusObservation = player.observe(\.status, options: [.initial, .new]) { [weak self] _, _ in
                self?.scheduleStartIfReady()
            }
            displayObservation = view.playerLayer.observe(\.isReadyForDisplay, options: [.initial, .new]) { [weak self] _, _ in
                self?.scheduleStartIfReady()
            }

            endToken = NotificationCenter.default.addObserver(
                forName: .AVPlayerItemDidPlayToEndTime,
                object: item,
                queue: .main
            ) { [weak self] _ in
                self?.finish()
            }

            errorToken = NotificationCenter.default.addObserver(
                forName: .AVPlayerItemFailedToPlayToEndTime,
                object: item,
                queue: .main
            ) { [weak self] _ in
                self?.finish()
            }

            audioHandoffToken = NotificationCenter.default.addObserver(
                forName: BootSplashView.stopPlaybackForAudioHandoff,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                self?.stopPlayback()
            }
        }

        private func scheduleStartIfReady() {
            // KVO and layout can arrive before SwiftUI finishes
            // mounting the player. Publish playback state on the next main turn.
            DispatchQueue.main.async { [weak self] in
                self?.startIfReady()
            }
        }

        @MainActor
        private func startIfReady() {
            guard playbackAllowed, !playbackStopped, !hasStarted,
                  let player else { return }
            if player.status == .failed {
                finish()
                return
            }
            guard player.status == .readyToPlay,
                  let playerView, playerView.window != nil,
                  playerView.bounds.width > 0, playerView.bounds.height > 0 else {
                return
            }

            // Prepare the first frame at time zero without running
            // the audio clock ahead of a layer that cannot display the video yet.
            if !prerollRequested {
                prerollRequested = true
                player.preroll(atRate: 1) { [weak self] completed in
                    DispatchQueue.main.async {
                        guard let self, !self.playbackStopped else { return }
                        guard completed else {
                            self.finish()
                            return
                        }
                        self.prerollFinished = true
                        self.startIfReady()
                    }
                }
                return
            }
            guard prerollFinished, playerView.playerLayer.isReadyForDisplay else { return }
            hasStarted = true
            player.play()
            onPlaybackStarted()
        }

        @MainActor
        func setPlaybackAllowed(_ allowed: Bool) {
            playbackAllowed = allowed
            if allowed {
                scheduleStartIfReady()
            }
        }

        private func stopPlayback() {
            playbackStopped = true
            player?.cancelPendingPrerolls()
            player?.pause()
        }

        func stopObserving() {
            stopPlayback()
            statusObservation?.invalidate()
            statusObservation = nil
            displayObservation?.invalidate()
            displayObservation = nil
            playerView = nil
            if let endToken {
                NotificationCenter.default.removeObserver(endToken)
                self.endToken = nil
            }
            if let errorToken {
                NotificationCenter.default.removeObserver(errorToken)
                self.errorToken = nil
            }
            if let audioHandoffToken {
                NotificationCenter.default.removeObserver(audioHandoffToken)
                self.audioHandoffToken = nil
            }
            player = nil
            hasStarted = false
            prerollRequested = false
            prerollFinished = false
        }

        func finish() {
            guard !playbackStopped else { return }
            stopPlayback()
            onFinished()
        }
    }
}

private final class BootSplashPlayerUIView: UIView {
    var onLayoutReady: (() -> Void)?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        onLayoutReady?()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        onLayoutReady?()
    }

    override static var layerClass: AnyClass {
        AVPlayerLayer.self
    }

    var playerLayer: AVPlayerLayer {
        layer as! AVPlayerLayer
    }
}
