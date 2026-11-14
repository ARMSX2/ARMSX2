// SettingsClearGlassListInstaller.swift — clear glass behind Settings rows
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

/// Every Settings destination stays transparent above RootView's one persistent
/// menu renderer. Non-Appearance Forms receive the same independent rounded
/// row glass that Appearance declares directly; no full-screen glass slab is
/// inserted between the list and the background.
private struct SettingsClearGlassListInstaller: UIViewRepresentable {
    let isClear: Bool

    func makeUIView(context: Context) -> UIView {
        let view = SettingsClearGlassListInstallerView()
        view.configure(isClear: isClear)
        view.scheduleInstallation()
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        guard let view = uiView as? SettingsClearGlassListInstallerView else {
            return
        }
        view.configure(isClear: isClear)
        view.scheduleInstallation()
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Void) {
        (uiView as? SettingsClearGlassListInstallerView)?.disconnect()
    }
}

/// Marks a SwiftUI row that owns its Liquid Glass surface. The native Form
/// decorator must not install a second UIVisualEffectView behind such a row.
/// Shader accordions use this because their height changes in place while the
/// glass surface itself must remain mounted.
@MainActor
final class SettingsOwnedClearGlassMarkerView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        isAccessibilityElement = false
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

struct SettingsOwnedClearGlassMarker: UIViewRepresentable {
    func makeUIView(context: Context) -> SettingsOwnedClearGlassMarkerView {
        SettingsOwnedClearGlassMarkerView()
    }

    func updateUIView(
        _ uiView: SettingsOwnedClearGlassMarkerView,
        context: Context
    ) {}
}

/// A true background-only rounded surface for Settings rows. Appearance uses a
/// fixed 16-point continuous radius, so compact rows can read as controls while
/// taller explanatory and compound rows remain cards instead of all becoming
/// capsules. Keeping the effect behind the cell leaves text and symbols plain.
@MainActor
private final class SettingsClearGlassRowBackgroundView: UIView {
    private let effectView: UIVisualEffectView
    private var isClear: Bool

