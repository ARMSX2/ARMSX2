// SessionStore+Offer.swift — which past session the launch report prompt asks about
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension SessionStore {
    // Its own file: finalize rewrites session.json at every launch and would lose a key.
    static let offeredMarkerName = "offered"

    static func isOffered(_ id: String, documents: URL = .documentsDirectory) -> Bool {
        guard let folder = url(for: id, documents: documents) else { return false }
        return FileManager.default.fileExists(atPath: folder.appendingPathComponent(offeredMarkerName).path)
    }

    // The target and every older session, so only the newest bad launch ever asks.
    static func markHandled(target: String, current: String?, documents: URL = .documentsDirectory) {
        for id in list(documents: documents) where id <= target && id != current && !isOffered(id, documents: documents) {
            guard let folder = url(for: id, documents: documents) else { continue }
            FileManager.default.createFile(atPath: folder.appendingPathComponent(offeredMarkerName).path, contents: nil)
        }
    }
}

struct OfferTarget {
    let id: String
    let ending: Ending
    let title: String
}

enum LaunchOffer {
    static func pick(documents: URL = .documentsDirectory, current: String?) -> OfferTarget? {
        for id in SessionStore.list(documents: documents).sorted(by: >)
        where id != current && !SessionStore.isOffered(id, documents: documents) {
            let row = DiagnosticsRows.load(id: id, isNow: false, documents: documents)
            if let ending = row.ending, ending.needsAttention {
                return OfferTarget(id: id, ending: ending, title: row.title)
            }
        }
        return nil
    }
}
