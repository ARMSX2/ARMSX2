// ShaderParams.swift — a preset's own numbers, tweaked, applied and saved
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ShaderParamsError: LocalizedError {
    case noName
    case missingBase
    case noSavedRoot
    case wouldOverwriteBase

    var errorDescription: String? {
        switch self {
        case .noName:
            return "Use letters or numbers in the name."
        case .missingBase:
            return "The preset these values came from is gone, so nothing was saved."
        case .noSavedRoot:
            return "Your shader folder couldn't be opened."
        case .wouldOverwriteBase:
            return "That name belongs to the preset you're changing. Pick another name."
        }
    }
}

/// What stops a preset loading, read off the path librashader quotes in its error.
enum ShaderPresetFailure: Equatable, Sendable {
    case needsBasePack
    case needsReimport
    case missing(String)

    init(_ error: Error, preset: URL) {
        let quoted = error.localizedDescription.components(separatedBy: "\"")
        let path = quoted.count > 2 && quoted[1].hasPrefix("/") ? quoted[1] : preset.path
        let base = "/" + ShaderPresetLibrary.basePackFolderName + "/"
        // librashader keeps ../ in the quoted path, and on a device it starts with /private/var.
        let bare = path.hasPrefix("/private/") ? String(path.dropFirst("/private".count)) : path
        if path.contains("/../"),
           [path, bare].allSatisfy({ ShaderPresetLibrary.token(for: URL(fileURLWithPath: $0).standardized) == nil }) {
            self = .needsReimport
        } else {
            self = path.contains(base) && !ShaderPresetLibrary.hasBasePack
                ? .needsBasePack : .missing((path as NSString).lastPathComponent)
        }
    }
}

/// One tweakable value a `.slangp` declares. Its numbers can be missing, non-finite, inverted
/// or zero-step, so each one is checked before use.
struct ShaderParam: Identifiable, Hashable, Sendable {
    let name: String
    let description: String
    let initial: Float
    let minimum: Float
    let maximum: Float
    let step: Float

    var id: String { name }
    var label: String { description.isEmpty ? name : description }

    /// Whether the range allows movement. Names and descriptions are not read, since stock packs
    /// give working sliders heading-like names.
    var isAdjustable: Bool { maximum > minimum }

    /// The step a control moves by. A zero, negative or wider-than-range step counts as none, as in
    /// RetroArch, and moves by a hundredth of the range.
    var increment: Float {
        let span = maximum - minimum
        guard span > 0 else { return 0 }
        return (step > 0 && step <= span) ? step : span / 100
    }

    var decimals: Int {
        switch increment {
        case 1...: return 0
        case 0.1...: return 1
        case 0.01...: return 2
        default: return 3
        }
    }

    func clamped(_ value: Float) -> Float {
        guard isAdjustable, !value.isNaN else { return initial }
        return min(max(value, minimum), maximum)
    }

    func isInitial(_ value: Float) -> Bool {
        abs(value - initial) < max(increment / 2, 1e-6)
    }
}

extension ShaderParam: Decodable {
    private enum Key: String, CodingKey {
        case name, description, initial, minimum, maximum, step
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: Key.self)
        name = (try? values.decode(String.self, forKey: .name)) ?? ""
        description = (try? values.decode(String.self, forKey: .description)) ?? ""
        initial = Self.number(values, .initial)
        minimum = Self.number(values, .minimum)
        maximum = Self.number(values, .maximum)
        step = Self.number(values, .step)
    }

    private static func number(_ values: KeyedDecodingContainer<Key>, _ key: Key) -> Float {
        guard let value = (try? values.decodeIfPresent(Float.self, forKey: key)) ?? nil,
              value.isFinite else { return 0 }
        return value
    }
}

/// A preset's parameters and the user's overrides, pushed to the running chain and saved as presets.
@MainActor
final class ShaderParams: ObservableObject {
    @Published private(set) var params: [ShaderParam] = []
    @Published private(set) var overrides: [String: Float] = [:]
    @Published private(set) var isLoading = false
    @Published private(set) var errorText: String?
    @Published private(set) var loadFailure: ShaderPresetFailure?

    nonisolated static let section = "EmuCore/GS"
    nonisolated static let key = "ShaderChainParams"
    nonisolated static let invariant = Locale(identifier: "en_US_POSIX")

    private var token = ""
    private var generation = 0

    var hasOverrides: Bool { !overrides.isEmpty }

