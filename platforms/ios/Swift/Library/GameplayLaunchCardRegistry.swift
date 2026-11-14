// GameplayLaunchCardRegistry.swift — Card frames the gameplay launch transition starts from
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

@MainActor
final class GameplayLaunchCardRegistry {
    private final class WeakCardView {
        weak var view: UIView?

        init(_ view: UIView) {
            self.view = view
        }
    }

    private var cards: [String: WeakCardView] = [:]

    func register(_ view: UIView, for gameID: String) {
        guard cards[gameID]?.view !== view else { return }
        cards[gameID] = WeakCardView(view)
    }

    func unregister(_ view: UIView, for gameID: String) {
        guard cards[gameID]?.view === view else { return }
        cards.removeValue(forKey: gameID)
    }

    func view(for gameID: String) -> UIView? {
        guard let view = cards[gameID]?.view else {
            cards.removeValue(forKey: gameID)
            return nil
        }
        return view
    }

    /// Resolves a touch against the small set of currently mounted lazy cards.
    /// This lets touch browsing use one long-press recognizer per scroll view
    /// instead of attaching one recognizer per card.
    func gameID(at location: CGPoint, in coordinateView: UIView) -> String? {
        cards = cards.filter { $0.value.view != nil }
        for (gameID, weakCard) in cards {
            guard let cardView = weakCard.view,
                  cardView.window != nil,
                  !cardView.isHidden,
                  cardView.alpha >= 0.01 else { continue }
            let localPoint = cardView.convert(location, from: coordinateView)
            if cardView.bounds.contains(localPoint) {
                return gameID
            }
        }
        return nil
    }

    func removeAll() {
        cards.removeAll(keepingCapacity: false)
    }
}

@MainActor
struct GameplayLaunchCardRegistrationView: UIViewRepresentable {
    let gameID: String
    let registry: GameplayLaunchCardRegistry

    func makeCoordinator() -> Coordinator {
        Coordinator(gameID: gameID, registry: registry)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isUserInteractionEnabled = false
        view.accessibilityElementsHidden = true
        context.coordinator.registry.register(view, for: gameID)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        if context.coordinator.gameID != gameID {
            context.coordinator.registry.unregister(
                uiView,
                for: context.coordinator.gameID
            )
            context.coordinator.gameID = gameID
        }
        context.coordinator.registry.register(uiView, for: gameID)
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView: UIView,
        context: Context
    ) -> CGSize? {
        guard let width = proposal.width, let height = proposal.height else {
            return nil
        }
        return CGSize(width: width, height: height)
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.registry.unregister(uiView, for: coordinator.gameID)
    }

    @MainActor
    final class Coordinator {
        var gameID: String
        let registry: GameplayLaunchCardRegistry

        init(gameID: String, registry: GameplayLaunchCardRegistry) {
            self.gameID = gameID
            self.registry = registry
        }
    }
}

extension View {
    @MainActor
    func registerGameplayLaunchCard(
        for gameID: String,
        in registry: GameplayLaunchCardRegistry,
        isEnabled: Bool = true
    ) -> some View {
        overlay {
            if isEnabled {
                GeometryReader { geometry in
                    GameplayLaunchCardRegistrationView(
                        gameID: gameID,
                        registry: registry
                    )
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
                    .allowsHitTesting(false)
                }
            }
        }
    }
}
