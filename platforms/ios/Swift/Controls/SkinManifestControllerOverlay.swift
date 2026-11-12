// SkinManifestControllerOverlay.swift — manifest v2 skin rendering for the virtual controller
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct SkinManifestControllerOverlay<InputZones: View>: View {
    let layout: SkinManifestRuntimeLayout
    let assetsDirectory: URL
    let descriptor: VPadSkinDescriptor
    let w: CGFloat
    let h: CGFloat
    let fitScale: CGFloat?
    let isLandscape: Bool
    let legacyThumbsticks: Bool
    let swipeCamera: Bool
    let inputSessionGeneration: Int
    let inputZones: InputZones

    var body: some View {
        let transform = v2Transform(mappingWidth: layout.mappingWidth, mappingHeight: layout.mappingHeight, canvasW: w, canvasH: h)
        ZStack {
            if let backgroundPath = layout.backgroundAssetPath,
               let backgroundImage = SkinManifestRuntimeLayout.image(forRelativePath: backgroundPath, in: assetsDirectory) {
                Image(uiImage: backgroundImage)
                    .resizable()
                    .interpolation(.high)
                    .antialiased(true)
                    .scaledToFill()
                    .frame(width: transform.scaleX, height: transform.scaleY)
                    .clipped()
                    .allowsHitTesting(false)
            }

            inputZones

            ForEach(layout.controls) { control in
                v2ControlView(control, transform: transform, assetsDirectory: assetsDirectory, descriptor: descriptor)
            }

            if layout.debug {
                ForEach(layout.controls) { control in
                    let hit = v2HitRect(control, transform: transform)
                    Rectangle()
                        .stroke(.yellow.opacity(0.5), lineWidth: 1)
                        .frame(width: hit.width, height: hit.height)
                        .position(x: hit.midX, y: hit.midY)
                        .allowsHitTesting(false)
                }
            }
        }
        .frame(width: w, height: h)
        .clipped()
    }

    // Uniform transform from the manifest mapping size to the controller canvas.
    private struct V2CanvasTransform {
        let originX: CGFloat
        let originY: CGFloat
        let scaleX: CGFloat
        let scaleY: CGFloat

        func rect(_ frame: NormalizedFrame) -> CGRect {
            CGRect(
                x: originX + frame.x * scaleX,
                y: originY + frame.y * scaleY,
                width: frame.width * scaleX,
                height: frame.height * scaleY
            )
        }
    }

    private func v2Transform(mappingWidth: CGFloat, mappingHeight: CGFloat, canvasW: CGFloat, canvasH: CGFloat) -> V2CanvasTransform {
        let fitsDeck = fitScale != nil && !isLandscape
        let scale = fitsDeck
            ? min(canvasW / mappingWidth, canvasH / mappingHeight)
            : max(canvasW / mappingWidth, canvasH / mappingHeight)
        let sx = mappingWidth * scale
        let sy = mappingHeight * scale
        return V2CanvasTransform(originX: (canvasW - sx) / 2, originY: (canvasH - sy) / 2, scaleX: sx, scaleY: sy)
    }

    private func v2VisualRect(_ control: SkinManifestRuntimeLayout.Control, transform: V2CanvasTransform) -> CGRect {
        transform.rect(control.visualFrame)
    }

    private func v2HitRect(_ control: SkinManifestRuntimeLayout.Control, transform: V2CanvasTransform) -> CGRect {
        let visual = v2VisualRect(control, transform: transform)
        let insets = control.hitInsets
        return CGRect(
            x: visual.minX - insets.left * transform.scaleX,
            y: visual.minY - insets.top * transform.scaleY,
            width: visual.width + (insets.left + insets.right) * transform.scaleX,
            height: visual.height + (insets.top + insets.bottom) * transform.scaleY
        )
    }

    @ViewBuilder
    private func v2ControlView(_ control: SkinManifestRuntimeLayout.Control, transform: V2CanvasTransform, assetsDirectory: URL, descriptor: VPadSkinDescriptor) -> some View {
        let visual = v2VisualRect(control, transform: transform)
        let hit = v2HitRect(control, transform: transform)
        let knobSize: CGSize? = {
            if let kw = control.knobWidth, let kh = control.knobHeight {
                CGSize(width: kw * transform.scaleX, height: kh * transform.scaleY)
            } else {
                nil
            }
        }()
        switch control.placement {
        case .inert:
            EmptyView()
        case .button(let button):
            SkinManifestButtonView(
                button: button,
                visualRect: visual,
                hitRect: hit,
                normalPath: control.normalAssetPath,
                pressedPath: control.pressedAssetPath,
                assetsDirectory: assetsDirectory,
                descriptor: descriptor
            )
        case .dpad:
            v2DPadView(visualRect: visual, hitRect: hit, normalPath: control.normalAssetPath, pressedPath: control.pressedAssetPath, directional: control.directional, assetsDirectory: assetsDirectory)
        case .thumbstick(let side, _):
            if legacyThumbsticks && !(side == .right && swipeCamera) {
                v2StickView(visualRect: visual, captureDiameter: min(hit.width, hit.height), normalPath: control.normalAssetPath, knobPath: control.knobAssetPath, knobSize: knobSize, assetsDirectory: assetsDirectory, side: side)
                    .id(inputSessionGeneration)
            }
        }
    }

    @ViewBuilder
    private func v2DPadView(visualRect: CGRect, hitRect: CGRect, normalPath: String?, pressedPath: String?, directional: SkinManifestRuntimeLayout.DirectionalAssetPaths?, assetsDirectory: URL) -> some View {
        let minDimension = min(visualRect.width, visualRect.height)
        let halfStep = minDimension * 0.29
        let arrowSize = minDimension * 0.42
        let centroid = CGPoint(x: visualRect.midX, y: visualRect.midY)
        let faces: [CompositeDPadFaceInfo] = [
            .init(button: .up, label: "\u{25b2}", center: CGPoint(x: centroid.x, y: centroid.y - halfStep), baseSize: arrowSize, visibleScaleX: 1, visibleScaleY: 1, hitScaleX: 1, hitScaleY: 1),
            .init(button: .down, label: "\u{25bc}", center: CGPoint(x: centroid.x, y: centroid.y + halfStep), baseSize: arrowSize, visibleScaleX: 1, visibleScaleY: 1, hitScaleX: 1, hitScaleY: 1),
            .init(button: .left, label: "\u{25c0}", center: CGPoint(x: centroid.x - halfStep, y: centroid.y), baseSize: arrowSize, visibleScaleX: 1, visibleScaleY: 1, hitScaleX: 1, hitScaleY: 1),
            .init(button: .right, label: "\u{25b6}", center: CGPoint(x: centroid.x + halfStep, y: centroid.y), baseSize: arrowSize, visibleScaleX: 1, visibleScaleY: 1, hitScaleX: 1, hitScaleY: 1)
        ]
        let captureDiameter = max(min(hitRect.width, hitRect.height), minDimension)
        let deadzone = minDimension * 0.12
        let normalImage = normalPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }
        let pressedImage = pressedPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }
        let directionalArt: [ARMSX2PadButton: DirectionalFaceArt]? = directional.map { dirs in
            func resolve(_ d: SkinManifestRuntimeLayout.DirectionalAssetPath) -> DirectionalFaceArt {
                DirectionalFaceArt(
                    normal: d.normalPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) },
                    pressed: d.pressedPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }
                )
            }
            return [
                .up: resolve(dirs.up),
                .down: resolve(dirs.down),
                .left: resolve(dirs.left),
                .right: resolve(dirs.right)
            ]
        }
        CompositeDPadView(
            faces: faces,
            centroid: centroid,
            captureDiameter: captureDiameter,
            deadzone: deadzone,
            backgroundNormal: normalImage,
            backgroundPressed: pressedImage,
            backgroundFrame: visualRect,
            directional: directionalArt
        )
        .environment(\.padUsesFullSkin, true)
    }

    @ViewBuilder
    private func v2StickView(visualRect: CGRect, captureDiameter: CGFloat, normalPath: String?, knobPath: String?, knobSize: CGSize?, assetsDirectory: URL, side: SkinManifestRuntimeLayout.Side) -> some View {
        let target = min(visualRect.width, visualRect.height)
        let sizeScale = min(max(target / 68.0, 0.8), 1.6)
        let knobImage = knobPath.flatMap({ SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) })
        ZStack {
            if let baseImage = normalPath.flatMap({ SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }) {
                Image(uiImage: baseImage)
                    .resizable()
                    .interpolation(.high)
                    .antialiased(true)
                    .scaledToFit()
                    .frame(width: visualRect.width, height: visualRect.height)
                    .allowsHitTesting(false)
            }
            StickView(isLeft: side == .left, sizeScale: sizeScale, layoutScale: 1.0, captureDiameter: captureDiameter, knobImage: knobImage, knobSize: knobSize)
        }
        .position(x: visualRect.midX, y: visualRect.midY)
    }
}