    func load(token newToken: String) async {
        generation += 1
        let current = generation
        token = newToken
        errorText = nil
        loadFailure = nil
        params = []
        guard let url = ShaderPresetLibrary.resolve(newToken) else {
            overrides = [:]
            isLoading = false
            return
        }
        overrides = Self.stored()[newToken] ?? [:]
        isLoading = true
        let result = await Task.detached(priority: .userInitiated) {
            Result { try Self.parameters(at: url) }
        }.value
        guard current == generation else { return }
        isLoading = false
        switch result {
        case .success(let decoded):
            params = decoded
            if let failure = ARMSX2Bridge.shaderChainError(forPreset: url.path) {
                loadFailure = ShaderPresetFailure(failure, preset: url)
            }
        case .failure(let error):
            loadFailure = ShaderPresetFailure(error, preset: url)
        }
        pushEffective()
    }

    func value(for param: ShaderParam) -> Float {
        param.clamped(overrides[param.name] ?? param.initial)
    }

    func setValue(_ value: Float, for param: ShaderParam) {
        let settled = param.clamped(value)
        overrides[param.name] = param.isInitial(settled) ? nil : settled
        persist()
        pushEffective()
    }

    func reset(_ param: ShaderParam) {
        overrides[param.name] = nil
        persist()
        pushEffective()
    }

    func resetAll() {
        overrides = [:]
        persist()
        pushEffective()
    }

    /// A generated multi-layer preset can be parsed by a renderer that only
    /// reports parameters from one inherited preset. Merge the parameters
    /// discovered from each source layer so every visible layer remains
    /// adjustable and receives its effective runtime value.
    func includeAdditionalParameters(_ additional: [ShaderParam]) {
        guard !additional.isEmpty else { return }
        var known = Set(params.map(\.name))
        var merged = params
        for param in additional where known.insert(param.name).inserted {
            merged.append(param)
        }
        guard merged != params else { return }
        params = merged
        pushEffective()
    }

    func save(as name: String) async -> String? {
        errorText = nil
        let safe = Self.safeName(name)
        guard !safe.isEmpty else {
            errorText = ShaderParamsError.noName.errorDescription
            return nil
        }
        guard let base = ShaderPresetLibrary.resolve(token) else {
            errorText = ShaderParamsError.missingBase.errorDescription
            return nil
        }
        let text = Self.presetText(base: base, params: params, overrides: overrides)
        do {
            let url = try await Task.detached(priority: .userInitiated) {
                try Self.write(text, named: safe, base: base)
            }.value
            guard let saved = ShaderPresetLibrary.token(for: url) else { return nil }
            var store = Self.stored()
            if store.removeValue(forKey: saved) != nil { Self.storeOverrides(store) }
            return saved
        } catch {
            errorText = error.localizedDescription
            return nil
        }
    }

    /// Sends every parameter's effective value, since librashader has no call to unset one.
    private func pushEffective() {
        guard !params.isEmpty, let url = ShaderPresetLibrary.resolve(token) else { return }
        var effective: [String: NSNumber] = [:]
        effective.reserveCapacity(params.count)
        for param in params {
            effective[param.name] = NSNumber(value: value(for: param))
        }
        ARMSX2Bridge.setShaderChainParameters(effective, forPreset: url.path)
    }

    /// Pushes a preset's saved overrides with no shader screen open, at launch and per-game boot.
    /// Only overrides go down; a new chain already holds the preset's own values. nonisolated so
    /// the per-game boot path can call it before bootISO.
    nonisolated static func pushStored(token: String) {
        guard let url = ShaderPresetLibrary.resolve(token) else { return }
        var values: [String: NSNumber] = [:]
        for (name, value) in stored()[token] ?? [:] where value.isFinite {
            values[name] = NSNumber(value: value)
        }
        guard !values.isEmpty else { return }
        ARMSX2Bridge.setShaderChainParameters(values, forPreset: url.path)
    }

    private func persist() {
        guard !token.isEmpty else { return }
        var store = Self.stored()
        store[token] = overrides.isEmpty ? nil : overrides
        Self.storeOverrides(store)
    }

    private static func storeOverrides(_ store: [String: [String: Float]]) {
        guard let data = try? JSONEncoder().encode(store),
              let json = String(data: data, encoding: .utf8) else { return }
        ARMSX2Bridge.setINIString(section, key: key, value: json)
    }

    private nonisolated static func stored() -> [String: [String: Float]] {
        let json = ARMSX2Bridge.getINIString(section, key: key, defaultValue: "")
        guard let data = json.data(using: .utf8),
              let decoded = try? JSONDecoder()
                .decode([String: [String: Float]].self, from: data) else { return [:] }
        return decoded
    }

    nonisolated static func parameters(at url: URL) throws -> [ShaderParam] {
        do {
            let json = try ARMSX2Bridge.shaderPresetParameters(atPath: url.path)
            if let data = json.data(using: .utf8),
               let decoded = try? JSONDecoder().decode([ShaderParam].self, from: data) {
                return decoded.filter { !$0.name.isEmpty }
            }
        } catch {
            let fallback = ShaderPresetParameterFallbackParser.parameters(in: url)
            if !fallback.isEmpty { return fallback }
            throw error
        }

        // Preset download and configuration belong to the Swift frontend, not
        // to the optional renderer. When librashader is absent, inspect the
        // same #pragma parameter declarations directly so the editor remains
        // useful and its choices are ready for a renderer-enabled build.
        return ShaderPresetParameterFallbackParser.parameters(in: url)
    }

