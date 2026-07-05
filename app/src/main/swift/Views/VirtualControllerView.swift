// VirtualControllerView.swift — PS2 DualShock2 virtual controller
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

// Singleton haptic generator — prepared once, reused for all button presses
@MainActor
enum HapticManager {
    static let medium: UIImpactFeedbackGenerator = {
        let g = UIImpactFeedbackGenerator(style: .medium)
        g.prepare()
        return g
    }()
    static let light: UIImpactFeedbackGenerator = {
        let g = UIImpactFeedbackGenerator(style: .light)
        g.prepare()
        return g
    }()
}

struct VirtualControllerView: View {
    @State private var settings = SettingsStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var layout = PadLayoutStore.shared
    var isLandscape: Bool = false
    var layoutSnapshot: PadLayoutSnapshot? = nil
    var skinDescriptor: VPadSkinDescriptor? = nil

    private var analogStickScale: CGFloat {
        min(max(CGFloat(settings.analogStickScale), 0.8), 1.6)
    }

    private var effectiveSkinDescriptor: VPadSkinDescriptor {
        skinDescriptor ?? skinLibrary.selectedDescriptor
    }

    var body: some View {
        GeometryReader { geo in
            let descriptor = effectiveSkinDescriptor
            let skin = descriptor.virtualPadSkin
            let usesFullSkin = ControllerAsset.gameplayFullSkinImage(descriptor: descriptor, isLandscape: isLandscape) != nil

            if isLandscape {
                landscapeLayout(w: geo.size.width, h: geo.size.height)
                    .environment(\.padOpacity, Double(settings.padOpacity))
                    .environment(\.padSkin, skin)
                    .environment(\.padSkinDescriptor, descriptor)
                    .environment(\.padUsesFullSkin, usesFullSkin)
            } else {
                portraitLayout(w: geo.size.width, h: geo.size.height)
                    .environment(\.padOpacity, Double(settings.padOpacity))
                    .environment(\.padSkin, skin)
                    .environment(\.padSkinDescriptor, descriptor)
                    .environment(\.padUsesFullSkin, usesFullSkin)
            }
        }
        // Prepare mask images before gameplay input so the first press cannot decode/scan on the hot path.
        .onAppear {
            ARMSX2VirtualPadMaskImageCache.prewarm(descriptor: effectiveSkinDescriptor)
        }
        .onChange(of: skinLibrary.selectedSkinID) { _, _ in
            ARMSX2VirtualPadMaskImageCache.prewarm(descriptor: effectiveSkinDescriptor)
        }
        .onChange(of: skinDescriptor) { _, _ in
            ARMSX2VirtualPadMaskImageCache.prewarm(descriptor: effectiveSkinDescriptor)
        }

    }

    private func pos(_ id: String, landscape: Bool) -> PadGroupPosition {
        layoutSnapshot?.position(for: id, landscape: landscape) ?? layout.position(for: id, landscape: landscape)
    }

    private func perButtonPos(_ id: String, landscape: Bool, w: CGFloat, h: CGFloat) -> PadGroupPosition {
        layoutSnapshot?.perButtonPosition(for: id, landscape: landscape, areaW: w, areaH: h)
            ?? layout.perButtonPosition(for: id, landscape: landscape, areaW: w, areaH: h)
    }

    private func isVisible(_ id: String) -> Bool {
        layoutSnapshot?.isControlVisible(id) ?? layout.isControlVisible(id)
    }

    @ViewBuilder
    private func placedPadButton(
        id: String,
        label: String,
        w: CGFloat,
        h: CGFloat,
        btn: ARMSX2PadButton,
        landscape: Bool,
        areaW: CGFloat,
        areaH: CGFloat,
        perButton: Bool = false
    ) -> some View {
        let p = perButton ? perButtonPos(id, landscape: landscape, w: areaW, h: areaH) : pos(id, landscape: landscape)
        PadBtn(label: label, w: w, h: h, btn: btn, visibleScaleX: p.scaleX, visibleScaleY: p.scaleY, hitScaleX: p.hitScaleX, hitScaleY: p.hitScaleY)
            .position(x: p.x * areaW, y: p.y * areaH)
    }

