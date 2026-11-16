// CrashDiagnostics.swift — files the OS's crash and hang payloads under the session they belong to
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import MetricKit
import os

final class CrashDiagnostics: Sendable {
    static let shared = CrashDiagnostics()
    private let started = OSAllocatedUnfairLock(initialState: false)
    private let subscriber = Subscriber()

    func start() {
        guard !started.withLock({ let was = $0; $0 = true; return was }) else { return }
        MXMetricManager.shared.add(subscriber)
    }

    private final class Subscriber: NSObject, MXMetricManagerSubscriber, Sendable {
        func didReceive(_ payloads: [MXDiagnosticPayload]) {
            for payload in payloads {
                SessionStore.persistPayload(payload.jsonRepresentation(), begin: payload.timeStampBegin, end: payload.timeStampEnd)
            }
        }
    }
}