    // MARK: - Saving

    /// A RetroArch simple preset: a #reference to the base plus the changed values. Those read back
    /// as each parameter's initial, so Reset on a saved preset returns to the saved value.
    private static func presetText(
        base: URL, params: [ShaderParam], overrides: [String: Float]
    ) -> String {
        var text = "#reference \"" + reference(to: base) + "\"\n\n"
        for param in params {
            guard let value = overrides[param.name] else { continue }
            text += param.name + " = \"" + String(format: "%.6f", locale: invariant, value) + "\"\n"
        }
        return text
    }

    /// Relative when the base is under the user root, so the pair survives a new container UUID.
    /// A bundled base gets an absolute path, which launch re-roots after a reinstall.
    private static func reference(to base: URL) -> String {
        let target = base.standardizedFileURL
        guard let root = ShaderPresetLibrary.userRoot?.standardizedFileURL,
              let origin = ShaderPresetLibrary.savedPresetRoot?.standardizedFileURL,
              target.path.hasPrefix(root.path + "/") else { return target.path }
        let to = target.pathComponents
        let from = origin.pathComponents
        var shared = 0
        while shared < to.count, shared < from.count, to[shared] == from[shared] { shared += 1 }
        let up = Array(repeating: "..", count: from.count - shared)
        return (up + to[shared...]).joined(separator: "/")
    }

    private nonisolated static func write(_ text: String, named name: String,
                                          base: URL) throws -> URL {
        guard ShaderPresetLibrary.prepareUserRoots() != nil,
              let root = ShaderPresetLibrary.savedPresetRoot?.standardizedFileURL else {
            throw ShaderParamsError.noSavedRoot
        }
        let url = root.appendingPathComponent(name)
            .appendingPathExtension(ShaderPresetLibrary.presetExtension).standardizedFileURL
        guard url.deletingLastPathComponent().path == root.path else {
            throw ShaderParamsError.noName
        }
        // The sheet pre-fills the base's name, and saving onto the base would reference itself.
        guard url.path != base.standardizedFileURL.path else {
            throw ShaderParamsError.wouldOverwriteBase
        }
        try text.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    private static func safeName(_ name: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: " _-"))
        let scalars = name.unicodeScalars.map { allowed.contains($0) ? $0 : Unicode.Scalar("_") }
        return String(String.UnicodeScalarView(scalars)).trimmingCharacters(in: .whitespaces)
    }
}

/// Lightweight RetroArch preset inspection used only when librashader was not
/// linked. Rendering still belongs to librashader; this parser merely exposes
/// author-declared controls and their preset overrides to the settings UI.
private enum ShaderPresetParameterFallbackParser {
    private struct Definition {
        var description: String
        var initial: Float
        var minimum: Float
        var maximum: Float
        var step: Float
    }

    nonisolated static func parameters(in presetURL: URL) -> [ShaderParam] {
        var presetVisits = Set<String>()
        var shaderVisits = Set<String>()
        var definitions: [String: Definition] = [:]
        var order: [String] = []
        var overrides: [String: Float] = [:]

        inspectPreset(
            presetURL,
            presetVisits: &presetVisits,
            shaderVisits: &shaderVisits,
            definitions: &definitions,
            order: &order,
            overrides: &overrides
        )

        return order.compactMap { name in
            guard let definition = definitions[name] else { return nil }
            let initial = overrides[name] ?? definition.initial
            return ShaderParam(
                name: name,
                description: definition.description,
                initial: min(max(initial, definition.minimum), definition.maximum),
                minimum: definition.minimum,
                maximum: definition.maximum,
                step: definition.step
            )
        }
    }

    private nonisolated static func inspectPreset(
        _ url: URL,
        presetVisits: inout Set<String>,
        shaderVisits: inout Set<String>,
        definitions: inout [String: Definition],
        order: inout [String],
        overrides: inout [String: Float]
    ) {
        let resolvedURL = url.standardizedFileURL
        guard presetVisits.insert(resolvedURL.path).inserted,
              let text = try? String(contentsOf: resolvedURL, encoding: .utf8)
        else { return }

        let lines = text.components(separatedBy: .newlines)
        for line in lines {
            guard let reference = directivePath("#reference", in: line),
                  let referenceURL = resolve(reference, relativeTo: resolvedURL)
            else { continue }
            inspectPreset(
                referenceURL,
                presetVisits: &presetVisits,
                shaderVisits: &shaderVisits,
                definitions: &definitions,
                order: &order,
                overrides: &overrides
            )
        }

        let assignments = dictionary(from: lines)
        for key in assignments.keys
        where key.hasPrefix("shader")
            && Int(key.dropFirst("shader".count)) != nil {
            guard let path = assignments[key],
                  let shaderURL = resolve(path, relativeTo: resolvedURL)
            else { continue }
            inspectShader(
                shaderURL,
                visits: &shaderVisits,
                definitions: &definitions,
                order: &order
            )
        }

        for (name, value) in assignments {
            guard definitions[name] != nil,
                  let number = Float(value), number.isFinite else { continue }
            overrides[name] = number
        }
    }

