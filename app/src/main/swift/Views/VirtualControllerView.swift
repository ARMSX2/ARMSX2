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

private struct PadSkinDescriptorKey: EnvironmentKey {
    static let defaultValue: VPadSkinDescriptor = VPadSkinLibraryStore.defaultDescriptor
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

    var padSkinDescriptor: VPadSkinDescriptor {
        get { self[PadSkinDescriptorKey.self] }
        set { self[PadSkinDescriptorKey.self] = newValue }
    }

    var padUsesFullSkin: Bool {
        get { self[PadUsesFullSkinKey.self] }
        set { self[PadUsesFullSkinKey.self] = newValue }
    }
}

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

enum ControllerAsset {
    private static let edgeToEdgePortraitAspectRatio: CGFloat = 1.55
    private static let analogBaseCommonFileName = "ic_controller_analog_base.png"
    private static let analogBaseLeftFileName = "ic_controller_analog_base_left.png"
    private static let analogBaseRightFileName = "ic_controller_analog_base_right.png"
    private static let analogStickCurrentFileName = "ic_controller_analog_stick.png"
    private static let legacyAnalogStickFileName = "ic_controller_analog_button.png"
    private static let analogStickLeftFileName = "ic_controller_analog_stick_left.png"
    private static let analogStickRightFileName = "ic_controller_analog_stick_right.png"
    private static let legacyAnalogStickLeftFileName = "ic_controller_analog_button_left.png"
    private static let legacyAnalogStickRightFileName = "ic_controller_analog_button_right.png"

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

    static func analogBaseFileName(isLeft: Bool, exists: (String) -> Bool) -> String {
        let sideFileName = isLeft ? analogBaseLeftFileName : analogBaseRightFileName
        return exists(sideFileName) ? sideFileName : analogBaseCommonFileName
    }

