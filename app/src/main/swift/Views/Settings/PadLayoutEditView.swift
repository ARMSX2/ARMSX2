// PadLayoutEditView.swift — Drag-to-edit virtual pad layout
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct PadLayoutSnapshot {
    let portrait: [String: PadGroupPosition]
    let landscape: [String: PadGroupPosition]
    let perButtonPortrait: [String: PadGroupPosition]
    let perButtonLandscape: [String: PadGroupPosition]
    let controlVisibility: [String: Bool]
    let skin: VirtualPadSkin
}

struct PadLayoutEditView: View {
    let onDismiss: () -> Void
    @State private var settings = SettingsStore.shared
    @State private var layout = PadLayoutStore.shared
    @State private var editLandscape = false
    @State private var undoStack: [PadLayoutSnapshot] = []
    @State private var originalSnapshot: PadLayoutSnapshot? = nil
    @State private var didInitializeOrientation = false

    var body: some View {
        GeometryReader { geo in
            let isActuallyLandscape = geo.size.width > geo.size.height
            let isMismatch = isActuallyLandscape != editLandscape
            ZStack(alignment: .top) {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture {}
                    .gesture(DragGesture(minimumDistance: 0))

                if isActuallyLandscape {
                    landscapeEditorCanvas(isEditable: !isMismatch)
                } else {
                    portraitEditorCanvas(width: geo.size.width, height: geo.size.height, isEditable: !isMismatch)
                }

                VStack(spacing: 0) {
                    editorToolbar()
                        .padding(.top, max(geo.safeAreaInsets.top, 8))
                        .padding(.horizontal, max(max(geo.safeAreaInsets.leading, geo.safeAreaInsets.trailing), 8))

                    if isMismatch {
                        orientationMismatchView()
                    } else {
                        Spacer(minLength: 0)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                .zIndex(2)
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
            .onAppear {
                if originalSnapshot == nil {
                    originalSnapshot = captureSnapshot()
                }
                if !didInitializeOrientation {
                    editLandscape = isActuallyLandscape
                    didInitializeOrientation = true
                }
            }
        }
        .navigationBarHidden(true)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .onDisappear {
            rollbackUnsavedChanges()
            NotificationCenter.default.post(name: Notification.Name("ARMSX2iOSPadLayoutEditorDismissed"), object: nil)
        }
    }

    // MARK: - Landscape editor canvas (always shown when device is landscape)
    private func landscapeEditorCanvas(isEditable: Bool) -> some View {
        GeometryReader { geo in
            let width = geo.size.width
            let height = geo.size.height
            ZStack {
                Color.clear
                    .allowsHitTesting(false)

                if isEditable {
                    customSkinPreview(isLandscape: true, width: width, height: height)
                    editorControls(areaW: width, areaH: height, isLandscape: true)
                }
            }
            .contentShape(Rectangle())
        }
        .ignoresSafeArea()
    }

    // MARK: - Portrait editor canvas (always shown when device is portrait)
    private func portraitEditorCanvas(width: CGFloat, height: CGFloat, isEditable: Bool) -> some View {
        let gameHeight = min(width * 3 / 4, height * 0.55)
        let padHeight = height - gameHeight
        return VStack(spacing: 0) {
            // Top: reveal paused gameplay underneath
            Color.clear
                .allowsHitTesting(false)
                .frame(height: gameHeight)

            // Bottom: controller deck editing area (dark enough for controls)
            ZStack {
                Color.black.opacity(0.85)
                if isEditable {
                    customSkinPreview(isLandscape: false, width: width, height: padHeight)
                    editorControls(areaW: width, areaH: padHeight, isLandscape: false)
                }
            }
            .frame(height: padHeight)
            .clipped()
        }
    }

    private func editorToolbar() -> some View {
        HStack(spacing: 6) {
            Button {
                rollbackUnsavedChanges()
                onDismiss()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(.white)
                    .frame(width: 28, height: 28)
            }

            Button {
                guard let snapshot = undoStack.popLast() else { return }
                restore(snapshot: snapshot)
            } label: {
                Image(systemName: "arrow.uturn.backward.circle")
                    .font(.title3)
                    .foregroundStyle(undoStack.isEmpty ? .white.opacity(0.3) : .white)
                    .frame(width: 28, height: 28)
            }
            .disabled(undoStack.isEmpty)

            Picker("", selection: $editLandscape) {
                Text("Port.").tag(false)
                Text("Land.").tag(true)
            }
            .pickerStyle(.segmented)
            .frame(width: 112)
            .background(.black.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))

            skinPickerMenuButton()
            optionsMenuButton()

            Button {
                layout.save()
                undoStack.removeAll()
                originalSnapshot = nil
                onDismiss()
            } label: {
                Image(systemName: "checkmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(.green)
                    .frame(width: 28, height: 28)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private func orientationMismatchView() -> some View {
        let message = editLandscape
            ? "Rotate device to landscape to edit landscape layout"
            : "Rotate device to portrait to edit portrait layout"
        let explanation = editLandscape
            ? "Accurate editing requires rotating the device or switching to portrait layout."
            : "Accurate editing requires rotating the device or switching to landscape layout."
        return VStack(spacing: 12) {
            Image(systemName: "rotate.right")
                .font(.system(size: 48, weight: .light))
                .foregroundStyle(.white.opacity(0.6))
            Text(message)
                .font(.headline.weight(.semibold))
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .lineLimit(3)
            Text(explanation)
                .font(.subheadline)
                .foregroundStyle(.white.opacity(0.7))
                .multilineTextAlignment(.center)
                .lineLimit(3)
        }
        .padding(.horizontal, 32)
        .padding(.vertical, 24)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 16))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .allowsHitTesting(false)
    }

    private func skinPickerMenuButton() -> some View {
        Menu {
            ForEach(VirtualPadSkin.allCases) { skin in
                Button {
                    guard settings.virtualPadSkin != skin else { return }
                    pushSnapshot()
                    settings.virtualPadSkin = skin
                } label: {
                    Label(settings.localized(skin.label), systemImage: settings.virtualPadSkin == skin ? "checkmark" : "circle")
                }
            }
        } label: {
            Image(systemName: "paintpalette.fill")
                .font(.title3)
                .foregroundStyle(.white)
                .frame(width: 28, height: 28)
        }
    }

    private func optionsMenuButton() -> some View {
        Menu {
            Section("Visibility") {
                ForEach(PadLayoutStore.groupIDs.filter { $0 != "action" }, id: \.self) { (id: String) in
                    visibilityMenuItem(id: id)
                }
                visibilityMenuItem(id: "cross", label: "X")
                visibilityMenuItem(id: "circle", label: "O")
                visibilityMenuItem(id: "triangle", label: "Triangle")
                visibilityMenuItem(id: "square", label: "Square")
            }

            Section("Reset") {
                Button("Reset Action", role: .destructive) {
                    pushSnapshot()
                    layout.resetPerButtonActionButtons(isLandscape: editLandscape)
                }
                Button("Reset D-Pad", role: .destructive) {
                    pushSnapshot()
                    layout.resetPerButtonDPad(isLandscape: editLandscape)
                }
                Button("Reset All", role: .destructive) {
                    pushSnapshot()
                    layout.resetAll()
                }
                Button("Reset Visibility", role: .destructive) {
                    pushSnapshot()
                    layout.resetControlVisibility()
                }
            }
        } label: {
            Image(systemName: "ellipsis.circle")
                .font(.title3)
                .foregroundStyle(.white)
                .frame(width: 28, height: 28)
        }
    }

    private func visibilityMenuItem(id: String, label: String? = nil) -> some View {
        let visible = layout.isControlVisible(id)
        return Button {
            pushSnapshot()
            layout.setControlVisible(id, visible: !visible)
        } label: {
            let name = label ?? id.uppercased()
            Label(name, systemImage: visible ? "eye" : "eye.slash")
        }
    }

    @ViewBuilder
    private func customSkinPreview(isLandscape: Bool, width: CGFloat, height: CGFloat) -> some View {
        if let fullSkin = ControllerAsset.gameplayFullSkinImage(skin: settings.virtualPadSkin, isLandscape: isLandscape) {
            Image(uiImage: fullSkin)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFill()
                .frame(width: width, height: height)
                .clipped()
                .opacity(0.42)
                .allowsHitTesting(false)
        }
    }

    @ViewBuilder
    private func editorControls(areaW: CGFloat, areaH: CGFloat, isLandscape: Bool) -> some View {
        let stickScale = min(max(CGFloat(settings.analogStickScale), 0.8), 1.6)
        ZStack {
            ForEach(PadLayoutStore.perButtonIDs, id: \.self) { (id: String) in
                DraggableButton(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape, onBeginEdit: pushSnapshot)
            }
            ForEach(PadLayoutStore.groupIDs.filter { $0 != "action" && $0 != "dpad" }, id: \.self) { (id: String) in
                DraggableGroup(id: id, areaW: areaW, areaH: areaH, isLandscape: isLandscape, analogStickScale: stickScale, onBeginEdit: pushSnapshot)
            }
        }
        .environment(\.padSkin, settings.virtualPadSkin)
        .environment(\.padOpacity, 1.0)
        .environment(\.padUsesFullSkin, false)
    }

    // MARK: - Snapshot / Undo

    private func captureSnapshot() -> PadLayoutSnapshot {
        PadLayoutSnapshot(
            portrait: layout.portrait,
            landscape: layout.landscape,
            perButtonPortrait: layout.perButtonPortrait,
            perButtonLandscape: layout.perButtonLandscape,
            controlVisibility: layout.controlVisibility,
            skin: settings.virtualPadSkin
        )
    }

    private func restore(snapshot: PadLayoutSnapshot) {
        layout.portrait = snapshot.portrait
        layout.landscape = snapshot.landscape
        layout.perButtonPortrait = snapshot.perButtonPortrait
        layout.perButtonLandscape = snapshot.perButtonLandscape
        layout.controlVisibility = snapshot.controlVisibility
        settings.virtualPadSkin = snapshot.skin
    }

    private func pushSnapshot() {
        undoStack.append(captureSnapshot())
    }

    private func rollbackUnsavedChanges() {
        guard let snapshot = originalSnapshot else { return }
        restore(snapshot: snapshot)
        undoStack.removeAll()
        originalSnapshot = nil
    }
}

// MARK: - Draggable group widget
private struct DraggableGroup: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool
    let analogStickScale: CGFloat
    let onBeginEdit: () -> Void

    @State private var layout = PadLayoutStore.shared
    @State private var dragOffset: CGSize = .zero
    @State private var currentScale: CGFloat = 1.0
    @State private var hasPushedSnapshot = false

    private var pos: PadGroupPosition {
        layout.position(for: id, landscape: isLandscape)
    }

    private var currentX: CGFloat { pos.x * areaW + dragOffset.width }
    private var currentY: CGFloat { pos.y * areaH + dragOffset.height }
    private var scaledSize: CGSize {
        let s = pos.scale * currentScale
        return CGSize(width: groupSize.width * s, height: groupSize.height * s)
    }

    var body: some View {
        let isVisible = layout.isControlVisible(id)
        ZStack {
            RoundedRectangle(cornerRadius: 8)
                .fill(.white.opacity(0.05))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(.blue.opacity(0.4), lineWidth: 1.5)
                )
                .frame(width: scaledSize.width + 16, height: scaledSize.height + 16)

            groupContent
                .scaleEffect(pos.scale * currentScale)
                .allowsHitTesting(false)

            Text(id.uppercased())
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(.blue.opacity(0.7))
                .offset(y: -(scaledSize.height / 2 + 12))
        }
        .position(x: currentX, y: currentY)
        .opacity(isVisible ? 0.8 : 0.25)
        .gesture(
            DragGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    dragOffset = v.translation
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    let newX = (pos.x * areaW + v.translation.width) / areaW
                    let newY = (pos.y * areaH + v.translation.height) / areaH
                    updatePosition(x: newX.clamped(0, 1), y: newY.clamped(0, 1))
                    dragOffset = .zero
                }
        )
        .simultaneousGesture(
            MagnifyGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    currentScale = v.magnification
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    let newScale = (pos.scale * v.magnification).clamped(0.5, 2.0)
                    updateScale(newScale)
                    currentScale = 1.0
                }
        )
    }

