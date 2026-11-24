// PadLayoutSnapshot.swift — pad layout value types: snapshot, portrait fit and editor geometry
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadLayoutSnapshot: Codable, Equatable {
    var portrait: [String: PadGroupPosition]
    var landscape: [String: PadGroupPosition]
    var perButtonPortrait: [String: PadGroupPosition]
    var perButtonLandscape: [String: PadGroupPosition]
    var controlVisibility: [String: Bool]
    // The Duo's open inner screen; nil plays the landscape layout there.
    var book: [String: PadGroupPosition]? = nil
    var perButtonBook: [String: PadGroupPosition]? = nil

    func inBook() -> PadLayoutSnapshot {
        var s = self
        s.landscape = book ?? landscape
        s.perButtonLandscape = perButtonBook ?? perButtonLandscape
        return s
    }

    func position(for id: String, landscape isLandscape: Bool) -> PadGroupPosition {
        let dict = isLandscape ? landscape : portrait
        let defaults = isLandscape ? PadLayoutStore.defaultLandscape : PadLayoutStore.defaultPortrait
        return dict[id] ?? defaults[id] ?? PadGroupPosition(x: 0.5, y: 0.5, scale: 1.0)
    }

    func perButtonPosition(for id: String, landscape isLandscape: Bool, areaW: CGFloat, areaH: CGFloat) -> PadGroupPosition {
        let dict = isLandscape ? perButtonLandscape : perButtonPortrait
        if let pos = dict[id] {
            return pos
        }

        let groupID = PadLayoutStore.groupID(for: id)
        let groupPos = position(for: groupID, landscape: isLandscape)
        let offset = VirtualPadButtonOffset.offset(for: id, isLandscape: isLandscape)
        let scaledOffsetX = offset.width * groupPos.scaleX
        let scaledOffsetY = offset.height * groupPos.scaleY
        return PadGroupPosition(
            x: groupPos.x + scaledOffsetX / areaW,
            y: groupPos.y + scaledOffsetY / areaH,
            scaleX: groupPos.scaleX,
            scaleY: groupPos.scaleY,
            hitScaleX: groupPos.hitScaleX,
            hitScaleY: groupPos.hitScaleY
        )
    }

    func isControlVisible(_ id: String) -> Bool {
        if let explicit = controlVisibility[id] {
            return explicit
        }
        let group = PadLayoutStore.groupID(for: id)
        return controlVisibility[group] ?? true
    }

    func controlGeometries(size: CGSize, landscape isLandscape: Bool, analogStickScale: CGFloat) -> [PadEditorControlGeometry] {
        func geometry(_ id: String, _ p: PadGroupPosition) -> PadEditorControlGeometry {
            let isStick = id == "lstick" || id == "rstick"
            return PadEditorControlGeometry(
                id: id,
                center: CGPoint(x: p.x * size.width, y: p.y * size.height),
                baseSize: padEditorBaseSize(for: id, isLandscape: isLandscape, analogStickScale: analogStickScale),
                hitScaleX: isStick ? p.scaleX : p.hitScaleX,
                hitScaleY: isStick ? p.scaleY : p.hitScaleY
            )
        }
        let buttons = PadLayoutStore.perButtonIDs.filter(isControlVisible).map {
            geometry($0, perButtonPosition(for: $0, landscape: isLandscape, areaW: size.width, areaH: size.height))
        }
        let groups = PadLayoutStore.groupIDs.filter { $0 != "action" && $0 != "dpad" && isControlVisible($0) }.map {
            geometry($0, position(for: $0, landscape: isLandscape))
        }
        return buttons + groups
    }

    func portraitEnvelope(analogStickScale: CGFloat) -> (top: CGFloat, height: CGFloat) {
        let reference = CGSize(width: PadPortraitFit.referenceDeckWidth, height: PadPortraitFit.referenceDeckHeight)
        let geometries = controlGeometries(size: reference, landscape: false, analogStickScale: analogStickScale)
        // Touch areas count only up to the deck edges, where the fitted layout clips them.
        guard let top = geometries.map({ max($0.hitFrame.minY, 0) }).min() else { return (0, 0) }
        // StickView draws its 18 pt badge right under the stick.
        let bottom = geometries.map {
            min($0.hitFrame.maxY, PadPortraitFit.referenceDeckHeight) + ($0.id.hasSuffix("stick") ? 18 : 0)
        }.max() ?? top
        return (top, bottom - top)
    }

    func fittedToPortraitDeck(_ fit: PadPortraitFit, size: CGSize, analogStickScale: CGFloat) -> PadLayoutSnapshot {
        var fitted = self
        fitted.portrait = Dictionary(uniqueKeysWithValues: PadLayoutStore.groupIDs.map { ($0, fit.fitted(position(for: $0, landscape: false), of: $0)) })
        fitted.perButtonPortrait = Dictionary(uniqueKeysWithValues: perButtonPortrait.map { ($0.key, fit.fitted($0.value, of: $0.key)) })

        for g in fitted.controlGeometries(size: size, landscape: false, analogStickScale: analogStickScale) where !g.id.hasSuffix("stick") {
            let frame = g.hitFrame
            guard frame.minX < 0 || frame.minY < 0 || frame.maxX > size.width || frame.maxY > size.height else { continue }
            let isPerButton = PadLayoutStore.perButtonIDs.contains(g.id)
            var p = isPerButton
                ? fitted.perButtonPosition(for: g.id, landscape: false, areaW: size.width, areaH: size.height)
                : fitted.position(for: g.id, landscape: false)
            let roomX = max(0, min(g.center.x, size.width - g.center.x)) * 2
            let roomY = max(0, min(g.center.y, size.height - g.center.y)) * 2
            p.hitScaleX = min(p.hitScaleX, roomX / max(g.baseSize.width, PadLayoutMetrics.minimumTouchLength))
            p.hitScaleY = min(p.hitScaleY, roomY / max(g.baseSize.height, PadLayoutMetrics.minimumTouchLength))
            if isPerButton {
                fitted.perButtonPortrait[g.id] = p
            } else {
                fitted.portrait[g.id] = p
            }
        }
        return fitted
    }
}