    private nonisolated static func inspectShader(
        _ url: URL,
        visits: inout Set<String>,
        definitions: inout [String: Definition],
        order: inout [String]
    ) {
        let resolvedURL = url.standardizedFileURL
        guard visits.insert(resolvedURL.path).inserted,
              let text = try? String(contentsOf: resolvedURL, encoding: .utf8)
        else { return }

        for line in text.components(separatedBy: .newlines) {
            if let include = directivePath("#include", in: line),
               let includeURL = resolve(include, relativeTo: resolvedURL) {
                inspectShader(
                    includeURL,
                    visits: &visits,
                    definitions: &definitions,
                    order: &order
                )
            }
            guard let parsed = parameterDeclaration(in: line) else { continue }
            if definitions[parsed.name] == nil { order.append(parsed.name) }
            definitions[parsed.name] = parsed.definition
        }
    }

    private nonisolated static func parameterDeclaration(
        in line: String
    ) -> (name: String, definition: Definition)? {
        let pattern = #"^\s*#pragma\s+parameter\s+([A-Za-z_][A-Za-z0-9_]*)\s+\"([^\"]*)\"\s+([-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?)\s+([-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?)\s+([-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?)(?:\s+([-+]?(?:\d*\.?\d+)(?:[eE][-+]?\d+)?))?"#
        guard let expression = try? NSRegularExpression(pattern: pattern),
              let match = expression.firstMatch(
                in: line,
                range: NSRange(line.startIndex..., in: line)
              ), match.numberOfRanges >= 6,
              let nameRange = Range(match.range(at: 1), in: line),
              let descriptionRange = Range(match.range(at: 2), in: line),
              let initial = number(at: 3, match: match, in: line),
              let minimum = number(at: 4, match: match, in: line),
              let maximum = number(at: 5, match: match, in: line),
              maximum >= minimum else { return nil }
        let step = number(at: 6, match: match, in: line) ?? 0
        return (
            String(line[nameRange]),
            Definition(
                description: String(line[descriptionRange]),
                initial: initial,
                minimum: minimum,
                maximum: maximum,
                step: step
            )
        )
    }

    private nonisolated static func number(
        at index: Int,
        match: NSTextCheckingResult,
        in text: String
    ) -> Float? {
        guard index < match.numberOfRanges,
              match.range(at: index).location != NSNotFound,
              let range = Range(match.range(at: index), in: text),
              let value = Float(text[range]), value.isFinite else { return nil }
        return value
    }

    private nonisolated static func dictionary(
        from lines: [String]
    ) -> [String: String] {
        var result: [String: String] = [:]
        for rawLine in lines {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.hasPrefix("#"),
                  let separator = line.firstIndex(of: "=") else { continue }
            let key = line[..<separator]
                .trimmingCharacters(in: .whitespacesAndNewlines)
            let value = unquote(String(line[line.index(after: separator)...]))
            guard !key.isEmpty, !value.isEmpty else { continue }
            result[key] = value
        }
        return result
    }

    private nonisolated static func directivePath(
        _ directive: String,
        in line: String
    ) -> String? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix(directive) else { return nil }
        let tail = trimmed.dropFirst(directive.count)
            .trimmingCharacters(in: .whitespaces)
        let value = unquote(tail.hasPrefix("=")
            ? String(tail.dropFirst()) : tail)
        return value.isEmpty ? nil : value
    }

    private nonisolated static func unquote(_ source: String) -> String {
        var value = source.trimmingCharacters(in: .whitespacesAndNewlines)
        if let comment = value.firstIndex(of: "#") {
            value = String(value[..<comment])
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }
        if value.count >= 2, value.first == "\"", value.last == "\"" {
            value.removeFirst()
            value.removeLast()
        }
        return value
    }

    private nonisolated static func resolve(
        _ path: String,
        relativeTo source: URL
    ) -> URL? {
        guard !path.isEmpty else { return nil }
        let candidate = path.hasPrefix("/")
            ? URL(fileURLWithPath: path)
            : source.deletingLastPathComponent()
                .appendingPathComponent(path)
        let resolved = candidate.standardizedFileURL
        return FileManager.default.fileExists(atPath: resolved.path)
            ? resolved : nil
    }
}