    static func analogBaseFileName(
        isLeft: Bool,
        descriptor: VPadSkinDescriptor,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> String {
        analogBaseFileName(isLeft: isLeft) { image(named: $0, descriptor: descriptor, skinLibrary: skinLibrary) != nil }
    }

    static func analogStickFileName(isLeft: Bool, exists: (String) -> Bool) -> String {
        let sideFileName = isLeft ? analogStickLeftFileName : analogStickRightFileName
        let legacySideFileName = isLeft ? legacyAnalogStickLeftFileName : legacyAnalogStickRightFileName
        if exists(sideFileName) {
            return sideFileName
        }
        if exists(legacySideFileName) {
            return legacySideFileName
        }
        if exists(analogStickCurrentFileName) {
            return analogStickCurrentFileName
        }
        if exists(legacyAnalogStickFileName) {
            return legacyAnalogStickFileName
        }
        return analogStickCurrentFileName
    }

    static func analogStickFileName(
        isLeft: Bool,
        descriptor: VPadSkinDescriptor,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> String {
        analogStickFileName(isLeft: isLeft) { skinContainsExactAsset(named: $0, descriptor: descriptor, skinLibrary: skinLibrary) }
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

    static func image(
        named fileName: String,
        descriptor: VPadSkinDescriptor,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> UIImage? {
        guard !fileName.isEmpty else { return nil }

        let skin = descriptor.virtualPadSkin
        let baseName = (fileName as NSString).deletingPathExtension
        if descriptor.source == .imported,
           let directory = skinLibrary.importedAssetsDirectory(for: descriptor),
           let customImage = customImage(named: fileName, baseName: baseName, directory: directory) {
            return customImage
        }

        if isLegacyCustomDescriptor(descriptor),
           let directory = VirtualPadSkin.legacyCustomSkinDirectory(),
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

    private static func skinContainsExactAsset(
        named fileName: String,
        descriptor: VPadSkinDescriptor,
        skinLibrary: VPadSkinLibraryStore
    ) -> Bool {
        let baseName = (fileName as NSString).deletingPathExtension
        let skin = descriptor.virtualPadSkin
        if descriptor.source == .imported,
           let directory = skinLibrary.importedAssetsDirectory(for: descriptor) {
            return FileManager.default.fileExists(atPath: directory.appendingPathComponent(fileName).path)
        }

        if isLegacyCustomDescriptor(descriptor),
           let directory = VirtualPadSkin.legacyCustomSkinDirectory() {
            return FileManager.default.fileExists(atPath: directory.appendingPathComponent(fileName).path)
        }

        if let directoryName = skin.bundledDirectoryName {
            return Bundle.main.url(forResource: baseName, withExtension: "png", subdirectory: "controller_skins/\(directoryName)") != nil
        }

        return UIImage(named: baseName) != nil || UIImage(named: fileName) != nil || Bundle.main.path(forResource: baseName, ofType: "png") != nil
    }

    private static func bundledSkinImage(named baseName: String, directoryName: String) -> UIImage? {
        let subdirectory = "controller_skins/\(directoryName)"
        guard let url = Bundle.main.url(forResource: baseName, withExtension: "png", subdirectory: subdirectory) else {
            return nil
        }

        return UIImage(contentsOfFile: url.path)
    }

    static func fullSkinImage(
        descriptor: VPadSkinDescriptor,
        isLandscape: Bool,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> UIImage? {
        let skin = descriptor.virtualPadSkin
        let directory: URL?
        if descriptor.source == .imported {
            directory = skinLibrary.importedAssetsDirectory(for: descriptor)
        } else if isLegacyCustomDescriptor(descriptor) {
            directory = VirtualPadSkin.legacyCustomSkinDirectory()
        } else {
            directory = nil
        }
        guard let directory else {
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

    static func gameplayFullSkinImage(
        descriptor: VPadSkinDescriptor,
        isLandscape: Bool,
        skinLibrary: VPadSkinLibraryStore = .shared
    ) -> UIImage? {
        let skin = descriptor.virtualPadSkin
        let directory: URL?
        if descriptor.source == .imported {
            directory = skinLibrary.importedAssetsDirectory(for: descriptor)
        } else if isLegacyCustomDescriptor(descriptor) {
            directory = VirtualPadSkin.legacyCustomSkinDirectory()
        } else {
            directory = nil
        }
        guard let directory else {
            return nil
        }

        guard !isLandscape else {
            return nil
        }

        let portraitCandidates = [
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

        for baseName in portraitCandidates {
            if let image = customImage(named: "\(baseName).png", baseName: baseName, directory: directory),
               !looksLikeEdgeToEdgePortrait(image) {
                return image
            }
        }

        return nil
    }

    private static func isLegacyCustomDescriptor(_ descriptor: VPadSkinDescriptor) -> Bool {
        descriptor.source != .imported && descriptor.id == VirtualPadSkin.custom.descriptorID
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
    var descriptor: VPadSkinDescriptor? = nil

    var body: some View {
        if skin == .crispVector {
            ControllerVectorGlyph(
                fileName: fileName,
                fallback: fallback,
                fallbackColor: fallbackColor,
                fallbackFontSize: fallbackFontSize
            )
        } else if let descriptor,
                  let image = ControllerAsset.image(named: fileName, descriptor: descriptor) {
            Image(uiImage: image)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
        } else if descriptor == nil,
                  let image = ControllerAsset.image(named: fileName, skin: skin) {
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




// MARK: - Visual-only alpha-mask press feedback
private struct ARMSX2SkinMaskPressEffect: View {
    let button: ARMSX2PadButton
    let skin: VirtualPadSkin
    let descriptor: VPadSkinDescriptor
    let color: Color
    let isPressed: Bool
    let opacity: Double

    private var maskImage: UIImage? {
        let fileName = ControllerAsset.fileName(for: button)
        if let image = ControllerAsset.image(named: fileName, descriptor: descriptor) {
            return image
        }
        if skin != .legacyRefresh,
           let fallback = ControllerAsset.image(named: fileName, skin: .legacyRefresh) {
            return fallback
        }
        return nil
    }

    var body: some View {
        if isPressed, let image = maskImage {
            Image(uiImage: image)
                .resizable()
                .renderingMode(.template)
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
                .foregroundStyle(color.opacity(0.34 * opacity))
                .overlay {
                    Image(uiImage: image)
                        .resizable()
                        .renderingMode(.template)
                        .interpolation(.high)
                        .antialiased(true)
                        .scaledToFit()
                        .foregroundStyle(color.opacity(0.42 * opacity))
                        .blendMode(.plusLighter)
                }
                .shadow(color: color.opacity(0.42 * opacity), radius: 9)
                .scaleEffect(0.92)
                .allowsHitTesting(false)
                .animation(.easeOut(duration: 0.06), value: isPressed)
        }
    }
}

@MainActor
private enum ARMSX2VirtualPadMaskImageCache {
    private static var cachedImages: [String: UIImage] = [:]
    private static let buttons: [ARMSX2PadButton] = [
        .up, .down, .left, .right,
        .cross, .circle, .square, .triangle,
        .L1, .R1, .L2, .R2,
        .start, .select, .L3, .R3
    ]

    private static func key(button: ARMSX2PadButton, descriptor: VPadSkinDescriptor) -> String {
        let skinKey = descriptor.id
        return "\(skinKey):\(ControllerAsset.fileName(for: button))"
    }

    static func image(for button: ARMSX2PadButton, descriptor: VPadSkinDescriptor) -> UIImage? {
        let cacheKey = key(button: button, descriptor: descriptor)
        if let cached = cachedImages[cacheKey] {
            return cached
        }

        let fileName = ControllerAsset.fileName(for: button)
        let image = ControllerAsset.image(named: fileName, descriptor: descriptor)
            ?? ControllerAsset.image(named: fileName, skin: .legacyRefresh)
        guard let image else {
            return nil
        }

        // preparingForDisplay decodes/prepares the bitmap outside the first real press path when prewarmed.
        let prepared = image.preparingForDisplay() ?? image
        cachedImages[cacheKey] = prepared
        return prepared
    }

    static func prewarm(descriptor: VPadSkinDescriptor) {
        for button in buttons {
            _ = image(for: button, descriptor: descriptor)
        }
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

enum VirtualPadPressSurfacePolicy {
    static func usesUIKitPressSurface(osMajorVersion: Int = ProcessInfo.processInfo.operatingSystemVersion.majorVersion) -> Bool {
        osMajorVersion >= 27
    }
}

private func ARMSX2UsesUIKitPadPressSurface() -> Bool {
    VirtualPadPressSurfacePolicy.usesUIKitPressSurface()
}

@MainActor
private struct UIKitPadPressSurface<Content: View>: UIViewRepresentable {
    let content: Content
    let onPress: (Bool) -> Void

    init(
        onPress: @escaping (Bool) -> Void,
        @ViewBuilder content: () -> Content
    ) {
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

    // Release the button's target/action pairs and detach the hosted view when SwiftUI
    // removes this representable (e.g. a control hidden through a visibility edit). Without
    // this, the retained UIHostingController and stale UIControl targets survive the view
    // removal and can corrupt touch dispatch after the pad is rebuilt.
    static func dismantleView(_ uiView: UIButton, coordinator: Coordinator) {
        uiView.removeTarget(coordinator, action: nil, for: .allEvents)
        coordinator.hostingController.willMove(toParent: nil)
        coordinator.hostingController.view?.removeFromSuperview()
        coordinator.hostingController.removeFromParent()
        coordinator.releasePress()
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
            releasePress()
        }

        func releasePress() {
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
    var visibleScaleX: CGFloat = 1.0
    var visibleScaleY: CGFloat = 1.0
    var hitScaleX: CGFloat = 1.0
    var hitScaleY: CGFloat = 1.0
    @State private var on = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padSkinDescriptor) private var padSkinDescriptor
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var visibleW: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: sz, visibleScale: visibleScaleX)
    }

    private var visibleH: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: sz, visibleScale: visibleScaleY)
    }

    private var touchW: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: sz, hitScale: hitScaleX)
    }

    private var touchH: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: sz, hitScale: hitScaleY)
    }

    var body: some View {
        Group {
            if ARMSX2UsesUIKitPadPressSurface() {
                ZStack {
                    UIKitPadPressSurface(onPress: updatePressed) {
                        centeredButtonFace
                    }
                    .frame(width: touchW, height: touchH)
                }
                .frame(width: touchW, height: touchH)
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .animation(.easeOut(duration: 0.06), value: on)
            } else {
                centeredButtonFace
                .frame(width: touchW, height: touchH)
                .contentShape(Rectangle())
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .animation(.easeOut(duration: 0.06), value: on)
                .simultaneousGesture(DragGesture(minimumDistance: 0)
                    .onChanged { _ in updatePressed(true) }
                    .onEnded { _ in updatePressed(false) })
            }
        }
        .onDisappear {
            updatePressed(false)
        }
    }

    private var centeredButtonFace: some View {
        ZStack(alignment: .topLeading) {
            Color.clear
                .frame(width: touchW, height: touchH)

            buttonFace
                .frame(width: visibleW, height: visibleH)
                .position(x: touchW / 2, y: touchH / 2)
        }
        .frame(width: touchW, height: touchH)
    }

    private var buttonFace: some View {
        ZStack {
            // Ellipse (not Circle) so the press-mask shape matches a non-uniform
            // visible frame; degenerates to a circle when the axes are equal.
            Ellipse()
                .fill(.clear)

            if !padUsesFullSkin || on {
                ARMSX2SkinMaskPressEffect(button: btn, skin: padSkin, descriptor: padSkinDescriptor, color: clr, isPressed: on, opacity: padUsesFullSkin ? padOpacity * 0.75 : padOpacity)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: ControllerAsset.fileName(for: btn),
                    fallback: sym,
                    fallbackColor: on ? .white : clr,
                    fallbackFontSize: min(visibleW, visibleH) * 0.42,
                    skin: padSkin,
                    descriptor: padSkinDescriptor
                )
                    .padding(padSkin == .crispVector ? 0 : max(1, min(visibleW, visibleH) * 0.03))
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

// Reusable render-only visual for a pad button. PadBtn drives its own press
// state; the composite D-pad drives this view directly for diagonal highlighting.
private struct PadButtonFace: View {
    let button: ARMSX2PadButton
    let label: String
    let baseW: CGFloat
    let baseH: CGFloat
    var visibleScaleX: CGFloat = 1.0
    var visibleScaleY: CGFloat = 1.0
    var hitScaleX: CGFloat = 1.0
    var hitScaleY: CGFloat = 1.0
    let isPressed: Bool
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padSkinDescriptor) private var padSkinDescriptor
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var visibleW: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: baseW, visibleScale: visibleScaleX)
    }

    private var visibleH: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: baseH, visibleScale: visibleScaleY)
    }

    private var touchW: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: baseW, hitScale: hitScaleX)
    }

    private var touchH: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: baseH, hitScale: hitScaleY)
    }

    var body: some View {
        centeredButtonFace
    }

    private var centeredButtonFace: some View {
        ZStack(alignment: .topLeading) {
            Color.clear
                .frame(width: touchW, height: touchH)

            buttonFace
                .frame(width: visibleW, height: visibleH)
                .position(x: touchW / 2, y: touchH / 2)
        }
        .frame(width: touchW, height: touchH)
    }

    private var buttonFace: some View {
        let shape = RoundedRectangle(cornerRadius: min(visibleW, visibleH) * 0.28, style: .continuous)
        return ZStack {
            shape
                .fill(.clear)

            if !padUsesFullSkin || isPressed {
                ARMSX2SkinMaskPressEffect(button: button, skin: padSkin, descriptor: padSkinDescriptor, color: .white, isPressed: isPressed, opacity: padUsesFullSkin ? padOpacity * 0.75 : padOpacity)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: ControllerAsset.fileName(for: button),
                    fallback: label,
                    fallbackColor: isPressed ? .black : .white,
                    fallbackFontSize: min(visibleW, visibleH) * 0.38,
                    skin: padSkin,
                    descriptor: padSkinDescriptor
                )
                    .padding(padSkin == .crispVector ? 0 : max(1, min(visibleW, visibleH) * 0.03))
                    .brightness(isPressed ? 0.18 : 0)
                    .scaleEffect(isPressed ? 0.91 : 1.0)
            }
        }
    }
}

struct PadBtn: View {
    let label: String; let w: CGFloat; let h: CGFloat; let btn: ARMSX2PadButton
    var visibleScaleX: CGFloat = 1.0
    var visibleScaleY: CGFloat = 1.0
    var hitScaleX: CGFloat = 1.0
    var hitScaleY: CGFloat = 1.0
    @State private var on = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var touchW: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: w, hitScale: hitScaleX)
    }

    private var touchH: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: h, hitScale: hitScaleY)
    }

    private var face: some View {
        PadButtonFace(button: btn, label: label, baseW: w, baseH: h, visibleScaleX: visibleScaleX, visibleScaleY: visibleScaleY, hitScaleX: hitScaleX, hitScaleY: hitScaleY, isPressed: on)
    }

    var body: some View {
        Group {
            if ARMSX2UsesUIKitPadPressSurface() {
                ZStack {
                    UIKitPadPressSurface(onPress: updatePressed) {
                        face
                    }
                    .frame(width: touchW, height: touchH)
                }
                .frame(width: touchW, height: touchH)
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .animation(.easeOut(duration: 0.06), value: on)
            } else {
                face
                .frame(width: touchW, height: touchH)
                .contentShape(Rectangle())
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .animation(.easeOut(duration: 0.06), value: on)
                .simultaneousGesture(DragGesture(minimumDistance: 0)
                    .onChanged { _ in updatePressed(true) }
                    .onEnded { _ in updatePressed(false) })
            }
        }
        .onDisappear {
            updatePressed(false)
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

// MARK: - Composite D-pad (8-way diagonals)
private struct CompositeDPadFaceInfo {
    let button: ARMSX2PadButton
    let label: String
    let center: CGPoint
    let baseSize: CGFloat
    let visibleScaleX: CGFloat
    let visibleScaleY: CGFloat
    let hitScaleX: CGFloat
    let hitScaleY: CGFloat
}

// Resolves a composite D-pad touch from the four arrows' actual geometry instead of
// a fixed angle→button mapping about the centroid. The previous angle resolver assumed
// the arrows sat exactly on the cardinal rays, which is only true for the symmetric
// default layout; moving/resizing a single arrow in a custom layout broke direction
// mapping even though the visual arrow was drawn at its real position.
//
// This mirrors `compositeFaceResolve`: each arrow hit-tests against its own resolved
// center and hitScale-aware hitbox. A touch inside one arrow resolves to that
// direction; a touch in the overlap of two neighbouring arrows resolves to both (an
// intentional diagonal); the central deadzone stays neutral. Unlike the face cluster,
// a touch that lands in the gap between arrows (but outside the deadzone) resolves to
// the nearest arrow by center distance, so quarter-circle slides still roll smoothly.
private func compositeDPadResolve(
    offset: CGPoint,
    centroid: CGPoint,
    deadzone: CGFloat,
    faces: [CompositeDPadFaceInfo]
) -> Set<ARMSX2PadButton> {
    let distance = hypot(offset.x, offset.y)
    if distance < deadzone {
        return []
    }

    let point = CGPoint(x: centroid.x + offset.x, y: centroid.y + offset.y)

    var hits: [(face: CompositeDPadFaceInfo, distance: CGFloat)] = []
    for face in faces {
        let halfW = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleX) / 2
        let halfH = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleY) / 2
        let dx = abs(point.x - face.center.x)
        let dy = abs(point.y - face.center.y)
        if dx <= halfW && dy <= halfH {
            hits.append((face, hypot(dx, dy)))
        }
    }

    if hits.isEmpty {
        // Gap between arrows: resolve to the nearest arrow by center distance so a D-pad
        // press always registers a direction once past the deadzone, keeping slides fluid.
        guard let nearest = faces.min(by: {
            hypot($0.center.x - point.x, $0.center.y - point.y) <
            hypot($1.center.x - point.x, $1.center.y - point.y)
        }) else { return [] }
        return [nearest.button]
    }
    if hits.count == 1 {
        return [hits[0].face.button]
    }

    // Two or more hitboxes overlap here: press the two nearest neighbours (a diagonal),
    // never a broad chord.
    hits.sort { $0.distance < $1.distance }
    return Set(hits.prefix(2).map(\.face.button))
}