// MARK: - Manifest v2 single button
private struct SkinManifestButtonView: View {
    let button: ARMSX2PadButton
    let visualRect: CGRect
    let hitRect: CGRect
    let normalPath: String?
    let pressedPath: String?
    let assetsDirectory: URL
    let descriptor: VPadSkinDescriptor
    @State private var normalImage: UIImage? = nil
    @State private var pressedImage: UIImage? = nil
    @State private var on = false
    @Environment(\.padOpacity) private var padOpacity

    private var symbol: String {
        switch button {
        case .up: return "\u{25b2}"
        case .down: return "\u{25bc}"
        case .left: return "\u{25c0}"
        case .right: return "\u{25b6}"
        case .cross: return "\u{2715}"
        case .circle: return "\u{25cb}"
        case .square: return "\u{25a1}"
        case .triangle: return "\u{25b3}"
        case .L1: return "L1"
        case .L2: return "L2"
        case .R1: return "R1"
        case .R2: return "R2"
        case .start: return "START"
        case .select: return "SEL"
        case .L3: return "L3"
        case .R3: return "R3"
        @unknown default: return ""
        }
    }

    private var accessibilityName: String {
        switch button {
        case .up: return "D-pad up"
        case .down: return "D-pad down"
        case .left: return "D-pad left"
        case .right: return "D-pad right"
        case .triangle: return "Triangle"
        case .cross: return "Cross"
        case .square: return "Square"
        case .circle: return "Circle"
        case .L1: return "L1"
        case .R1: return "R1"
        case .L2: return "L2"
        case .R2: return "R2"
        case .start: return "Start"
        case .select: return "Select"
        case .L3: return "L3"
        case .R3: return "R3"
        @unknown default: return symbol
        }
    }

