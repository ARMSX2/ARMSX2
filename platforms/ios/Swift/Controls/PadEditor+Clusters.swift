// PadEditor+Clusters.swift — the D-pad and the face buttons as one piece until they are split
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

extension PadEditor {
    /// A cluster is split once any of its buttons has an entry of its own in this orientation.
    func isSplit(_ cluster: String) -> Bool {
        PadLayoutStore.clusterMembers[cluster]?.contains { position(of: $0) != nil } ?? false
    }

    var joinedClusters: Set<String> { Set(PadLayoutStore.clusterMembers.keys.filter { !isSplit($0) }) }

    var pieces: [String] {
        let joined = joinedClusters
        return Self.pieceIDs.compactMap { id in
            let cluster = PadLayoutStore.groupID(for: id)
            guard cluster != id, joined.contains(cluster) else { return id }
            return PadLayoutStore.clusterMembers[cluster]?.first == id ? cluster : nil
        }
    }

    func mirrored(_ id: String) -> String? {
        Self.mirrorPartner[id].flatMap { pieces.contains($0) ? $0 : nil }
    }

    /// Each button starts where it is drawn now, at the cluster's size and opacity.
    func split(_ cluster: String) {
        guard let members = PadLayoutStore.clusterMembers[cluster], !isSplit(cluster) else { return }
        record()
        let drawn = members.map { ($0, resolved($0)) }
        for (member, p) in drawn { write(member, p) }
        selection = members.first
    }

    func join(_ cluster: String) {
        guard let members = PadLayoutStore.clusterMembers[cluster], isSplit(cluster) else { return }
        record()
        for member in members { write(member, nil) }
        selection = cluster
    }
}
