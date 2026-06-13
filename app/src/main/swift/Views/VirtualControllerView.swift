// VirtualControllerView.swift — PS2 DualShock2 virtual controller
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private struct PadOpacityKey: EnvironmentKey {
    static let defaultValue: Double = 1.0
}

private struct PadSkinKey: EnvironmentKey {
    static let defaultValue: VirtualPadSkin = .armsx2Refresh
}

private struct PadUsesFullSkinKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var padOpacity: Double {
        get { self[PadOpacityKey.self] }
        set { self[PadOpacityKey.self] = newValue }
    }

    var padSkin: VirtualPadSkin {
        get { self[PadSkinKey.self] }
        set { self[PadSkinKey.self] = newValue }
    }

    var padUsesFullSkin: Bool {
        get { self[PadUsesFullSkinKey.self] }
        set { self[PadUsesFullSkinKey.self] = newValue }
    }
}

// U005: Singleton haptic generator — prepared once, reused for all button presses
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

enum ControllerAsset {
    private static let edgeToEdgePortraitAspectRatio: CGFloat = 1.55

    static func fileName(for button: ARMSX2PadButton) -> String {
        switch button {
        case .up:       return "ic_controller_up_button.png"
        case .down:     return "ic_controller_down_button.png"
        case .left:     return "ic_controller_left_button.png"
        case .right:    return "ic_controller_right_button.png"
        case .cross:    return "ic_controller_cross_button.png"
        case .circle:   return "ic_controller_circle_button.png"
        case .square:   return "ic_controller_square_button.png"
        case .triangle: return "ic_controller_triangle_button.png"
        case .L1:       return "ic_controller_l1_button.png"
        case .R1:       return "ic_controller_r1_button.png"
        case .L2:       return "ic_controller_l2_button.png"
        case .R2:       return "ic_controller_r2_button.png"
        case .start:    return "ic_controller_start_button.png"
        case .select:   return "ic_controller_select_button.png"
        case .L3:       return "ic_controller_l3_button.png"
        case .R3:       return "ic_controller_r3_button.png"
        @unknown default:
            return ""
        }
    }

    static func image(named fileName: String, skin: VirtualPadSkin) -> UIImage? {
        guard !fileName.isEmpty else { return nil }

        let baseName = (fileName as NSString).deletingPathExtension
        if skin == .custom,
           let directory = VirtualPadSkin.customSkinDirectory(),
           let customImage = customImage(named: fileName, baseName: baseName, directory: directory) {
            return customImage
        }

        if let directoryName = skin.bundledDirectoryName,
           let bundledImage = bundledSkinImage(named: baseName, directoryName: directoryName) {
            return bundledImage
        }

        if let image = UIImage(named: baseName) ?? UIImage(named: fileName) {
            return image
        }

        guard let path = Bundle.main.path(forResource: baseName, ofType: "png") else {
            return nil
        }

        return UIImage(contentsOfFile: path)
    }

    private static func bundledSkinImage(named baseName: String, directoryName: String) -> UIImage? {
        let subdirectory = "controller_skins/\(directoryName)"
        guard let url = Bundle.main.url(forResource: baseName, withExtension: "png", subdirectory: subdirectory) else {
            return nil
        }

        return UIImage(contentsOfFile: url.path)
    }

    static func fullSkinImage(skin: VirtualPadSkin, isLandscape: Bool) -> UIImage? {
        guard skin == .custom, let directory = VirtualPadSkin.customSkinDirectory() else {
            return nil
        }

        let orientationCandidates = isLandscape
            ? ["controller_edgetoedge_landscape", "iphone_edgetoedge_landscape", "controller_landscape", "iphone_landscape", "skin_landscape", "background_landscape", "gamepad_landscape", "landscape"]
            : ["controller_edgetoedge_portrait", "iphone_edgetoedge_portrait", "controller_portrait", "iphone_portrait", "skin_portrait", "background_portrait", "gamepad_portrait", "portrait"]
        let sharedCandidates = ["controller", "skin", "background", "gamepad", "full", "layout"]

        for baseName in orientationCandidates + sharedCandidates {
            if let image = customImage(named: "\(baseName).png", baseName: baseName, directory: directory) {
                return image
            }
        }

        return nil
    }

    static func gameplayFullSkinImage(skin: VirtualPadSkin, isLandscape: Bool) -> UIImage? {
        guard skin == .custom, let directory = VirtualPadSkin.customSkinDirectory() else {
            return nil
        }

        // Manic/Delta-style full-phone skins include their own game viewport and
        // need info.json coordinates before we can place them accurately. For
        // gameplay, only use simple pad-area skins; otherwise fall back to the
        // built-in ARMSX2 controls so inputs never become visually misleading.
        guard !isLandscape else {
            return nil
        }

        let candidates = [
            "controller_portrait",
            "skin_portrait",
            "background_portrait",
            "gamepad_portrait",
            "portrait",
            "controller",
            "skin",
            "background",
            "gamepad",
            "full",
            "layout"
        ]

        for baseName in candidates {
            if let image = customImage(named: "\(baseName).png", baseName: baseName, directory: directory),
               !looksLikeEdgeToEdgePortrait(image) {
                return image
            }
        }

        return nil
    }