// Resolves a face-button touch against each button's actual resolved hit region —
// the same center and hitbox normal face buttons hit-test — instead of a fixed
// angle→button mapping. A touch in a single hitbox resolves to that button; a touch
// in the overlap of two resolves to both (an intentional combo between neighbours);
// the central gap (no hitbox) resolves to nothing. This follows custom skins,
// layouts, per-axis scale, and hit scale, so pressing a button always resolves to
// that button regardless of how the skin arranged the cluster.
private func compositeFaceResolve(
    offset: CGPoint,
    centroid: CGPoint,
    faces: [CompositeFaceButtonInfo]
) -> Set<ARMSX2PadButton> {
    let point = CGPoint(x: centroid.x + offset.x, y: centroid.y + offset.y)

    var hits: [(face: CompositeFaceButtonInfo, distance: CGFloat)] = []
    for face in faces {
        let halfW = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleX) / 2
        let halfH = PadLayoutMetrics.touchLength(baseLength: face.baseSize, hitScale: face.hitScaleY) / 2
        let dx = abs(point.x - face.center.x)
        let dy = abs(point.y - face.center.y)
        if dx <= halfW && dy <= halfH {
            hits.append((face, hypot(dx, dy)))
        }
    }

    if hits.isEmpty { return [] }
    if hits.count == 1 { return [hits[0].face.button] }

    // Two or more hitboxes overlap here: press the two nearest neighbours, never a
    // broad chord.
    hits.sort { $0.distance < $1.distance }
    return Set(hits.prefix(2).map(\.face.button))
}