extension PadLayoutSnapshot {
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        portrait = try c.decode([String: PadGroupPosition].self, forKey: .portrait)
        landscape = try c.decode([String: PadGroupPosition].self, forKey: .landscape)
        perButtonPortrait = try c.decode([String: PadGroupPosition].self, forKey: .perButtonPortrait)
        perButtonLandscape = try c.decode([String: PadGroupPosition].self, forKey: .perButtonLandscape)
        controlVisibility = try c.decode([String: Bool].self, forKey: .controlVisibility)
        book = try? c.decodeIfPresent([String: PadGroupPosition].self, forKey: .book)
        perButtonBook = try? c.decodeIfPresent([String: PadGroupPosition].self, forKey: .perButtonBook)
    }
}

/// The device a preset keeps its own layout copy for.
enum PadDeviceClass: String, Codable, CaseIterable {
    case phone, duo, tablet

    @MainActor static var current: PadDeviceClass {
#if targetEnvironment(macCatalyst)
        return .tablet
#else
        if AppState.shared.hingePresent { return .duo }
        return UIDevice.current.userInterfaceIdiom == .pad ? .tablet : .phone
#endif
    }
}

/// Places the reference portrait deck's layout in a shorter deck: x stays, sizes and rows scale by `scale`.
struct PadPortraitFit {
    // The iPhone 18 Pro portrait deck the built-in layout was drawn on.
    static let referenceDeckWidth: CGFloat = 402
    static let referenceDeckHeight: CGFloat = 510.5
    // The smallest pad scale before the game gives up height on a short deck.
    static let outerMinimumScale: CGFloat = 0.70
    static let topGap: CGFloat = 4
    static let minimumHitScale: CGFloat = 44 / PadLayoutMetrics.minimumTouchLength