    // Composite 8-way D-pad used when D-pad diagonals are enabled. Derives the
    // capture circle and center deadzone from the four cardinal button positions
    // so it follows custom layouts, per-orientation sizing, and hit scaling.
    private func placedCompositeDPad(landscape: Bool, areaW: CGFloat, areaH: CGFloat) -> some View {
        let entries: [(id: String, button: ARMSX2PadButton, label: String)] = [
            ("up", .up, "▲"), ("down", .down, "▼"), ("left", .left, "◀"), ("right", .right, "▶")
        ]
        let dpadW = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: landscape)

        var faces: [CompositeDPadFaceInfo] = []
        var centers: [CGPoint] = []
        for entry in entries {
            let p = perButtonPos(entry.id, landscape: landscape, w: areaW, h: areaH)
            let center = CGPoint(x: p.x * areaW, y: p.y * areaH)
            centers.append(center)
            faces.append(CompositeDPadFaceInfo(
                button: entry.button,
                label: entry.label,
                center: center,
                baseSize: dpadW,
                visibleScaleX: p.scaleX,
                visibleScaleY: p.scaleY,
                hitScaleX: p.hitScaleX,
                hitScaleY: p.hitScaleY
            ))
        }

        let centroid = CGPoint(
            x: centers.reduce(0, { $0 + $1.x }) / CGFloat(centers.count),
            y: centers.reduce(0, { $0 + $1.y }) / CGFloat(centers.count)
        )
        let distances = centers.map { hypot($0.x - centroid.x, $0.y - centroid.y) }
        let maxTouchHalf = faces.map { PadLayoutMetrics.touchLength(baseLength: dpadW, hitScale: $0.hitScaleX) / 2 }.max() ?? 0
        let captureRadius = (distances.max() ?? 0) + maxTouchHalf
        let nearestDistance = distances.min() ?? captureRadius
        let deadzone = nearestDistance * 0.20