@MainActor
private protocol CompositeDPadTouchHandling: AnyObject {
    func touchBegan(at offset: CGPoint)
    func touchMoved(at offset: CGPoint)
    func touchEnded()
}

@MainActor
private final class CompositeDPadTouchView: UIView {
    weak var handler: CompositeDPadTouchHandling?

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        backgroundColor = .clear
        isExclusiveTouch = false
        isMultipleTouchEnabled = false
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let offset = offset(from: touches) else { return }
        handler?.touchBegan(at: offset)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let offset = offset(from: touches) else { return }
        handler?.touchMoved(at: offset)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        handler?.touchEnded()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        handler?.touchEnded()
    }

    // Confine capture to the inscribed circle so the square frame's corners
    // never steal touches meant for neighbouring controls.
    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        let radius = min(bounds.width, bounds.height) / 2
        let dx = point.x - bounds.midX
        let dy = point.y - bounds.midY
        return (dx * dx) + (dy * dy) <= (radius * radius)
    }

    private func offset(from touches: Set<UITouch>) -> CGPoint? {
        guard let touch = touches.first else {
            return nil
        }
        let location = touch.location(in: self)
        return CGPoint(x: location.x - bounds.midX, y: location.y - bounds.midY)
    }
}

@MainActor
private struct CompositeDPadTouchSurface: UIViewRepresentable {
    let onBegan: () -> Void
    let onLocation: (CGPoint) -> Void
    let onEnded: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onBegan: onBegan, onLocation: onLocation, onEnded: onEnded)
    }

    func makeUIView(context: Context) -> CompositeDPadTouchView {
        let view = CompositeDPadTouchView(frame: .zero)
        view.handler = context.coordinator
        return view
    }

    func updateUIView(_ uiView: CompositeDPadTouchView, context: Context) {
        context.coordinator.onBegan = onBegan
        context.coordinator.onLocation = onLocation
        context.coordinator.onEnded = onEnded
    }

    @MainActor
    final class Coordinator: NSObject, CompositeDPadTouchHandling {
        var onBegan: () -> Void
        var onLocation: (CGPoint) -> Void
        var onEnded: () -> Void

        init(onBegan: @escaping () -> Void, onLocation: @escaping (CGPoint) -> Void, onEnded: @escaping () -> Void) {
            self.onBegan = onBegan
            self.onLocation = onLocation
            self.onEnded = onEnded
        }

        func touchBegan(at offset: CGPoint) {
            onBegan()
            onLocation(offset)
        }

        func touchMoved(at offset: CGPoint) {
            onLocation(offset)
        }

        func touchEnded() {
            onEnded()
        }
    }
}