    private func updatePosition(x: CGFloat, y: CGFloat) {
        var p = pos
        p.x = x
        p.y = y
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private func updateScale(_ scale: CGFloat) {
        var p = pos
        p.scale = scale
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private var groupSize: CGSize {
        switch id {
        case "dpad":
            let s = isLandscape ? VirtualPadButtonOffset.dpadLandscapeSize : VirtualPadButtonOffset.dpadPortraitSize
            return CGSize(width: s, height: s)
        case "action":
            let s = VirtualPadButtonOffset.actionButtonSize * 3.2
            return CGSize(width: s, height: s)
        case "l1":
            return CGSize(width: 120, height: 32)
        case "l2":
            return CGSize(width: 130, height: 44)
        case "r1":
            return CGSize(width: 120, height: 32)
        case "r2":
            return CGSize(width: 130, height: 44)
        case "lstick", "rstick":
            let s = 68 * min(max(analogStickScale, 0.8), 1.6)
            return CGSize(width: s, height: s)
        case "select":
            return CGSize(width: 40, height: 22)
        case "start":
            return CGSize(width: 48, height: 22)
        default:
            return CGSize(width: 60, height: 40)
        }
    }

    @ViewBuilder
    private var groupContent: some View {
        switch id {
        case "dpad":
            DPadView(size: isLandscape ? VirtualPadButtonOffset.dpadLandscapeSize : VirtualPadButtonOffset.dpadPortraitSize)
        case "action":
            ActionButtonsView(size: VirtualPadButtonOffset.actionButtonSize)
        case "l1":
            PadBtn(label: "L1", w: 120, h: 32, btn: .L1)
        case "l2":
            PadBtn(label: "L2", w: 130, h: 44, btn: .L2)
        case "r1":
            PadBtn(label: "R1", w: 120, h: 32, btn: .R1)
        case "r2":
            PadBtn(label: "R2", w: 130, h: 44, btn: .R2)
        case "lstick":
            StickView(isLeft: true, sizeScale: analogStickScale)
        case "rstick":
            StickView(isLeft: false, sizeScale: analogStickScale)
        case "select":
            PadBtn(label: "SEL", w: 40, h: 22, btn: .select)
        case "start":
            PadBtn(label: "START", w: 48, h: 22, btn: .start)
        default:
            EmptyView()
        }
    }
}

// MARK: - Draggable individual button widget
private struct DraggableButton: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool
    let onBeginEdit: () -> Void

    @State private var layout = PadLayoutStore.shared
    @State private var dragOffset: CGSize = .zero
    @State private var currentScale: CGFloat = 1.0
    @State private var hasPushedSnapshot = false

    private var pos: PadGroupPosition {
        layout.perButtonPosition(for: id, landscape: isLandscape, areaW: areaW, areaH: areaH)
    }

    private var currentX: CGFloat { pos.x * areaW + dragOffset.width }
    private var currentY: CGFloat { pos.y * areaH + dragOffset.height }

    private var baseButtonSize: CGSize {
        switch id {
        case "up", "down", "left", "right":
            let w = VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape)
            return CGSize(width: w, height: w)
        case "triangle", "circle", "square", "cross":
            let sz = VirtualPadButtonOffset.actionButtonSize
            return CGSize(width: sz, height: sz)
        default:
            return CGSize(width: 40, height: 40)
        }
    }

