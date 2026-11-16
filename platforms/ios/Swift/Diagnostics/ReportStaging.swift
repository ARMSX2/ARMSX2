// ReportStaging.swift — streams logs and small texts into the staging folder through redaction, capped in memory
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ReportStaging {
    static let chunkBytes = 4 << 20
    static let logCapBytes = 32 << 20
    static let headBytes = 256 << 10
    private static let overlapBytes = 64

    // nil means absent; a symlink or directory answers false and the caller refuses it.
    static func isRegularFile(_ url: URL) -> Bool? {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]) else { return nil }
        return values.isRegularFile == true && values.isSymbolicLink != true
    }

    // The size is sampled once, because the live log keeps growing while this reads it.
    @discardableResult
    static func stageLog(source: URL, as name: String, in folder: URL, context: ReportRedaction.Context) throws -> Bool {
        guard let regular = isRegularFile(source) else { return false }
        guard regular else { throw DiagnosticsError.notRegularFile(source.lastPathComponent) }
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        let output = try createOutput(name: name, in: folder)
        defer { try? output.close() }

        let size = try input.seekToEnd()
        guard size > UInt64(logCapBytes) else {
            try stream(input, from: 0, to: size, into: output, context: context)
            return true
        }
        let headEnd = try lastNewline(input, before: UInt64(headBytes)).map { $0 + 1 } ?? UInt64(headBytes)
        let marker = 40
        var tailStart = size - UInt64(logCapBytes - Int(headEnd) - marker)
        if let newline = try firstNewline(input, from: tailStart, limit: size) { tailStart = newline + 1 }
        try stream(input, from: 0, to: headEnd, into: output, context: context)
        try output.write(contentsOf: Data("-- \(tailStart - headEnd) bytes omitted --\n".utf8))
        try stream(input, from: tailStart, to: size, into: output, context: context)
        return true
    }

    @discardableResult
    static func stageText(source: URL, as name: String, in folder: URL, context: ReportRedaction.Context,
                          limit: Int = 8 << 20) throws -> Bool {
        guard let regular = isRegularFile(source) else { return false }
        guard regular else { throw DiagnosticsError.notRegularFile(source.lastPathComponent) }
        let size = (try? source.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
        guard size <= limit else { return false }
        let text = String(decoding: try Data(contentsOf: source), as: UTF8.self)
        let redacted = name.hasSuffix(".ini")
            ? ReportRedaction.redactINI(text, context: context) : ReportRedaction.redact(text, context: context)
        let output = try createOutput(name: name, in: folder)
        defer { try? output.close() }
        try output.write(contentsOf: Data(redacted.utf8))
        return true
    }

    // Names the staged file that still holds a needle, never the needle or the text around it.
    static func tripwireScan(stagingRoot: URL, context: ReportRedaction.Context) -> String? {
        let keys: [URLResourceKey] = [.isRegularFileKey]
        let entries = FileManager.default.enumerator(at: stagingRoot, includingPropertiesForKeys: keys)
        while let file = entries?.nextObject() as? URL {
            guard (try? file.resourceValues(forKeys: Set(keys)))?.isRegularFile == true,
                  let input = try? FileHandle(forReadingFrom: file) else { continue }
            defer { try? input.close() }
            var overlap = Data()
            var finished = false
            var hit = false
            while !finished {
                autoreleasepool {
                    guard let piece = try? input.read(upToCount: chunkBytes), !piece.isEmpty else { finished = true; return }
                    hit = !ReportRedaction.tripwireHits(in: String(decoding: overlap + piece, as: UTF8.self), context: context).isEmpty
                    overlap = Data(piece.suffix(overlapBytes))
                }
                if hit { return file.lastPathComponent }
            }
        }
        return nil
    }

    // A logged-out or switched account is gone from the INI, so only the kept logs still name it.
    static func scanLoginNames(in sources: [URL]) -> [String] {
        var names = Set<String>()
        for source in sources where isRegularFile(source) == true {
            try? SessionLogScanner.lines(of: source, containing: ReportRedaction.loginNeedles) {
                names.formUnion(ReportRedaction.loginNames(in: $0))
            }
        }
        return Array(names)
    }

    private static func createOutput(name: String, in folder: URL) throws -> FileHandle {
        let target = folder.appendingPathComponent(name)
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard FileManager.default.createFile(atPath: target.path, contents: nil) else {
            throw DiagnosticsError.zipFailed("cannot create \(name)")
        }
        return try FileHandle(forWritingTo: target)
    }

    // Cuts each read at its last newline and carries the partial line, so no rule sees half a line.
    private static func stream(_ input: FileHandle, from start: UInt64, to end: UInt64, into output: FileHandle,
                               context: ReportRedaction.Context) throws {
        var offset = start
        var carry = Data()
        try input.seek(toOffset: start)
        while offset < end {
            try autoreleasepool {
                let piece = try input.read(upToCount: Int(min(UInt64(chunkBytes), end - offset))) ?? Data()
                var block = carry + piece
                carry = Data()
                offset = piece.isEmpty ? end : offset + UInt64(piece.count)
                // A block with no newline is kept for the next read, up to twice the chunk size.
                if offset < end, block.count < 2 * chunkBytes {
                    guard let newline = block.lastIndex(of: 0x0A) else { carry = block; return }
                    carry = Data(block[(newline + 1)...])
                    block = Data(block[...newline])
                }
                let text = ReportRedaction.redact(String(decoding: block, as: UTF8.self), context: context)
                try output.write(contentsOf: Data(text.utf8))
            }
        }
    }

    private static func lastNewline(_ input: FileHandle, before limit: UInt64) throws -> UInt64? {
        try input.seek(toOffset: 0)
        let head = try input.read(upToCount: Int(limit)) ?? Data()
        return head.lastIndex(of: 0x0A).map { UInt64($0) }
    }

    private static func firstNewline(_ input: FileHandle, from start: UInt64, limit: UInt64) throws -> UInt64? {
        try input.seek(toOffset: start)
        let piece = try input.read(upToCount: Int(min(1 << 20, limit - start))) ?? Data()
        return piece.firstIndex(of: 0x0A).map { start + UInt64($0) }
    }
}