private struct CompositeDPadView: View {
    let faces: [CompositeDPadFaceInfo]
    let centroid: CGPoint
    let captureDiameter: CGFloat
    let deadzone: CGFloat

    @State private var pressed: Set<ARMSX2PadButton> = []
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    var body: some View {
        ZStack {
            CompositeDPadTouchSurface(
                onBegan: {
                    if SettingsStore.shared.hapticFeedback {
                        HapticManager.medium.impactOccurred()
                    }
                },
                onLocation: { offset in
                    apply(compositeDPadResolve(offset: offset, centroid: centroid, deadzone: deadzone, faces: faces))
                },
                onEnded: {
                    releaseAll()
                }
            )
            .frame(width: captureDiameter, height: captureDiameter)
            .position(x: centroid.x, y: centroid.y)

            ForEach(faces, id: \.button) { face in
                PadButtonFace(
                    button: face.button,
                    label: face.label,
                    baseW: face.baseSize,
                    baseH: face.baseSize,
                    visibleScaleX: face.visibleScaleX,
                    visibleScaleY: face.visibleScaleY,
                    hitScaleX: face.hitScaleX,
                    hitScaleY: face.hitScaleY,
                    isPressed: pressed.contains(face.button)
                )
                .allowsHitTesting(false)
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .position(x: face.center.x, y: face.center.y)
            }
        }
        .onDisappear {
            releaseAll()
        }
    }