    let scale: CGFloat
    let envelopeTop: CGFloat
    let deckHeight: CGFloat

    init?(envelope: (top: CGFloat, height: CGFloat), deck: CGSize, barBase: CGFloat) {
        guard envelope.height > 0, deck.width > 0, deck.height > 0 else { return nil }
        scale = max(min(1, (deck.height - Self.topGap - barBase) / envelope.height), PadLayoutMetrics.minControlScale)
        envelopeTop = envelope.top
        deckHeight = deck.height
    }

    func fitted(_ p: PadGroupPosition, of id: String) -> PadGroupPosition {
        // Opposite D-pad arrows sit closer than about 44 pt, so their touch areas shrink with the pad.
        let minimumHit = PadLayoutStore.groupID(for: id) == "dpad" ? 0 : Self.minimumHitScale
        var q = p
        q.y = (Self.topGap + (p.y * Self.referenceDeckHeight - envelopeTop) * scale) / deckHeight
        q.scaleX = p.scaleX * scale
        q.scaleY = p.scaleY * scale
        q.hitScaleX = max(p.hitScaleX * scale, min(p.hitScaleX, minimumHit))
        q.hitScaleY = max(p.hitScaleY * scale, min(p.hitScaleY, minimumHit))
        return q
    }
}

extension PadPortraitFit {
    /// The deck a per-button position is derived in, so a stored edit fits back to its drop point.
    func referenceArea(deckWidth: CGFloat) -> CGSize {
        CGSize(width: deckWidth / scale, height: Self.referenceDeckHeight)
    }

    /// The stored y of a control dropped at `y` in the fitted deck.
    func storedY(_ y: CGFloat) -> CGFloat {
        min(max(((y * deckHeight - Self.topGap) / scale + envelopeTop) / Self.referenceDeckHeight, 0), 1)
    }
}

struct PadEditorControlGeometry {
    let id: String
    let center: CGPoint
    let baseSize: CGSize
    let hitScaleX: CGFloat
    let hitScaleY: CGFloat

    var hitSize: CGSize {
        if isStick(id) {
            return CGSize(
                width: PadLayoutMetrics.visibleLength(baseLength: baseSize.width, visibleScale: hitScaleX),
                height: PadLayoutMetrics.visibleLength(baseLength: baseSize.height, visibleScale: hitScaleY)
            )
        }
        return CGSize(
            width: PadLayoutMetrics.touchLength(baseLength: baseSize.width, hitScale: hitScaleX),
            height: PadLayoutMetrics.touchLength(baseLength: baseSize.height, hitScale: hitScaleY)
        )
    }

    var hitFrame: CGRect {
        CGRect(x: center.x - hitSize.width / 2, y: center.y - hitSize.height / 2, width: hitSize.width, height: hitSize.height)
    }
}

func isStick(_ id: String) -> Bool {
    id == "lstick" || id == "rstick"
}

func padEditorBaseSize(for id: String, isLandscape: Bool, analogStickScale: CGFloat) -> CGSize {
    switch id {
    case "up", "down", "left", "right":
        let width = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape)
        return CGSize(width: width, height: width)
    case "triangle", "circle", "square", "cross":
        let size = VirtualPadButtonOffset.actionButtonSize
        return CGSize(width: size, height: size)
    case "l1", "r1":
        return isLandscape ? CGSize(width: 120, height: 32) : CGSize(width: 100, height: 30)
    case "l2", "r2":
        return isLandscape ? CGSize(width: 130, height: 44) : CGSize(width: 110, height: 40)
    case "lstick", "rstick":
        let size = 68 * min(max(analogStickScale, 0.8), 1.6)
        return CGSize(width: size, height: size)
    case "select":
        return CGSize(width: isLandscape ? 40 : 42, height: 22)
    case "start":
        return CGSize(width: 48, height: 22)
    default:
        return CGSize(width: 60, height: 40)
    }
}