    private var scaledSize: CGSize {
        let s = pos.scale * currentScale
        return CGSize(width: baseButtonSize.width * s, height: baseButtonSize.height * s)
    }

    var body: some View {
        let isVisible = layout.isControlVisible(id)
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(.white.opacity(0.05))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(.blue.opacity(0.4), lineWidth: 1.5)
                )
                .frame(width: scaledSize.width + 12, height: scaledSize.height + 12)

            buttonContent
                .scaleEffect(pos.scale * currentScale)
                .allowsHitTesting(false)

            Text(buttonLabel.uppercased())
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(.blue.opacity(0.7))
                .offset(y: -(scaledSize.height / 2 + 10))
        }
        .position(x: currentX, y: currentY)
        .opacity(isVisible ? 0.8 : 0.25)
        .gesture(
            DragGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    dragOffset = v.translation
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    let newX = (pos.x * areaW + v.translation.width) / areaW
                    let newY = (pos.y * areaH + v.translation.height) / areaH
                    updatePosition(x: newX.clamped(0, 1), y: newY.clamped(0, 1))
                    dragOffset = .zero
                }
        )
        .simultaneousGesture(
            MagnifyGesture()
                .onChanged { v in
                    if !hasPushedSnapshot {
                        onBeginEdit()
                        hasPushedSnapshot = true
                    }
                    currentScale = v.magnification
                }
                .onEnded { v in
                    hasPushedSnapshot = false
                    let newScale = (pos.scale * v.magnification).clamped(0.5, 2.0)
                    updateScale(newScale)
                    currentScale = 1.0
                }
        )
    }

    private var buttonLabel: String {
        switch id {
        case "up": return "UP"
        case "down": return "DN"
        case "left": return "LT"
        case "right": return "RT"
        case "triangle": return "TRI"
        case "circle": return "CIR"
        case "square": return "SQU"
        case "cross": return "CRO"
        default: return id
        }
    }

    private func updatePosition(x: CGFloat, y: CGFloat) {
        var p = pos
        p.x = x
        p.y = y
        if isLandscape {
            layout.perButtonLandscape[id] = p
        } else {
            layout.perButtonPortrait[id] = p
        }
    }

    private func updateScale(_ scale: CGFloat) {
        var p = pos
        p.scale = scale
        if isLandscape {
            layout.perButtonLandscape[id] = p
        } else {
            layout.perButtonPortrait[id] = p
        }
    }

    @ViewBuilder
    private var buttonContent: some View {
        switch id {
        case "up":
            PadBtn(label: "▲", w: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), h: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), btn: .up)
        case "down":
            PadBtn(label: "▼", w: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), h: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), btn: .down)
        case "left":
            PadBtn(label: "◀", w: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), h: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), btn: .left)
        case "right":
            PadBtn(label: "▶", w: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), h: VirtualPadButtonOffset.dpadButtonWidth(isLandscape: isLandscape), btn: .right)
        case "triangle":
            PSBtn(sym: "△", clr: .green, sz: VirtualPadButtonOffset.actionButtonSize, btn: .triangle)
        case "cross":
            PSBtn(sym: "✕", clr: .blue, sz: VirtualPadButtonOffset.actionButtonSize, btn: .cross)
        case "square":
            PSBtn(sym: "□", clr: .pink, sz: VirtualPadButtonOffset.actionButtonSize, btn: .square)
        case "circle":
            PSBtn(sym: "○", clr: .red, sz: VirtualPadButtonOffset.actionButtonSize, btn: .circle)
        default:
            EmptyView()
        }
    }
}

// MARK: - Clamp helper
private extension CGFloat {
    func clamped(_ lo: CGFloat, _ hi: CGFloat) -> CGFloat {
        Swift.min(Swift.max(self, lo), hi)
    }
}