        return CompositeDPadView(
            faces: faces,
            centroid: centroid,
            captureDiameter: captureRadius * 2,
            deadzone: deadzone
        )
    }

    // Composite face-button cluster used when face-button combo zones are enabled.
    // Derives its centroid and capture circle from the four action-button positions
    // so it follows custom layouts, per-axis sizing, and hit scaling.
    private func placedCompositeFaceButtons(landscape: Bool, areaW: CGFloat, areaH: CGFloat) -> some View {
        let entries: [(id: String, button: ARMSX2PadButton, sym: String, clr: Color)] = [
            ("triangle", .triangle, "△", .green),
            ("cross", .cross, "✕", .blue),
            ("square", .square, "□", .pink),
            ("circle", .circle, "○", .red)
        ]
        let actionSz = VirtualPadButtonOffset.actionButtonSize

        var faces: [CompositeFaceButtonInfo] = []
        var centers: [CGPoint] = []
        for entry in entries {
            let p = perButtonPos(entry.id, landscape: landscape, w: areaW, h: areaH)
            let center = CGPoint(x: p.x * areaW, y: p.y * areaH)
            centers.append(center)
            faces.append(CompositeFaceButtonInfo(
                button: entry.button,
                symbol: entry.sym,
                color: entry.clr,
                center: center,
                baseSize: actionSz,
                visibleScaleX: p.scaleX,
                visibleScaleY: p.scaleY,
                hitScaleX: p.hitScaleX,
                hitScaleY: p.hitScaleY
            ))
        }

        let centroid = CGPoint(
            x: centers.reduce(0, { $0 + $1.x }) / CGFloat(centers.count),
            y: centers.reduce(0, { $0 + $1.y }) / CGFloat(centers.count)
        )
        let captureRadius = faces.map { face in
            let centerDistance = hypot(face.center.x - centroid.x, face.center.y - centroid.y)
            let halfW = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleX) / 2
            let halfH = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleY) / 2
            return centerDistance + hypot(halfW, halfH)
        }.max() ?? 0

        return CompositeFaceView(
            faces: faces,
            centroid: centroid,
            captureDiameter: captureRadius * 2
        )
    }

    @ViewBuilder
    private func placedPSButton(
        id: String,
        sym: String,
        clr: Color,
        sz: CGFloat,
        btn: ARMSX2PadButton,
        landscape: Bool,
        areaW: CGFloat,
        areaH: CGFloat
    ) -> some View {
        let p = perButtonPos(id, landscape: landscape, w: areaW, h: areaH)
        PSBtn(sym: sym, clr: clr, sz: sz, btn: btn, visibleScaleX: p.scaleX, visibleScaleY: p.scaleY, hitScaleX: p.hitScaleX, hitScaleY: p.hitScaleY)
            .position(x: p.x * areaW, y: p.y * areaH)
    }

    @ViewBuilder
    private func placedStick(
        id: String,
        isLeft: Bool,
        landscape: Bool,
        areaW: CGFloat,
        areaH: CGFloat
    ) -> some View {
        let p = pos(id, landscape: landscape)
        StickView(isLeft: isLeft, sizeScale: analogStickScale, layoutScale: p.scale)
            .position(x: p.x * areaW, y: p.y * areaH)
    }

    // MARK: - Landscape: overlay on game screen
    @ViewBuilder
    func landscapeLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            if let fullSkin = ControllerAsset.gameplayFullSkinImage(descriptor: effectiveSkinDescriptor, isLandscape: true) {
                Image(uiImage: fullSkin)
                    .resizable()
                    .interpolation(.high)
                    .antialiased(true)
                    .scaledToFill()
                    .frame(width: w, height: h)
                    .clipped()
                    .allowsHitTesting(false)
            }

            // D-pad buttons
            if isVisible("dpad") {
                if settings.dpadDiagonalsEnabled {
                    placedCompositeDPad(landscape: true, areaW: w, areaH: h)
                } else {
                    let dpadW = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: true)
                    placedPadButton(id: "up", label: "▲", w: dpadW, h: dpadW, btn: .up, landscape: true, areaW: w, areaH: h, perButton: true)
                    placedPadButton(id: "down", label: "▼", w: dpadW, h: dpadW, btn: .down, landscape: true, areaW: w, areaH: h, perButton: true)
                    placedPadButton(id: "left", label: "◀", w: dpadW, h: dpadW, btn: .left, landscape: true, areaW: w, areaH: h, perButton: true)
                    placedPadButton(id: "right", label: "▶", w: dpadW, h: dpadW, btn: .right, landscape: true, areaW: w, areaH: h, perButton: true)
                }
            }

            // Action buttons: composite combo surface when enabled (and all four face
            // buttons are visible); otherwise individual buttons with per-button visibility.
            if settings.faceComboZonesEnabled,
               isVisible("triangle") && isVisible("cross") && isVisible("square") && isVisible("circle") {
                placedCompositeFaceButtons(landscape: true, areaW: w, areaH: h)
            } else {
                let actionSz = VirtualPadButtonOffset.actionButtonSize
                if isVisible("triangle") {
                    placedPSButton(id: "triangle", sym: "△", clr: .green, sz: actionSz, btn: .triangle, landscape: true, areaW: w, areaH: h)
                }
                if isVisible("cross") {
                    placedPSButton(id: "cross", sym: "✕", clr: .blue, sz: actionSz, btn: .cross, landscape: true, areaW: w, areaH: h)
                }
                if isVisible("square") {
                    placedPSButton(id: "square", sym: "□", clr: .pink, sz: actionSz, btn: .square, landscape: true, areaW: w, areaH: h)
                }
                if isVisible("circle") {
                    placedPSButton(id: "circle", sym: "○", clr: .red, sz: actionSz, btn: .circle, landscape: true, areaW: w, areaH: h)
                }
            }

            if isVisible("l2") {
                placedPadButton(id: "l2", label: "L2", w: 130, h: 44, btn: .L2, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("l1") {
                placedPadButton(id: "l1", label: "L1", w: 120, h: 32, btn: .L1, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("r2") {
                placedPadButton(id: "r2", label: "R2", w: 130, h: 44, btn: .R2, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("r1") {
                placedPadButton(id: "r1", label: "R1", w: 120, h: 32, btn: .R1, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("select") {
                placedPadButton(id: "select", label: "SEL", w: 40, h: 22, btn: .select, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("start") {
                placedPadButton(id: "start", label: "START", w: 48, h: 22, btn: .start, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("lstick") {
                placedStick(id: "lstick", isLeft: true, landscape: true, areaW: w, areaH: h)
            }
            if isVisible("rstick") {
                placedStick(id: "rstick", isLeft: false, landscape: true, areaW: w, areaH: h)
            }
        }
    }

    // MARK: - Portrait: controller fills its given area
    @ViewBuilder
    func portraitLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            if let fullSkin = ControllerAsset.gameplayFullSkinImage(descriptor: effectiveSkinDescriptor, isLandscape: false) {
                Image(uiImage: fullSkin)
                    .resizable()
                    .interpolation(.high)
                    .antialiased(true)
                    .scaledToFill()
                    .frame(width: w, height: h)
                    .clipped()
                    .allowsHitTesting(false)
            }

            GeometryReader { cGeo in
                let cW = cGeo.size.width
                let cH = cGeo.size.height

                if isVisible("l2") {
                    placedPadButton(id: "l2", label: "L2", w: 110, h: 40, btn: .L2, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("l1") {
                    placedPadButton(id: "l1", label: "L1", w: 100, h: 30, btn: .L1, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("r2") {
                    placedPadButton(id: "r2", label: "R2", w: 110, h: 40, btn: .R2, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("r1") {
                    placedPadButton(id: "r1", label: "R1", w: 100, h: 30, btn: .R1, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("select") {
                    placedPadButton(id: "select", label: "SEL", w: 42, h: 22, btn: .select, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("start") {
                    placedPadButton(id: "start", label: "START", w: 48, h: 22, btn: .start, landscape: false, areaW: cW, areaH: cH)
                }

                // D-pad buttons
                if isVisible("dpad") {
                    if settings.dpadDiagonalsEnabled {
                        placedCompositeDPad(landscape: false, areaW: cW, areaH: cH)
                    } else {
                        let dpadW = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: false)
                        placedPadButton(id: "up", label: "▲", w: dpadW, h: dpadW, btn: .up, landscape: false, areaW: cW, areaH: cH, perButton: true)
                        placedPadButton(id: "down", label: "▼", w: dpadW, h: dpadW, btn: .down, landscape: false, areaW: cW, areaH: cH, perButton: true)
                        placedPadButton(id: "left", label: "◀", w: dpadW, h: dpadW, btn: .left, landscape: false, areaW: cW, areaH: cH, perButton: true)
                        placedPadButton(id: "right", label: "▶", w: dpadW, h: dpadW, btn: .right, landscape: false, areaW: cW, areaH: cH, perButton: true)
                    }
                }

                // Action buttons: composite combo surface when enabled (and all four face
                // buttons are visible); otherwise individual buttons with per-button visibility.
                if settings.faceComboZonesEnabled,
                   isVisible("triangle") && isVisible("cross") && isVisible("square") && isVisible("circle") {
                    placedCompositeFaceButtons(landscape: false, areaW: cW, areaH: cH)
                } else {
                    let actionSz = VirtualPadButtonOffset.actionButtonSize
                    if isVisible("triangle") {
                        placedPSButton(id: "triangle", sym: "△", clr: .green, sz: actionSz, btn: .triangle, landscape: false, areaW: cW, areaH: cH)
                    }
                    if isVisible("cross") {
                        placedPSButton(id: "cross", sym: "✕", clr: .blue, sz: actionSz, btn: .cross, landscape: false, areaW: cW, areaH: cH)
                    }
                    if isVisible("square") {
                        placedPSButton(id: "square", sym: "□", clr: .pink, sz: actionSz, btn: .square, landscape: false, areaW: cW, areaH: cH)
                    }
                    if isVisible("circle") {
                        placedPSButton(id: "circle", sym: "○", clr: .red, sz: actionSz, btn: .circle, landscape: false, areaW: cW, areaH: cH)
                    }
                }

                if isVisible("lstick") {
                    placedStick(id: "lstick", isLeft: true, landscape: false, areaW: cW, areaH: cH)
                }
                if isVisible("rstick") {
                    placedStick(id: "rstick", isLeft: false, landscape: false, areaW: cW, areaH: cH)
                }
            }
        }
    }
}