    init(isClear: Bool) {
        self.isClear = isClear
        if #available(iOS 26.0, *) {
            effectView = UIVisualEffectView(
                effect: UIGlassEffect(style: isClear ? .clear : .regular)
            )
        } else {
            effectView = UIVisualEffectView(
                effect: UIBlurEffect(
                    style: isClear
                        ? .systemUltraThinMaterial
                        : .systemMaterial
                )
            )
        }
        super.init(frame: .zero)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        effectView.isUserInteractionEnabled = false
        effectView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(effectView)
        NSLayoutConstraint.activate([
            effectView.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            effectView.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            effectView.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            effectView.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func configure(isClear: Bool) {
        guard self.isClear != isClear else { return }
        self.isClear = isClear
        if #available(iOS 26.0, *) {
            effectView.effect = UIGlassEffect(
                style: isClear ? .clear : .regular
            )
        } else {
            effectView.effect = UIBlurEffect(
                style: isClear
                    ? .systemUltraThinMaterial
                    : .systemMaterial
            )
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        effectView.layer.cornerRadius = min(
            16,
            max(0, effectView.bounds.height / 2)
        )
        effectView.layer.cornerCurve = .continuous
        effectView.layer.masksToBounds = true
    }

    /// Form cells animate their height independently from SwiftUI content.
    /// Explicitly following the cell's intermediate bounds keeps the clear
    /// glass sampler attached throughout a shader-card expansion instead of
    /// disappearing until the final layout pass.
    func synchronize() {
        setNeedsLayout()
    }
}

@MainActor
private final class SettingsClearGlassListInstallerView: UIView {
    private weak var listScrollView: UIScrollView?
    private var contentOffsetObservation: NSKeyValueObservation?
    private var contentSizeObservation: NSKeyValueObservation?
    private var pendingInstallation: Task<Void, Never>?
    private var pendingDecoration: Task<Void, Never>?
    private var isClear = true

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = false
        isAccessibilityElement = false
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        scheduleInstallation()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        scheduleInstallation()
    }

    func scheduleInstallation() {
        // Coalesce layout invalidations without continually cancelling the
        // already queued installation while a controller scroll is running.
        guard pendingInstallation == nil else { return }
        pendingInstallation = Task { @MainActor [weak self] in
            await Task.yield()
            guard !Task.isCancelled, let self else { return }
            installIfPossible()
            pendingInstallation = nil
        }
    }

    func configure(isClear: Bool) {
        guard self.isClear != isClear else { return }
        self.isClear = isClear
        scheduleVisibleCellDecoration()
    }

    func disconnect() {
        pendingInstallation?.cancel()
        pendingInstallation = nil
        pendingDecoration?.cancel()
        pendingDecoration = nil
        contentOffsetObservation = nil
        contentSizeObservation = nil
        listScrollView = nil
    }

    private func installIfPossible() {
        guard window != nil else {
            disconnect()
            return
        }
        guard let candidate = resolveListScrollView() else { return }
        if listScrollView !== candidate {
            observe(candidate)
        }
        decorateVisibleCells()
    }

    private func observe(_ scrollView: UIScrollView) {
        contentOffsetObservation = nil
        contentSizeObservation = nil
        listScrollView = scrollView

        contentOffsetObservation = scrollView.observe(
            \.contentOffset,
            options: [.new]
        ) { [weak self] _, _ in
            MainActor.assumeIsolated {
                self?.scheduleVisibleCellDecoration()
            }
        }
        contentSizeObservation = scrollView.observe(
            \.contentSize,
            options: [.new]
        ) { [weak self] _, _ in
            MainActor.assumeIsolated {
                self?.scheduleVisibleCellDecoration()
            }
        }
    }

    /// A List can publish several offset and size changes during one rendered
    /// frame. Decorate its newly recycled cells once instead of enqueueing a
    /// complete visible-cell traversal for every KVO callback.
    private func scheduleVisibleCellDecoration() {
        guard pendingDecoration == nil else { return }
        pendingDecoration = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(8))
            guard !Task.isCancelled, let self else { return }
            UIView.performWithoutAnimation {
                self.decorateVisibleCells()
            }
            pendingDecoration = nil
        }
    }

    private func resolveListScrollView() -> UIScrollView? {
        if let listScrollView,
           listScrollView.window === window,
           isListView(listScrollView) {
            return listScrollView
        }
        guard let window else { return nil }
        let markerFrame = convert(bounds, to: window)
        guard markerFrame.width > 1, markerFrame.height > 1 else { return nil }

        var ancestor = superview
        for _ in 0..<8 {
            guard let root = ancestor, root !== window else { break }
            var candidates: [UIScrollView] = []
            collectListViews(
                below: root,
                in: window,
                intersecting: markerFrame,
                into: &candidates
            )
            if let candidate = candidates.max(by: {
                overlapArea(of: $0, with: markerFrame, in: window)
                    < overlapArea(of: $1, with: markerFrame, in: window)
            }) {
                return candidate
            }
            ancestor = root.superview
        }
        return nil
    }

    private func collectListViews(
        below view: UIView,
        in window: UIWindow,
        intersecting markerFrame: CGRect,
        into result: inout [UIScrollView]
    ) {
        guard !view.isHidden,
              view.alpha >= 0.01,
              view.window === window else { return }
        let frame = view.convert(view.bounds, to: window)
        guard frame.intersects(markerFrame) else { return }
        if let scrollView = view as? UIScrollView,
           isListView(scrollView) {
            result.append(scrollView)
        }
        for child in view.subviews.reversed() {
            collectListViews(
                below: child,
                in: window,
                intersecting: markerFrame,
                into: &result
            )
        }
    }

    private func isListView(_ view: UIScrollView) -> Bool {
        view is UICollectionView || view is UITableView
    }

    private func overlapArea(
        of view: UIView,
        with markerFrame: CGRect,
        in window: UIWindow
    ) -> CGFloat {
        let intersection = view.convert(view.bounds, to: window)
            .intersection(markerFrame)
        guard !intersection.isNull else { return 0 }
        return intersection.width * intersection.height
    }

    private func decorateVisibleCells() {
        guard let listScrollView else { return }
        if listScrollView.backgroundColor != .clear {
            listScrollView.backgroundColor = .clear
        }

        if let collectionView = listScrollView as? UICollectionView {
            for cell in collectionView.visibleCells {
                decorate(cell)
            }
        } else if let tableView = listScrollView as? UITableView {
            if tableView.separatorStyle != .none { tableView.separatorStyle = .none }
            for cell in tableView.visibleCells {
                decorate(cell)
            }
        }
    }

    private func decorate(_ cell: UICollectionViewCell) {
        if cell.backgroundColor != .clear { cell.backgroundColor = .clear }
        if cell.contentView.backgroundColor != .clear {
            cell.contentView.backgroundColor = .clear
        }
        if cell.backgroundConfiguration != nil {
            cell.backgroundConfiguration = nil
        }
        if containsOwnedGlassMarker(in: cell.contentView) {
            if cell.backgroundView is SettingsClearGlassRowBackgroundView {
                cell.backgroundView = nil
            }
            return
        }
        if let background = cell.backgroundView
            as? SettingsClearGlassRowBackgroundView {
            background.configure(isClear: isClear)
            background.synchronize()
        } else {
            let background = SettingsClearGlassRowBackgroundView(
                isClear: isClear
            )
            cell.backgroundView = background
            background.synchronize()
        }
    }

    private func decorate(_ cell: UITableViewCell) {
        if cell.backgroundColor != .clear { cell.backgroundColor = .clear }
        if cell.contentView.backgroundColor != .clear {
            cell.contentView.backgroundColor = .clear
        }
        if cell.backgroundConfiguration != nil {
            cell.backgroundConfiguration = nil
        }
        if containsOwnedGlassMarker(in: cell.contentView) {
            if cell.backgroundView is SettingsClearGlassRowBackgroundView {
                cell.backgroundView = nil
            }
            return
        }
        if let background = cell.backgroundView
            as? SettingsClearGlassRowBackgroundView {
            background.configure(isClear: isClear)
            background.synchronize()
        } else {
            let background = SettingsClearGlassRowBackgroundView(
                isClear: isClear
            )
            cell.backgroundView = background
            background.synchronize()
        }
        if cell.selectedBackgroundView == nil {
            cell.selectedBackgroundView = UIView()
        }
        if cell.selectedBackgroundView?.backgroundColor != .clear {
            cell.selectedBackgroundView?.backgroundColor = .clear
        }
    }

    private func containsOwnedGlassMarker(in view: UIView) -> Bool {
        if view is SettingsOwnedClearGlassMarkerView { return true }
        return view.subviews.contains { containsOwnedGlassMarker(in: $0) }
    }
}

struct SettingsDetailClearLiquidGlassModifier: ViewModifier {
    let decoratesRows: Bool
    let isClear: Bool

    func body(content: Content) -> some View {
        content
            // Native Form glass is installed in UIKit cell backgrounds. Read
            // each focused target's anchor at the Form boundary so the depth
            // copy is composited below those materials, not above them.
            .environment(
                \.controllerFocusDepthHandledByAncestor,
                decoratesRows
            )
            .scrollContentBackground(.hidden)
            .controllerFocusDepthBehindGlass(
                isEnabled: decoratesRows,
                force: true,
                cornerRadius: 16
            )
            .background(Color.clear)
            .overlay {
                if decoratesRows {
                    SettingsClearGlassListInstaller(isClear: isClear)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }
            }
            .stableMenuContentGlassContainer()
            .clearNavigationContainerBackground()
            .toolbarBackground(.hidden, for: .navigationBar)
    }
}