    static func edgeToEdgePortraitSkinImage(skin: VirtualPadSkin) -> UIImage? {
        guard let image = fullSkinImage(skin: skin, isLandscape: false) else {
            return nil
        }

        return looksLikeEdgeToEdgePortrait(image) ? image : nil
    }

    private static func looksLikeEdgeToEdgePortrait(_ image: UIImage) -> Bool {
        let aspect = image.size.height / max(image.size.width, 1)
        return aspect >= edgeToEdgePortraitAspectRatio
    }

    private static func customImage(named fileName: String, baseName: String, directory: URL) -> UIImage? {
        let candidates = [
            fileName,
            "\(baseName).png",
            "\(baseName).jpg",
            "\(baseName).jpeg",
            "\(baseName).webp"
        ]

        for candidate in candidates {
            let url = directory.appendingPathComponent(candidate)
            if FileManager.default.fileExists(atPath: url.path),
               let image = UIImage(contentsOfFile: url.path) {
                return image
            }
        }

        return nil
    }
}

private struct ControllerAssetImage: View {
    let fileName: String
    let fallback: String
    let fallbackColor: Color
    let fallbackFontSize: CGFloat
    let skin: VirtualPadSkin

    var body: some View {
        if skin == .crispVector {
            ControllerVectorGlyph(
                fileName: fileName,
                fallback: fallback,
                fallbackColor: fallbackColor,
                fallbackFontSize: fallbackFontSize
            )
        } else if let image = ControllerAsset.image(named: fileName, skin: skin) {
            Image(uiImage: image)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
        } else {
            Text(fallback)
                .font(.system(size: fallbackFontSize, weight: .semibold))
                .foregroundStyle(fallbackColor)
                .minimumScaleFactor(0.5)
        }
    }
}

private struct ControllerVectorGlyph: View {
    let fileName: String
    let fallback: String
    let fallbackColor: Color
    let fallbackFontSize: CGFloat

    var body: some View {
        let lowerName = fileName.lowercased()

        GeometryReader { geo in
            if lowerName.contains("analog_base") {
                AnalogBaseGlyph()
            } else if lowerName.contains("analog_stick") || lowerName.contains("analog_button") {
                AnalogStickGlyph()
            } else if let face = FaceGlyph.Kind(fileName: lowerName) {
                FaceGlyph(kind: face)
            } else if let direction = DPadGlyph.Direction(fileName: lowerName) {
                DPadGlyph(direction: direction)
            } else if lowerName.contains("start") {
                CapsuleGlyph(label: fallback.isEmpty ? "START" : fallback, symbol: .play)
            } else if lowerName.contains("select") {
                CapsuleGlyph(label: fallback.isEmpty ? "SEL" : fallback, symbol: .minus)
            } else if lowerName.contains("l3") || lowerName.contains("r3") {
                CircleLabelGlyph(label: fallback)
            } else if lowerName.contains("l1") || lowerName.contains("l2") || lowerName.contains("r1") || lowerName.contains("r2") {
                ShoulderGlyph(label: fallback)
            } else {
                Text(fallback)
                    .font(.system(size: fallbackFontSize, weight: .semibold))
                    .foregroundStyle(fallbackColor)
                    .minimumScaleFactor(0.5)
            }
        }
    }
}

private struct FaceGlyph: View {
    enum Kind {
        case cross
        case circle
        case square
        case triangle

        init?(fileName: String) {
            if fileName.contains("cross") {
                self = .cross
            } else if fileName.contains("circle") {
                self = .circle
            } else if fileName.contains("square") {
                self = .square
            } else if fileName.contains("triangle") {
                self = .triangle
            } else {
                return nil
            }
        }

        var color: Color {
            switch self {
            case .cross:
                return Color(red: 0.18, green: 0.43, blue: 1.0)
            case .circle:
                return Color(red: 0.86, green: 0.0, blue: 0.0)
            case .square:
                return Color(red: 1.0, green: 0.0, blue: 1.0)
            case .triangle:
                return Color(red: 0.0, green: 0.78, blue: 0.33)
            }
        }
    }

    let kind: Kind

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let lineWidth = max(2, side * 0.08)