    var body: some View {
        ZStack {
            Color.clear
                .frame(width: hitRect.width, height: hitRect.height)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { _ in setPressed(true) }
                        .onEnded { _ in setPressed(false) }
                )
                .position(x: hitRect.midX, y: hitRect.midY)

            visualFace
                .frame(width: visualRect.width, height: visualRect.height)
                .position(x: visualRect.midX, y: visualRect.midY)
                .allowsHitTesting(false)
                .opacity(padOpacity)
        }
        .onDisappear { setPressed(false) }
        .task(id: normalPath ?? "") {
            normalImage = normalPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }
        }
        .task(id: pressedPath ?? "") {
            pressedImage = pressedPath.flatMap { SkinManifestRuntimeLayout.image(forRelativePath: $0, in: assetsDirectory) }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(accessibilityName)
        .accessibilityAddTraits(.isButton)
    }

    @ViewBuilder
    private var visualFace: some View {
        if on, let pressed = pressedImage ?? normalImage {
            Image(uiImage: pressed)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
                .brightness(0.12)
                .scaleEffect(0.94)
        } else if let normal = normalImage {
            Image(uiImage: normal)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
        } else {
            ControllerAssetImage(
                fileName: ControllerAsset.fileName(for: button),
                fallback: symbol,
                fallbackColor: on ? .white : .white.opacity(0.85),
                fallbackFontSize: min(visualRect.width, visualRect.height) * 0.4,
                skin: .custom,
                descriptor: descriptor
            )
            .brightness(on ? 0.18 : 0)
            .scaleEffect(on ? 0.92 : 1.0)
        }
    }

    private func setPressed(_ pressed: Bool) {
        guard on != pressed else { return }
        on = pressed
        EmulatorBridge.shared.setPadButton(button, pressed: pressed)
        if pressed, SettingsStore.shared.hapticFeedback {
            GameEventHaptics.shared.medium.impactOccurred()
        }
    }
}
