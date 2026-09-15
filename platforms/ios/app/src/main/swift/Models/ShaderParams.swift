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

    nonisolated static let section = "EmuCore/GS"
    nonisolated static let key = "ShaderChainParams"
    nonisolated static let invariant = Locale(identifier: "en_US_POSIX")

    private var token = ""

    var hasOverrides: Bool { !overrides.isEmpty }

    func load(token newToken: String) async {
        token = newToken
        errorText = nil
        guard let url = ShaderPresetLibrary.resolve(newToken) else {
            params = []
            overrides = [:]
            return
        }
        overrides = Self.stored()[newToken] ?? [:]
        isLoading = true
        params = await Task.detached(priority: .userInitiated) { Self.read(at: url) }.value
        isLoading = false
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

    private nonisolated static func read(at url: URL) -> [ShaderParam] {
        guard let json = ARMSX2Bridge.shaderPresetParameters(atPath: url.path),
              let data = json.data(using: .utf8),
              let decoded = try? JSONDecoder().decode([ShaderParam].self, from: data) else {
            return []
        }
        return decoded.filter { !$0.name.isEmpty }
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