    // Press only directions that newly entered the set and release only those
    // that left it, so quarter-circle slides never leave a stale direction held.
    private func apply(_ next: Set<ARMSX2PadButton>) {
        for button in pressed.subtracting(next) {
            EmulatorBridge.shared.setPadButton(button, pressed: false)
        }
        for button in next.subtracting(pressed) {
            EmulatorBridge.shared.setPadButton(button, pressed: true)
        }
        pressed = next
    }

    private func releaseAll() {
        let held = pressed
        guard !held.isEmpty else { return }
        for button in held {
            EmulatorBridge.shared.setPadButton(button, pressed: false)
        }
        pressed = []
    }
}

// MARK: - Composite face buttons (combo zones)
private struct CompositeFaceButtonInfo {
    let button: ARMSX2PadButton
    let symbol: String
    let color: Color
    let center: CGPoint
    let baseSize: CGFloat
    let visibleScaleX: CGFloat
    let visibleScaleY: CGFloat
    let hitScaleX: CGFloat
    let hitScaleY: CGFloat
}

// Render-only visual for an action (face) button. The composite face cluster drives
// this view directly with an isPressed flag; it owns no input state of its own, so
// the cluster is the single owner of every press/release it shows.
private struct PSButtonFace: View {
    let button: ARMSX2PadButton
    let symbol: String
    let color: Color
    let baseSize: CGFloat
    var visibleScaleX: CGFloat = 1.0
    var visibleScaleY: CGFloat = 1.0
    var hitScaleX: CGFloat = 1.0
    var hitScaleY: CGFloat = 1.0
    let isPressed: Bool
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padSkinDescriptor) private var padSkinDescriptor
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    private var visibleW: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: baseSize, visibleScale: visibleScaleX)
    }

    private var visibleH: CGFloat {
        PadLayoutMetrics.visibleLength(baseLength: baseSize, visibleScale: visibleScaleY)
    }

    private var touchW: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: baseSize, hitScale: hitScaleX)
    }

    private var touchH: CGFloat {
        PadLayoutMetrics.touchLength(baseLength: baseSize, hitScale: hitScaleY)
    }

    var body: some View {
        centeredButtonFace
    }

    private var centeredButtonFace: some View {
        ZStack(alignment: .topLeading) {
            Color.clear
                .frame(width: touchW, height: touchH)

            buttonFace
                .frame(width: visibleW, height: visibleH)
                .position(x: touchW / 2, y: touchH / 2)
        }
        .frame(width: touchW, height: touchH)
    }

    private var buttonFace: some View {
        ZStack {
            // Ellipse (not Circle) so the press-mask shape matches a non-uniform
            // visible frame; degenerates to a circle when the axes are equal.
            Ellipse()
                .fill(.clear)

            if !padUsesFullSkin || isPressed {
                ARMSX2SkinMaskPressEffect(button: button, skin: padSkin, descriptor: padSkinDescriptor, color: color, isPressed: isPressed, opacity: padUsesFullSkin ? padOpacity * 0.75 : padOpacity)
            }

            if !padUsesFullSkin {
                ControllerAssetImage(
                    fileName: ControllerAsset.fileName(for: button),
                    fallback: symbol,
                    fallbackColor: isPressed ? .white : color,
                    fallbackFontSize: min(visibleW, visibleH) * 0.42,
                    skin: padSkin,
                    descriptor: padSkinDescriptor
                )
                    .padding(padSkin == .crispVector ? 0 : max(1, min(visibleW, visibleH) * 0.03))
                    .brightness(isPressed ? 0.18 : 0)
                    .saturation(isPressed ? 1.16 : 1.0)
                    .scaleEffect(isPressed ? 0.90 : 1.0)
            }
        }
    }
}

