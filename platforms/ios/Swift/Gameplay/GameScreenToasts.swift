// GameScreenToasts.swift — the game screen's status toast, live preview toast and its exit overlay
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct RetroAchievementsToast: Equatable {
    let categoryTitle: String
    let title: String
    let message: String
    let badgePath: String?
    let symbolName: String?
    let imageData: Data?
}

/// Isolates rapid Per-Game value updates from `GameScreenView`'s large render tree. The toast is
/// the only view which observes this object, so holding Left/Right no longer invalidates Metal,
/// virtual-pad, Quick Menu, and overlay layout on every repeated percentage step.
@MainActor
@Observable
final class PerGameLivePreviewToastState {
    private(set) var status: PerGameLivePreviewStatus?

    func present(_ next: PerGameLivePreviewStatus) {
        guard status?.value != next.value else { return }
        withAnimation(.easeOut(duration: 0.14)) {
            status = next
        }
    }

    func clear() {
        guard status != nil else { return }
        withAnimation(.easeIn(duration: 0.14)) {
            status = nil
        }
    }
}

struct GameScreenStatusToastOverlay: View {
    let livePreview: PerGameLivePreviewToastState?
    @ObservedObject var fallback: TransientBannerController<String>
    let bottomPadding: CGFloat
    let horizontalPadding: CGFloat

    var body: some View {
        if let message = livePreview?.status?.value ?? fallback.content {
            Text(message)
                .font(.callout.weight(.semibold))
                .foregroundStyle(.white)
                // The same Text remains mounted while its number rolls in the direction of the
                // adjustment. This avoids repeatedly transitioning an entire toast in and out.
                .contentTransition(
                    .numericText(
                        countsDown: livePreview?.status?.countsDown ?? false
                    )
                )
                .animation(.easeOut(duration: 0.14), value: message)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.72), in: Capsule())
                .padding(.bottom, bottomPadding)
                .padding(.horizontal, horizontalPadding)
                .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }
}

/// Owns all input while the live game image is exposed. The first command is
/// consumed here, rather than being delivered to either gameplay or the
/// invisible Per-Game Settings graph, and requests restoration of the editor.
struct PerGameLivePreviewExitOverlay: View {
    let controllerInput: MenuControllerInputRouter?
    let stopsWithCircle: Bool
    let onExit: () -> Void

    @Environment(\.uiContentTextColour) private var textColour
    @State private var exitRequested = false

    var body: some View {
        ZStack(alignment: .top) {
            Color.clear
                .contentShape(Rectangle())
                .onTapGesture {
                    requestExit(fromController: false)
                }

            Label(
                stopsWithCircle
                    ? "Tap or press Circle to return"
                    : "Tap or press any button to return",
                systemImage: "gamecontroller.fill"
            )
            .font(.callout.weight(.semibold))
            .foregroundStyle(textColour)
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .glassSurface(
                clear: false,
                forceClear: false,
                cornerRadius: 22
            )
            .padding(.horizontal, 20)
            .padding(.top, 22)
            .allowsHitTesting(false)
        }
        .ignoresSafeArea()
        .onAppear {
            controllerInput?.setNavigationCaptured(
                true,
                owner: MenuControllerNavigationCaptureOwner
                    .perGameLivePreview,
                priority: 1_000
            )
        }
        .onDisappear {
            controllerInput?.setNavigationCaptured(
                false,
                owner: MenuControllerNavigationCaptureOwner
                    .perGameLivePreview,
                priority: 1_000
            )
        }
        .onChange(of: controllerInput?.latestEvent) { _, event in
            guard let event,
                  event.captureOwner
                    == MenuControllerNavigationCaptureOwner
                        .perGameLivePreview else { return }
            if event.command == .back {
                requestExit(fromController: true)
            } else if stopsWithCircle,
                      event.command.horizontalComponent != nil {
                _ = controllerInput?.adjustPerGameLivePreview(event.command)
            } else if !stopsWithCircle {
                requestExit(fromController: true)
            }
        }
    }

    private func requestExit(fromController: Bool) {
        guard !exitRequested else { return }
        exitRequested = true
        if fromController {
            controllerInput?.playFeedback(.back)
        } else {
            MenuAudioPackManager.shared.playEvent(.return)
            controllerInput?.playTouchHaptics(.back)
        }
        onExit()
    }
}
