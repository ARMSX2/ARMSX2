// SessionLogScanner.swift — streaming reader for the marker lines in a session log
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct MarkerLine {
    let tag: String
    let fields: [String: String]

    // Quote-aware so title="Ratchet & Clank" stays one value.
    static func parse(_ line: String) -> MarkerLine? {
        guard let open = line.range(of: "@@"),
              let close = line.range(of: "@@", range: open.upperBound..<line.endIndex) else { return nil }
        var fields: [String: String] = [:]
        var index = close.upperBound
        while index < line.endIndex {
            while index < line.endIndex, line[index] == " " { index = line.index(after: index) }
            let keyStart = index
            while index < line.endIndex, line[index] != "=", line[index] != " " { index = line.index(after: index) }
            guard index < line.endIndex, line[index] == "=" else { continue }
            let key = String(line[keyStart..<index])
            index = line.index(after: index)
            var value = ""
            if index < line.endIndex, line[index] == "\"" {
                index = line.index(after: index)
                while index < line.endIndex, line[index] != "\"" {
                    value.append(line[index])
                    index = line.index(after: index)
                }
                if index < line.endIndex { index = line.index(after: index) }
            } else {
                let valueStart = index
                while index < line.endIndex, line[index] != " " { index = line.index(after: index) }
                value = String(line[valueStart..<index])
            }
            fields[key] = value
        }
        return MarkerLine(tag: String(line[open.upperBound..<close.lowerBound]), fields: fields)
    }
}

enum SessionLogScanner {
    private static let chunkSize = 1 << 20
    private static let newline = UInt8(ascii: "\n")

    // Hands over only the lines that carry a marker or the crash handler's abort phrase.
    static func markerLines(of url: URL, _ body: (String) -> Void) throws {
        try scan(url, keep: { $0.contains(UInt8(ascii: "@")) || $0.contains(UInt8(ascii: "A")) }) { line in
            if line.contains("@@") || line.contains("Aborting application.") { body(line) }
        }
    }

    // Emulog lines carry no marker, so the summary asks for the few it needs by text.
    static func lines(of url: URL, containing needles: [String], _ body: (String) -> Void) throws {
        let patterns = needles.map { Data($0.utf8) }
        try scan(url, keep: { slice in patterns.contains { slice.range(of: $0) != nil } }, body)
    }

    private static func scan(_ url: URL, keep: (Data.SubSequence) -> Bool, _ body: (String) -> Void) throws {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var carry = Data()
        var reading = true
        while reading {
            try autoreleasepool {
                guard let chunk = try handle.read(upToCount: chunkSize), !chunk.isEmpty else {
                    reading = false
                    return
                }
                carry.append(chunk)
                guard let cut = carry.lastIndex(of: newline) else { return }
                emit(carry[carry.startIndex...cut], keep, body)
                carry.removeSubrange(carry.startIndex...cut)
            }
        }
        emit(carry, keep, body)
    }

    private static func emit(_ block: Data, _ keep: (Data.SubSequence) -> Bool, _ body: (String) -> Void) {
        for slice in block.split(separator: newline, omittingEmptySubsequences: true) where keep(slice) {
            body(String(decoding: slice, as: UTF8.self))
        }
    }
}