@MainActor
private protocol CompositeFaceTouchHandling: AnyObject {
    func faceTouchesChanged(_ offsets: [CGPoint])
}

// Multi-touch raw-touch surface for the face-button cluster. Each active touch is
// tracked independently by identity; on any change the handler receives the full set
// of current touch offsets so the cluster can resolve each one and union the pressed
// buttons. This keeps independent two-finger presses working while still allowing one
// finger to slide into a two-button combo.
@MainActor
private final class CompositeFaceTouchView: UIView {
    weak var handler: CompositeFaceTouchHandling?
    private var tracked: [ObjectIdentifier: CGPoint] = [:]

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        backgroundColor = .clear
        isExclusiveTouch = false
        isMultipleTouchEnabled = true
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            tracked[ObjectIdentifier(touch)] = offset(of: touch)
        }
        handler?.faceTouchesChanged(Array(tracked.values))
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            tracked[ObjectIdentifier(touch)] = offset(of: touch)
        }
        handler?.faceTouchesChanged(Array(tracked.values))
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            tracked[ObjectIdentifier(touch)] = nil
        }
        handler?.faceTouchesChanged(Array(tracked.values))
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            tracked[ObjectIdentifier(touch)] = nil
        }
        handler?.faceTouchesChanged(Array(tracked.values))
    }

    // Confine new touches to the inscribed circle so the square frame's corners
    // never steal touches meant for neighbouring controls.
    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        let radius = min(bounds.width, bounds.height) / 2
        let dx = point.x - bounds.midX
        let dy = point.y - bounds.midY
        return (dx * dx) + (dy * dy) <= (radius * radius)
    }

    private func offset(of touch: UITouch) -> CGPoint {
        let location = touch.location(in: self)
        return CGPoint(x: location.x - bounds.midX, y: location.y - bounds.midY)
    }

    // Release every button if the surface is torn down while touches are in flight
    // (e.g. orientation change or navigation). The SwiftUI view also calls releaseAll
    // on disappear; this is a belt-and-suspenders guard at the UIKit layer.
    override func removeFromSuperview() {
        handler?.faceTouchesChanged([])
        super.removeFromSuperview()
    }
}

@MainActor
private struct CompositeFaceTouchSurface: UIViewRepresentable {
    let onOffsets: ([CGPoint]) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onOffsets: onOffsets)
    }

    func makeUIView(context: Context) -> CompositeFaceTouchView {
        let view = CompositeFaceTouchView(frame: .zero)
        view.handler = context.coordinator
        return view
    }

    func updateUIView(_ uiView: CompositeFaceTouchView, context: Context) {
        context.coordinator.onOffsets = onOffsets
    }

    @MainActor
    final class Coordinator: NSObject, CompositeFaceTouchHandling {
        var onOffsets: ([CGPoint]) -> Void

        init(onOffsets: @escaping ([CGPoint]) -> Void) {
            self.onOffsets = onOffsets
        }

        func faceTouchesChanged(_ offsets: [CGPoint]) {
            onOffsets(offsets)
        }
    }
}

