// BackgroundContainerView.swift — Orientation-aware background container
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import AVKit
import ImageIO

struct BackgroundContainerView: View {
    @State private var settings = SettingsStore.shared
    let size: CGSize
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorSchemeContrast) private var colorSchemeContrast
    @State private var videoPoster: UIImage?
    @State private var isRenderingEnabled = true
    @State private var inactiveReleaseTask: Task<Void, Never>?

    private var activeAsset: BackgroundAsset? {
        if size.width > size.height, let landscape = settings.backgroundLandscapeAsset { return landscape }
        return settings.backgroundPrimaryAsset
    }

    private var effectiveFitMode: BackgroundFitMode {
        size.width > size.height ? settings.backgroundLandscapeFitMode : settings.backgroundFitMode
    }

    private var effectiveDim: Double {
        let dim = settings.backgroundDim
        if reduceTransparency || colorSchemeContrast == .increased { return max(dim, 0.55) }
        return dim
    }

    var body: some View {
        ZStack {
            if isRenderingEnabled {
                if settings.dynamicBackgroundsEnabled {
                    DynamicBackgroundRendererView(
                        preferences: settings.dynamicAppearancePreferences
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .clipped()
                } else if let asset = activeAsset {
                    let url = BackgroundStorage.fileURL(for: asset)
                    switch asset.kind {
                    case .image:
                        StaticLibraryBackgroundImageView(
                            url: url,
                            fitMode: effectiveFitMode,
                            size: size
                        )
                    case .animatedImage:
                        animated(url)
                    case .video:
                        if reduceMotion {
                            backgroundImage(videoPoster)
                                .task(id: url.path) { await generateVideoPoster(from: url) }
                        } else {
                            video(url)
                        }
                    }
                }
            }
            Color.black.opacity(effectiveDim)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .ignoresSafeArea()
        .onAppear {
            inactiveReleaseTask?.cancel()
            isRenderingEnabled = true
        }
        .onReceive(NotificationCenter.default.publisher(for: UIScene.willDeactivateNotification)) { _ in
            inactiveReleaseTask?.cancel()
            inactiveReleaseTask = Task { @MainActor in
                // Allow the immediate captured frame to enter the hierarchy
                // before releasing the inactive live surface.
                try? await Task.sleep(for: .milliseconds(32))
                guard !Task.isCancelled else { return }
                isRenderingEnabled = false
                videoPoster = nil
                inactiveReleaseTask = nil
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIScene.didActivateNotification)) { _ in
            inactiveReleaseTask?.cancel()
            inactiveReleaseTask = nil
            isRenderingEnabled = true
        }
        .onReceive(NotificationCenter.default.publisher(for: AppState.releaseMenuBackgroundResourcesNotification)) { _ in
            inactiveReleaseTask?.cancel()
            inactiveReleaseTask = nil
            isRenderingEnabled = false
            videoPoster = nil
        }
    }

    private func animated(_ url: URL) -> some View {
        AnimatedLibraryBackgroundView(url: url, fitMode: effectiveFitMode)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipped()
    }

    private func video(_ url: URL) -> some View {
        VideoBackgroundView(url: url, muted: settings.backgroundVideoMuted, fitMode: effectiveFitMode)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipped()
    }

    @ViewBuilder
    private func backgroundImage(_ image: UIImage?) -> some View {
        if let image {
            Image(uiImage: image)
                .resizable()
                .applyBackgroundFitMode(effectiveFitMode, size: size)
        }
    }

    private func generateVideoPoster(from url: URL) async {
        let asset = AVAsset(url: url)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        do {
            let (cgImage, _) = try await generator.image(at: .zero)
            videoPoster = UIImage(cgImage: cgImage)
        } catch {
            videoPoster = nil
        }
    }
}

/// Loads a screen-sized static wallpaper away from the main actor. The source
/// image can be much larger than the display, so ImageIO downsamples it before
/// UIKit and SwiftUI retain the decoded pixels.
private struct StaticLibraryBackgroundImageView: View {
    let url: URL
    let fitMode: BackgroundFitMode
    let size: CGSize
    @State private var image: UIImage?

    private var loadIdentity: String {
        "\(url.path)|\(Int(size.width))x\(Int(size.height))"
    }

    var body: some View {
        GeometryReader { geometry in
            if let image {
                fitted(Image(uiImage: image))
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .clipped()
            }
        }
        .task(id: loadIdentity) {
            let scale = UIScreen.main.scale
            let maximumPixelSize = max(
                1,
                Int(max(size.width, size.height) * scale)
            )
            let loader = Task.detached(priority: .utility) {
                Self.downsampledImage(
                    at: url,
                    maximumPixelSize: maximumPixelSize,
                    scale: scale
                )
            }
            let loaded = await withTaskCancellationHandler {
                await loader.value
            } onCancel: {
                loader.cancel()
            }
            guard !Task.isCancelled else { return }
            image = loaded
        }
    }

    @ViewBuilder
    private func fitted(_ image: Image) -> some View {
        switch fitMode {
        case .fill:
            image.resizable().scaledToFill()
        case .fit:
            image.resizable().scaledToFit()
        case .stretch:
            image.resizable()
        }
    }

    private nonisolated static func downsampledImage(
        at url: URL,
        maximumPixelSize: Int,
        scale: CGFloat
    ) -> UIImage? {
        let sourceOptions = [
            kCGImageSourceShouldCache: false,
        ] as CFDictionary
        let thumbnailOptions = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maximumPixelSize,
        ] as CFDictionary
        guard let source = CGImageSourceCreateWithURL(
            url as CFURL,
            sourceOptions
        ),
        let thumbnail = CGImageSourceCreateThumbnailAtIndex(
            source,
            0,
            thumbnailOptions
        ) else {
            return nil
        }
        return UIImage(cgImage: thumbnail, scale: scale, orientation: .up)
    }
}

private extension Image {
    @ViewBuilder
    func applyBackgroundFitMode(_ mode: BackgroundFitMode, size: CGSize) -> some View {
        switch mode {
        case .fill:
            self.scaledToFill().frame(width: size.width, height: size.height).clipped()
        case .fit:
            self.scaledToFit().frame(width: size.width, height: size.height)
        case .stretch:
            self.frame(width: size.width, height: size.height).clipped()
        }
    }
}
