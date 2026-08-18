// ShaderParams.swift — a preset's own numbers, tweaked, applied and saved
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ShaderParamsError: LocalizedError {
    case noName
    case noSavedRoot

    var errorDescription: String? {
        switch self {
        case .noName:
            return "That name has nothing in it that can become a filename."
        case .noSavedRoot:
            return "The Documents shader folder could not be opened."
        }
    }
}

/// One tweakable value a `.slangp` declares. Every number here is the shader author's, so
/// every number is treated as hostile: absent, non-finite, inverted and zero-step all occur.
struct ShaderParam: Identifiable, Hashable, Sendable {
    let name: String
    let description: String
    let initial: Float
    let minimum: Float
    let maximum: Float
    let step: Float

    var id: String { name }

    /// Whether the author left any room to move, and the only thing inferred about a
    /// parameter's role. Do not "improve" this by reading the name or the description: stock
    /// packs name real sliders like headings, so any such rule hides a working control.
    var isAdjustable: Bool { maximum > minimum }

    /// What a control actually moves by. A zero, negative or wider-than-the-range step is
    /// ordinary rather than broken, and RetroArch reads one as "no increment declared" too.
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

/// A preset's parameters, the user's overrides on them, and the two directions those travel:
/// down to the running chain, and out to a saved preset of the user's own.
@MainActor
final class ShaderParams: ObservableObject {
    @Published private(set) var params: [ShaderParam] = []
    @Published private(set) var overrides: [String: Float] = [:]
    @Published private(set) var isLoading = false
    @Published private(set) var savedName: String?
    @Published private(set) var errorText: String?

    nonisolated static let section = "EmuCore/GS"
    nonisolated static let key = "ShaderChainParams"
    nonisolated static let invariant = Locale(identifier: "en_US_POSIX")

    private var token = ""

    var hasOverrides: Bool { !overrides.isEmpty }

    func load(token newToken: String) async {
        token = newToken
        savedName = nil
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

    func save(as name: String) async {
        savedName = nil
        errorText = nil
        let safe = Self.safeName(name)
        guard !safe.isEmpty, let base = ShaderPresetLibrary.resolve(token) else {
            errorText = ShaderParamsError.noName.errorDescription
            return
        }
        let text = Self.presetText(base: base, params: params, overrides: overrides)
        do {
            let url = try await Task.detached(priority: .userInitiated) {
                try Self.write(text, named: safe)
            }.value
            savedName = url.deletingPathExtension().lastPathComponent
        } catch {
            errorText = error.localizedDescription
        }
    }

    /// Sends the effective value of EVERY parameter, not just the changed ones. librashader
    /// has no unset call, so a name dropped from the map leaves the chain on whatever was
    /// pushed last, and a reset would never take.
    private func pushEffective() {
        guard !params.isEmpty, let url = ShaderPresetLibrary.resolve(token) else { return }
        var effective: [String: NSNumber] = [:]
        effective.reserveCapacity(params.count)
        for param in params {
            effective[param.name] = NSNumber(value: value(for: param))
        }
        ARMSX2Bridge.setShaderChainParameters(effective, forPreset: url.path)
    }

    /// The saved overrides for a preset, pushed without a `ShaderParams` to push them: every
    /// other push happens because a shader screen is open, and a cold launch opens none. Only
    /// the overrides go down, because a chain built from the preset file already holds that
    /// preset's number for every name it is not told about, and reading the file here to say
    /// so again would put a librashader parse on the launch path.
    static func pushStored(token: String) {
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
        guard let data = try? JSONEncoder().encode(store),
              let json = String(data: data, encoding: .utf8) else { return }
        ARMSX2Bridge.setINIString(Self.section, key: Self.key, value: json)
    }

    private static func stored() -> [String: [String: Float]] {
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

    /// A RetroArch simple preset: a reference to the base plus the changed values. Reading one
    /// back reports those values as each parameter's initial, so Reset on a saved preset means
    /// "back to what I saved" rather than back to the base's own numbers.
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

    /// Relative while the base sits in the same Documents root, so the pair survives the
    /// container UUID changing. A bundled base has no such route and gets a path that a
    /// reinstall breaks, which is why the save sheet says the reference is a path.
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

    private nonisolated static func write(_ text: String, named name: String) throws -> URL {
        guard ShaderPresetLibrary.prepareUserRoots() != nil,
              let root = ShaderPresetLibrary.savedPresetRoot?.standardizedFileURL else {
            throw ShaderParamsError.noSavedRoot
        }
        let url = root.appendingPathComponent(name)
            .appendingPathExtension(ShaderPresetLibrary.presetExtension).standardizedFileURL
        guard url.deletingLastPathComponent().path == root.path else {
            throw ShaderParamsError.noName
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