// Composite face-button cluster: one multi-touch surface over the four action
// buttons. Each touch resolves to the button whose hitbox contains it, or the two
// nearest buttons where hitboxes overlap; the union of all
// active touches is pressed, so independent two-finger presses keep working while a
// single finger can still roll through a two-button combo. Buttons that leave the
// union are released immediately so no press ever goes stale.
private struct CompositeFaceView: View {
    let faces: [CompositeFaceButtonInfo]
    let centroid: CGPoint
    let captureDiameter: CGFloat

    @State private var pressed: Set<ARMSX2PadButton> = []
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padUsesFullSkin) private var padUsesFullSkin

    var body: some View {
        ZStack {
            CompositeFaceTouchSurface { offsets in
                var next: Set<ARMSX2PadButton> = []
                for offset in offsets {
                    next.formUnion(compositeFaceResolve(
                        offset: offset,
                        centroid: centroid,
                        faces: faces
                    ))
                }
                apply(next)
            }
            .frame(width: captureDiameter, height: captureDiameter)
            .position(x: centroid.x, y: centroid.y)

            ForEach(faces, id: \.button) { face in
                PSButtonFace(
                    button: face.button,
                    symbol: face.symbol,
                    color: face.color,
                    baseSize: face.baseSize,
                    visibleScaleX: face.visibleScaleX,
                    visibleScaleY: face.visibleScaleY,
                    hitScaleX: face.hitScaleX,
                    hitScaleY: face.hitScaleY,
                    isPressed: pressed.contains(face.button)
                )
                .allowsHitTesting(false)
                .opacity(padUsesFullSkin ? 1.0 : padOpacity)
                .position(x: face.center.x, y: face.center.y)
            }
        }
        .onDisappear {
            releaseAll()
        }
    }

    // Press only buttons that newly entered the union and release only those that
    // left it, so slides and multi-touch never leave a stale button held. Haptic
    // fires once per change that newly presses at least one button.
    private func apply(_ next: Set<ARMSX2PadButton>) {
        let released = pressed.subtracting(next)
        let newlyPressed = next.subtracting(pressed)
        for button in released {
            EmulatorBridge.shared.setPadButton(button, pressed: false)
        }
        for button in newlyPressed {
            EmulatorBridge.shared.setPadButton(button, pressed: true)
        }
        if !newlyPressed.isEmpty, SettingsStore.shared.hapticFeedback {
            HapticManager.medium.impactOccurred()
        }
        pressed = next
    }

    private func releaseAll() {
        let held = pressed
        guard !held.isEmpty else { return }
        for button in held {
            EmulatorBridge.shared.setPadButton(button, pressed: false)
        }
        pressed = []
    }
}

// MARK: - Analog Stick with L3/R3 tap
struct StickView: View {
    let isLeft: Bool
    let sizeScale: CGFloat
    var layoutScale: CGFloat = 1.0

    private var clampedScale: CGFloat {
        min(max(sizeScale, 0.8), 1.6)
    }
    private var effectiveScale: CGFloat {
        clampedScale * PadLayoutMetrics.clampedScale(layoutScale)
    }
    private var sz: CGFloat {
        68 * effectiveScale
    }
    private var knob: CGFloat {
        30 * effectiveScale
    }

    @State private var off: CGSize = .zero
    @State private var isDragging = false
    @Environment(\.padOpacity) private var padOpacity
    @Environment(\.padSkin) private var padSkin
    @Environment(\.padSkinDescriptor) private var padSkinDescriptor
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
                    fileName: ControllerAsset.analogBaseFileName(isLeft: isLeft, descriptor: padSkinDescriptor),
                    fallback: "",
                    fallbackColor: .white,
                    fallbackFontSize: 1,
                    skin: padSkin,
                    descriptor: padSkinDescriptor
                )
                    .frame(width: sz, height: sz)
                    .opacity(padOpacity)
                ControllerAssetImage(
                    fileName: ControllerAsset.analogStickFileName(isLeft: isLeft, descriptor: padSkinDescriptor),
                    fallback: "",
                    fallbackColor: .white,
                    fallbackFontSize: 1,
                    skin: padSkin,
                    descriptor: padSkinDescriptor
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
                    skin: padSkin,
                    descriptor: padSkinDescriptor
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