            ZStack {
                Circle()
                    .fill(.black.opacity(0.08))
                    .stroke(.white.opacity(0.52), lineWidth: max(1.2, side * 0.04))

                switch kind {
                case .cross:
                    CrossShape()
                        .stroke(kind.color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                        .frame(width: side * 0.42, height: side * 0.42)
                case .circle:
                    Circle()
                        .stroke(kind.color, lineWidth: lineWidth)
                        .frame(width: side * 0.48, height: side * 0.48)
                case .square:
                    Rectangle()
                        .stroke(kind.color, lineWidth: lineWidth)
                        .frame(width: side * 0.46, height: side * 0.46)
                case .triangle:
                    TriangleShape()
                        .stroke(kind.color, style: StrokeStyle(lineWidth: lineWidth, lineJoin: .round))
                        .frame(width: side * 0.58, height: side * 0.48)
                        .offset(y: -side * 0.02)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

private struct DPadGlyph: View {
    enum Direction {
        case up
        case down
        case left
        case right

        init?(fileName: String) {
            if fileName.contains("_up_") {
                self = .up
            } else if fileName.contains("_down_") {
                self = .down
            } else if fileName.contains("_left_") {
                self = .left
            } else if fileName.contains("_right_") {
                self = .right
            } else {
                return nil
            }
        }

        var angle: Angle {
            switch self {
            case .up:
                return .degrees(0)
            case .right:
                return .degrees(90)
            case .down:
                return .degrees(180)
            case .left:
                return .degrees(270)
            }
        }
    }

    let direction: Direction

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)

            ZStack {
                RoundedRectangle(cornerRadius: side * 0.20, style: .continuous)
                    .fill(.black.opacity(0.18))
                    .stroke(.white.opacity(0.22), lineWidth: max(1.2, side * 0.045))

                TriangleShape()
                    .fill(.white.opacity(0.72))
                    .frame(width: side * 0.30, height: side * 0.24)
                    .rotationEffect(direction.angle)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

private struct ShoulderGlyph: View {
    let label: String

    var body: some View {
        GeometryReader { geo in
            let corner = min(geo.size.height * 0.28, 14)

            ZStack {
                RoundedRectangle(cornerRadius: corner, style: .continuous)
                    .fill(.black.opacity(0.16))
                    .stroke(.white.opacity(0.28), lineWidth: 1.6)

                Text(label)
                    .font(.system(size: max(11, geo.size.height * 0.42), weight: .semibold, design: .rounded))
                    .foregroundStyle(.white.opacity(0.76))
                    .minimumScaleFactor(0.55)
            }
        }
    }
}

private struct CapsuleGlyph: View {
    enum Symbol {
        case play
        case minus
    }

    let label: String
    let symbol: Symbol

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Capsule()
                    .fill(.black.opacity(0.14))
                    .stroke(.white.opacity(0.26), lineWidth: 1.4)

                if geo.size.width < 34 {
                    symbolView
                } else {
                    Text(label)
                        .font(.system(size: max(8, geo.size.height * 0.42), weight: .semibold, design: .rounded))
                        .foregroundStyle(.white.opacity(0.72))
                        .minimumScaleFactor(0.45)
                }
            }
        }
    }

    @ViewBuilder
    private var symbolView: some View {
        switch symbol {
        case .play:
            TriangleShape()
                .fill(.white.opacity(0.72))
                .rotationEffect(.degrees(90))
                .padding(6)
        case .minus:
            Capsule()
                .fill(.white.opacity(0.72))
                .frame(width: 14, height: 3)
        }
    }
}

private struct CircleLabelGlyph: View {
    let label: String

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)

            ZStack {
                Circle()
                    .fill(.black.opacity(0.16))
                    .stroke(.white.opacity(0.26), lineWidth: max(1, side * 0.055))

                Text(label)
                    .font(.system(size: max(6, side * 0.32), weight: .bold, design: .rounded))
                    .foregroundStyle(.white.opacity(0.54))
                    .minimumScaleFactor(0.4)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

private struct AnalogBaseGlyph: View {
    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)

            ZStack {
                Circle()
                    .fill(.black.opacity(0.14))
                    .stroke(.white.opacity(0.20), lineWidth: max(1, side * 0.018))

                Circle()
                    .stroke(.white.opacity(0.08), lineWidth: max(1, side * 0.035))
                    .frame(width: side * 0.78, height: side * 0.78)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

private struct AnalogStickGlyph: View {
    var body: some View {
        GeometryReader { geo in
            ZStack {
                Circle()
                    .fill(.white.opacity(0.22))
                    .stroke(.white.opacity(0.18), lineWidth: 1.2)

                Circle()
                    .fill(.white.opacity(0.10))
                    .padding(6)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}

private struct CrossShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        path.move(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        return path
    }
}

private struct TriangleShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

struct VirtualControllerView: View {
    @State private var settings = SettingsStore.shared
    @State private var layout = PadLayoutStore.shared
    var isLandscape: Bool = false
    var drawFullSkinBackground: Bool = true

    private var analogStickScale: CGFloat {
        min(max(CGFloat(settings.analogStickScale), 0.8), 1.6)
    }

    // A004: Scale buttons based on screen width (baseline: iPhone 15 = 393pt width)
    private func deviceScale(_ geo: GeometryProxy) -> CGFloat {
        let baseWidth: CGFloat = 393
        let w = isLandscape ? max(geo.size.width, geo.size.height) : min(geo.size.width, geo.size.height)
        return max(0.7, min(1.4, w / baseWidth))
    }

    var body: some View {
        GeometryReader { geo in
            let skin = settings.virtualPadSkin
            let usesFullSkin = ControllerAsset.gameplayFullSkinImage(skin: skin, isLandscape: isLandscape) != nil

            if isLandscape {
                landscapeLayout(w: geo.size.width, h: geo.size.height)
                    .environment(\.padOpacity, Double(settings.padOpacity))
                    .environment(\.padSkin, skin)
                    .environment(\.padUsesFullSkin, usesFullSkin)
            } else {
                portraitLayout(w: geo.size.width, h: geo.size.height)
                    .environment(\.padOpacity, Double(settings.padOpacity))
                    .environment(\.padSkin, skin)
                    .environment(\.padUsesFullSkin, usesFullSkin)
            }
        }
    }

    private func pos(_ id: String, landscape: Bool) -> PadGroupPosition {
        layout.position(for: id, landscape: landscape)
    }

    private func perButtonPos(_ id: String, landscape: Bool, w: CGFloat, h: CGFloat) -> PadGroupPosition {
        layout.perButtonPosition(for: id, landscape: landscape, areaW: w, areaH: h)
    }

    // MARK: - Landscape: overlay on game screen
    @ViewBuilder
    func landscapeLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            if drawFullSkinBackground,
               let fullSkin = ControllerAsset.gameplayFullSkinImage(skin: settings.virtualPadSkin, isLandscape: true) {
                Image(uiImage: fullSkin)
                    .resizable()
                    .interpolation(.high)
                    .antialiased(true)
                    .scaledToFill()
                    .frame(width: w, height: h)
                    .clipped()
                    .allowsHitTesting(false)
            }

            // D-pad buttons (individual placement)
            if layout.isControlVisible("dpad") {
                let dpadW = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: true)
                PadBtn(label: "▲", w: dpadW, h: dpadW, btn: .up)
                    .scaleEffect(perButtonPos("up", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("up", landscape: true, w: w, h: h).x * w, y: perButtonPos("up", landscape: true, w: w, h: h).y * h)
                PadBtn(label: "▼", w: dpadW, h: dpadW, btn: .down)
                    .scaleEffect(perButtonPos("down", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("down", landscape: true, w: w, h: h).x * w, y: perButtonPos("down", landscape: true, w: w, h: h).y * h)
                PadBtn(label: "◀", w: dpadW, h: dpadW, btn: .left)
                    .scaleEffect(perButtonPos("left", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("left", landscape: true, w: w, h: h).x * w, y: perButtonPos("left", landscape: true, w: w, h: h).y * h)
                PadBtn(label: "▶", w: dpadW, h: dpadW, btn: .right)
                    .scaleEffect(perButtonPos("right", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("right", landscape: true, w: w, h: h).x * w, y: perButtonPos("right", landscape: true, w: w, h: h).y * h)
            }

            // Action buttons (individual placement)
            let actionSz = VirtualPadButtonOffset.actionButtonSize
            if layout.isControlVisible("triangle") {
                PSBtn(sym: "△", clr: .green, sz: actionSz, btn: .triangle)
                    .scaleEffect(perButtonPos("triangle", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("triangle", landscape: true, w: w, h: h).x * w, y: perButtonPos("triangle", landscape: true, w: w, h: h).y * h)
            }
            if layout.isControlVisible("cross") {
                PSBtn(sym: "✕", clr: .blue, sz: actionSz, btn: .cross)
                    .scaleEffect(perButtonPos("cross", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("cross", landscape: true, w: w, h: h).x * w, y: perButtonPos("cross", landscape: true, w: w, h: h).y * h)
            }
            if layout.isControlVisible("square") {
                PSBtn(sym: "□", clr: .pink, sz: actionSz, btn: .square)
                    .scaleEffect(perButtonPos("square", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("square", landscape: true, w: w, h: h).x * w, y: perButtonPos("square", landscape: true, w: w, h: h).y * h)
            }
            if layout.isControlVisible("circle") {
                PSBtn(sym: "○", clr: .red, sz: actionSz, btn: .circle)
                    .scaleEffect(perButtonPos("circle", landscape: true, w: w, h: h).scale)
                    .position(x: perButtonPos("circle", landscape: true, w: w, h: h).x * w, y: perButtonPos("circle", landscape: true, w: w, h: h).y * h)
            }

            if layout.isControlVisible("l2") {
                PadBtn(label: "L2", w: 130, h: 44, btn: .L2)
                    .scaleEffect(pos("l2", landscape: true).scale)
                    .position(x: pos("l2", landscape: true).x * w, y: pos("l2", landscape: true).y * h)
            }
            if layout.isControlVisible("l1") {
                PadBtn(label: "L1", w: 120, h: 32, btn: .L1)
                    .scaleEffect(pos("l1", landscape: true).scale)
                    .position(x: pos("l1", landscape: true).x * w, y: pos("l1", landscape: true).y * h)
            }
            if layout.isControlVisible("r2") {
                PadBtn(label: "R2", w: 130, h: 44, btn: .R2)
                    .scaleEffect(pos("r2", landscape: true).scale)
                    .position(x: pos("r2", landscape: true).x * w, y: pos("r2", landscape: true).y * h)
            }
            if layout.isControlVisible("r1") {
                PadBtn(label: "R1", w: 120, h: 32, btn: .R1)
                    .scaleEffect(pos("r1", landscape: true).scale)
                    .position(x: pos("r1", landscape: true).x * w, y: pos("r1", landscape: true).y * h)
            }
            if layout.isControlVisible("select") {
                PadBtn(label: "SEL", w: 40, h: 22, btn: .select)
                    .scaleEffect(pos("select", landscape: true).scale)
                    .position(x: pos("select", landscape: true).x * w, y: pos("select", landscape: true).y * h)
            }
            if layout.isControlVisible("start") {
                PadBtn(label: "START", w: 48, h: 22, btn: .start)
                    .scaleEffect(pos("start", landscape: true).scale)
                    .position(x: pos("start", landscape: true).x * w, y: pos("start", landscape: true).y * h)
            }
            if layout.isControlVisible("lstick") {
                StickView(isLeft: true, sizeScale: analogStickScale)
                    .scaleEffect(pos("lstick", landscape: true).scale)
                    .position(x: pos("lstick", landscape: true).x * w, y: pos("lstick", landscape: true).y * h)
            }
            if layout.isControlVisible("rstick") {
                StickView(isLeft: false, sizeScale: analogStickScale)
                    .scaleEffect(pos("rstick", landscape: true).scale)
                    .position(x: pos("rstick", landscape: true).x * w, y: pos("rstick", landscape: true).y * h)
            }
        }
    }

    // MARK: - Portrait: controller fills its given area
    @ViewBuilder
    func portraitLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            if drawFullSkinBackground,
               let fullSkin = ControllerAsset.gameplayFullSkinImage(skin: settings.virtualPadSkin, isLandscape: false) {
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

                if layout.isControlVisible("l2") {
                    PadBtn(label: "L2", w: 110, h: 40, btn: .L2)
                        .scaleEffect(pos("l2", landscape: false).scale)
                        .position(x: pos("l2", landscape: false).x * cW, y: pos("l2", landscape: false).y * cH)
                }
                if layout.isControlVisible("l1") {
                    PadBtn(label: "L1", w: 100, h: 30, btn: .L1)
                        .scaleEffect(pos("l1", landscape: false).scale)
                        .position(x: pos("l1", landscape: false).x * cW, y: pos("l1", landscape: false).y * cH)
                }
                if layout.isControlVisible("r2") {
                    PadBtn(label: "R2", w: 110, h: 40, btn: .R2)
                        .scaleEffect(pos("r2", landscape: false).scale)
                        .position(x: pos("r2", landscape: false).x * cW, y: pos("r2", landscape: false).y * cH)
                }
                if layout.isControlVisible("r1") {
                    PadBtn(label: "R1", w: 100, h: 30, btn: .R1)
                        .scaleEffect(pos("r1", landscape: false).scale)
                        .position(x: pos("r1", landscape: false).x * cW, y: pos("r1", landscape: false).y * cH)
                }
                if layout.isControlVisible("select") {
                    PadBtn(label: "SEL", w: 42, h: 22, btn: .select)
                        .scaleEffect(pos("select", landscape: false).scale)
                        .position(x: pos("select", landscape: false).x * cW, y: pos("select", landscape: false).y * cH)
                }
                if layout.isControlVisible("start") {
                    PadBtn(label: "START", w: 48, h: 22, btn: .start)
                        .scaleEffect(pos("start", landscape: false).scale)
                        .position(x: pos("start", landscape: false).x * cW, y: pos("start", landscape: false).y * cH)
                }

                // D-pad buttons (individual placement)
                if layout.isControlVisible("dpad") {
                    let dpadW = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: false)
                    PadBtn(label: "▲", w: dpadW, h: dpadW, btn: .up)
                        .scaleEffect(perButtonPos("up", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("up", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("up", landscape: false, w: cW, h: cH).y * cH)
                    PadBtn(label: "▼", w: dpadW, h: dpadW, btn: .down)
                        .scaleEffect(perButtonPos("down", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("down", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("down", landscape: false, w: cW, h: cH).y * cH)
                    PadBtn(label: "◀", w: dpadW, h: dpadW, btn: .left)
                        .scaleEffect(perButtonPos("left", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("left", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("left", landscape: false, w: cW, h: cH).y * cH)
                    PadBtn(label: "▶", w: dpadW, h: dpadW, btn: .right)
                        .scaleEffect(perButtonPos("right", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("right", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("right", landscape: false, w: cW, h: cH).y * cH)
                }

                // Action buttons (individual placement)
                let actionSz = VirtualPadButtonOffset.actionButtonSize
                if layout.isControlVisible("triangle") {
                    PSBtn(sym: "△", clr: .green, sz: actionSz, btn: .triangle)
                        .scaleEffect(perButtonPos("triangle", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("triangle", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("triangle", landscape: false, w: cW, h: cH).y * cH)
                }
                if layout.isControlVisible("cross") {
                    PSBtn(sym: "✕", clr: .blue, sz: actionSz, btn: .cross)
                        .scaleEffect(perButtonPos("cross", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("cross", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("cross", landscape: false, w: cW, h: cH).y * cH)
                }
                if layout.isControlVisible("square") {
                    PSBtn(sym: "□", clr: .pink, sz: actionSz, btn: .square)
                        .scaleEffect(perButtonPos("square", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("square", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("square", landscape: false, w: cW, h: cH).y * cH)
                }
                if layout.isControlVisible("circle") {
                    PSBtn(sym: "○", clr: .red, sz: actionSz, btn: .circle)
                        .scaleEffect(perButtonPos("circle", landscape: false, w: cW, h: cH).scale)
                        .position(x: perButtonPos("circle", landscape: false, w: cW, h: cH).x * cW, y: perButtonPos("circle", landscape: false, w: cW, h: cH).y * cH)
                }

                if layout.isControlVisible("lstick") {
                    StickView(isLeft: true, sizeScale: analogStickScale)
                        .scaleEffect(pos("lstick", landscape: false).scale)
                        .position(x: pos("lstick", landscape: false).x * cW, y: pos("lstick", landscape: false).y * cH)
                }
                if layout.isControlVisible("rstick") {
                    StickView(isLeft: false, sizeScale: analogStickScale)
                        .scaleEffect(pos("rstick", landscape: false).scale)
                        .position(x: pos("rstick", landscape: false).x * cW, y: pos("rstick", landscape: false).y * cH)
                }
            }
        }
    }
}

// MARK: - D-Pad
struct DPadView: View {
    let size: CGFloat
    @Environment(\.padOpacity) private var padOpacity

    var body: some View {
        let a = size * 0.42
        let sp = size * 0.29
        ZStack {
            PadBtn(label: "▲", w: a, h: a, btn: .up).offset(y: -sp)
            PadBtn(label: "▼", w: a, h: a, btn: .down).offset(y: sp)
            PadBtn(label: "◀", w: a, h: a, btn: .left).offset(x: -sp)
            PadBtn(label: "▶", w: a, h: a, btn: .right).offset(x: sp)
        }
        .environment(\.padOpacity, padOpacity)
    }
}

// MARK: - Action Buttons
struct ActionButtonsView: View {
    let size: CGFloat
    @Environment(\.padOpacity) private var padOpacity

    var body: some View {
        let sp = size * 1.1
        ZStack {
            PSBtn(sym: "△", clr: .green, sz: size, btn: .triangle).offset(y: -sp)
            PSBtn(sym: "✕", clr: .blue, sz: size, btn: .cross).offset(y: sp)
            PSBtn(sym: "□", clr: .pink, sz: size, btn: .square).offset(x: -sp)
            PSBtn(sym: "○", clr: .red, sz: size, btn: .circle).offset(x: sp)
        }
        .environment(\.padOpacity, padOpacity)
    }
}

private struct ControllerPressEffect<S: InsettableShape>: View {
    let shape: S
    let color: Color
    let isPressed: Bool
    let opacity: Double

    var body: some View {
        if isPressed {
            shape
                .inset(by: 1)
                .fill(color.opacity(0.34 * opacity))
                .overlay {
                    shape
                        .inset(by: 1)
                        .stroke(color.opacity(0.72 * opacity), lineWidth: 2.2)
                }
                .shadow(color: color.opacity(0.42 * opacity), radius: 9)
                .scaleEffect(0.92)
                .animation(.easeOut(duration: 0.06), value: isPressed)
        }
    }
}

private func ARMSX2UsesUIKitPadPressSurface() -> Bool {
    ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27
}

@MainActor
private struct UIKitPadPressSurface<Content: View>: UIViewRepresentable {
    let content: Content
    let onPress: (Bool) -> Void

    init(onPress: @escaping (Bool) -> Void, @ViewBuilder content: () -> Content) {
        self.content = content()
        self.onPress = onPress
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(content: content, onPress: onPress)
    }

    func makeUIView(context: Context) -> UIButton {
        let button = UIButton(type: .custom)
        button.backgroundColor = .clear
        button.isExclusiveTouch = false

        button.addTarget(context.coordinator, action: #selector(Coordinator.touchDown), for: [.touchDown, .touchDragEnter])
        button.addTarget(context.coordinator, action: #selector(Coordinator.touchUp), for: [.touchUpInside, .touchUpOutside, .touchCancel, .touchDragExit])

        let hostedView = context.coordinator.hostingController.view!
        hostedView.backgroundColor = .clear
        hostedView.isUserInteractionEnabled = false
        hostedView.translatesAutoresizingMaskIntoConstraints = false
        button.addSubview(hostedView)
        NSLayoutConstraint.activate([
            hostedView.topAnchor.constraint(equalTo: button.topAnchor),
            hostedView.bottomAnchor.constraint(equalTo: button.bottomAnchor),
            hostedView.leadingAnchor.constraint(equalTo: button.leadingAnchor),
            hostedView.trailingAnchor.constraint(equalTo: button.trailingAnchor)
        ])

        return button
    }

    func updateUIView(_ uiView: UIButton, context: Context) {
        context.coordinator.onPress = onPress
        context.coordinator.hostingController.rootView = content
    }

    @MainActor
    final class Coordinator: NSObject {
        let hostingController: UIHostingController<Content>
        var onPress: (Bool) -> Void
        private var isPressed = false

        init(content: Content, onPress: @escaping (Bool) -> Void) {
            self.hostingController = UIHostingController(rootView: content)
            self.onPress = onPress
        }

        @objc func touchDown() {
            guard !isPressed else {
                return
            }

            isPressed = true
            onPress(true)
        }

        @objc func touchUp() {
            guard isPressed else {
                return
            }

            isPressed = false
            onPress(false)
        }
    }
}

struct PSBtn: View {
    let sym: String; let clr: Color; let sz: CGFloat; let btn: ARMSX2PadButton
    @State private var on = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var touchTarget: CGFloat { max(sz, 55) }

    var body: some View {
        if ARMSX2UsesUIKitPadPressSurface() {
            ZStack {
                UIKitPadPressSurface(onPress: updatePressed) {
                    buttonFace
                        .frame(width: sz, height: sz)
                }
                .frame(width: touchTarget, height: touchTarget)
            }
            .frame(width: touchTarget, height: touchTarget)
            .opacity(padUsesFullSkin ? 1.0 : padOpacity)
            .animation(.easeOut(duration: 0.06), value: on)
        } else {
            ZStack {
                buttonFace
                    .frame(width: sz, height: sz)
            }
            .frame(width: touchTarget, height: touchTarget)
            .contentShape(Rectangle())
            .opacity(padUsesFullSkin ? 1.0 : padOpacity)
            .animation(.easeOut(duration: 0.06), value: on)
            .simultaneousGesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in updatePressed(true) }
                .onEnded { _ in updatePressed(false) })
        }
    }

    private var buttonFace: some View {
        ZStack {
            Circle()
                .fill(.clear)

            if !padUsesFullSkin || on {
                ControllerPressEffect(shape: Circle(), color: clr, isPressed: on, opacity: padUsesFullSkin ? padOpacity * 0.75 : padOpacity)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: ControllerAsset.fileName(for: btn),
                    fallback: sym,
                    fallbackColor: on ? .white : clr,
                    fallbackFontSize: sz * 0.42,
                    skin: padSkin
                )
                    .padding(padSkin == .crispVector ? 0 : max(1, sz * 0.03))
                    .brightness(on ? 0.18 : 0)
                    .saturation(on ? 1.16 : 1.0)
                    .scaleEffect(on ? 0.90 : 1.0)
            }
        }
    }

    private func updatePressed(_ pressed: Bool) {
        guard on != pressed else {
            return
        }

        on = pressed
        EmulatorBridge.shared.setPadButton(btn, pressed: pressed)
        if pressed && SettingsStore.shared.hapticFeedback {
            HapticManager.medium.impactOccurred()
        }
    }
}

struct PadBtn: View {
    let label: String; let w: CGFloat; let h: CGFloat; let btn: ARMSX2PadButton
    @State private var on = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var touchW: CGFloat { max(w, 55) }
    private var touchH: CGFloat { max(h, 55) }

    var body: some View {
        if ARMSX2UsesUIKitPadPressSurface() {
            ZStack {
                UIKitPadPressSurface(onPress: updatePressed) {
                    buttonFace
                        .frame(width: w, height: h)
                }
                .frame(width: touchW, height: touchH)
            }
            .frame(width: touchW, height: touchH)
            .opacity(padUsesFullSkin ? 1.0 : padOpacity)
            .animation(.easeOut(duration: 0.06), value: on)
        } else {
            ZStack {
                buttonFace
                    .frame(width: w, height: h)
            }
            .frame(width: touchW, height: touchH)
            .contentShape(Rectangle())
            .opacity(padUsesFullSkin ? 1.0 : padOpacity)
            .animation(.easeOut(duration: 0.06), value: on)
            .simultaneousGesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in updatePressed(true) }
                .onEnded { _ in updatePressed(false) })
        }
    }

    private var buttonFace: some View {
        let shape = RoundedRectangle(cornerRadius: min(w, h) * 0.28, style: .continuous)
        return ZStack {
            shape
                .fill(.clear)

            if !padUsesFullSkin || on {
                ControllerPressEffect(shape: shape, color: .white, isPressed: on, opacity: padUsesFullSkin ? padOpacity * 0.75 : padOpacity)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: ControllerAsset.fileName(for: btn),
                    fallback: label,
                    fallbackColor: on ? .black : .white,
                    fallbackFontSize: min(w, h) * 0.38,
                    skin: padSkin
                )
                    .padding(padSkin == .crispVector ? 0 : max(1, min(w, h) * 0.03))
                    .brightness(on ? 0.18 : 0)
                    .scaleEffect(on ? 0.91 : 1.0)
            }
        }
    }

    private func updatePressed(_ pressed: Bool) {
        guard on != pressed else {
            return
        }

        on = pressed
        EmulatorBridge.shared.setPadButton(btn, pressed: pressed)
        if pressed && SettingsStore.shared.hapticFeedback {
            HapticManager.medium.impactOccurred()
        }
    }
}

// MARK: - Analog Stick with L3/R3 tap
struct StickView: View {
    let isLeft: Bool
    let sizeScale: CGFloat

    private var clampedScale: CGFloat {
        min(max(sizeScale, 0.8), 1.6)
    }
    private var sz: CGFloat {
        68 * clampedScale
    }
    private var knob: CGFloat {
        30 * clampedScale
    }

    @State private var off: CGSize = .zero
    @State private var isDragging = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    var body: some View {
        ZStack {
            Circle()
                .fill(.clear)
                .frame(width: sz, height: sz)

            if isDragging {
                Circle()
                    .fill(.black.opacity((isDragging ? 0.26 : 0.18) * padOpacity))
                    .stroke(.white.opacity((isDragging ? 0.34 : 0.18) * padOpacity), lineWidth: isDragging ? 1.8 : 1)
                    .shadow(color: .white.opacity(isDragging ? 0.22 * padOpacity : 0.05 * padOpacity), radius: isDragging ? 8 : 2)
                    .frame(width: sz, height: sz)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: "ic_controller_analog_base.png",
                    fallback: "",
                    fallbackColor: .white,
                    fallbackFontSize: 1,
                    skin: padSkin
                )
                    .frame(width: sz, height: sz)
                    .opacity(padOpacity)
                ControllerAssetImage(
                    fileName: "ic_controller_analog_stick.png",
                    fallback: "",
                    fallbackColor: .white,
                    fallbackFontSize: 1,
                    skin: padSkin
                )
                    .frame(width: knob, height: knob)
                    .opacity(padOpacity)
                    .brightness(isDragging ? 0.18 : 0)
                    .scaleEffect(isDragging ? 1.08 : 1.0)
                    .offset(off)
                ControllerAssetImage(
                    fileName: isLeft ? "ic_controller_l3_button.png" : "ic_controller_r3_button.png",
                    fallback: isLeft ? "L3" : "R3",
                    fallbackColor: .white.opacity(0.35),
                    fallbackFontSize: 9,
                    skin: padSkin
                )
                    .frame(width: 18, height: 18)
                    .opacity(0.45 * padOpacity)
                    .offset(y: sz / 2 + 9)
            } else if isDragging {
                Circle()
                    .fill(.white.opacity(0.22 * padOpacity))
                    .stroke(.white.opacity(0.34 * padOpacity), lineWidth: 1.4)
                    .frame(width: knob, height: knob)
                    .offset(off)
            }
        }
        .contentShape(Circle())
        .simultaneousGesture(DragGesture(minimumDistance: 0)
            .onChanged { v in
                let maxR = (sz - knob) / 2
                let dist = hypot(v.translation.width, v.translation.height)
                if dist > 4 {
                    isDragging = true
                    let d = min(dist, maxR)
                    let a = atan2(v.translation.height, v.translation.width)
                    off = CGSize(width: cos(a) * d, height: sin(a) * d)
                    let nx = Float(cos(a) * d / maxR); let ny = Float(sin(a) * d / maxR)
                    isLeft ? EmulatorBridge.shared.setLeftStick(x: nx, y: ny)
                           : EmulatorBridge.shared.setRightStick(x: nx, y: ny)
                }
            }
            .onEnded { _ in
                if !isDragging {
                    // Tap (no significant drag) → L3/R3 press
                    let btn: ARMSX2PadButton = isLeft ? .L3 : .R3
                    EmulatorBridge.shared.setPadButton(btn, pressed: true)
                    if SettingsStore.shared.hapticFeedback {
                        HapticManager.light.impactOccurred()
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                        EmulatorBridge.shared.setPadButton(btn, pressed: false)
                    }
                } else {
                    // Drag ended → reset stick
                    withAnimation(.spring(duration: 0.12)) { off = .zero }
                    isLeft ? EmulatorBridge.shared.setLeftStick(x: 0, y: 0)
                           : EmulatorBridge.shared.setRightStick(x: 0, y: 0)
                }
                isDragging = false
            })
    }
}
