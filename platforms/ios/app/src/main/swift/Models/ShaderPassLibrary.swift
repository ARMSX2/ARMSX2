// ShaderPassLibrary.swift — inspect the active slang preset as named passes
// SPDX-License-Identifier: GPL-3.0+

import Foundation

struct ShaderPassDescriptor: Identifiable, Hashable, Sendable {
    struct Option: Identifiable, Hashable, Sendable {
        let key: String
        let value: String

        var id: String { key }
    }

    let id: String
    let name: String
    let sourceURL: URL
    let parameterNames: Set<String>
    let systemImage: String
    let options: [Option]

    var fileName: String { sourceURL.lastPathComponent }
}

struct ShaderWorkspaceLayerConfiguration: Identifiable, Hashable, Codable,
    Sendable {
    let id: String
    let token: String

    init(id: String = UUID().uuidString, token: String) {
        self.id = id
        self.token = token
    }
}

struct ShaderPresetLayerDescriptor: Identifiable, Hashable, Sendable {
    let id: String
    let token: String
    let name: String
    let passes: [ShaderPassDescriptor]
    let parameters: [ShaderParam]
    let presetAssignments: [ShaderPassDescriptor.Option]
}

/// A small, cancellable frontend inspector. It never compiles shaders and is
/// intentionally independent of librashader, so Settings can describe a chain
/// even when the optional renderer is absent from a development binary.
@MainActor
final class ShaderPassLibrary: ObservableObject {
    @Published private(set) var passes: [ShaderPassDescriptor] = []
    @Published private(set) var layers: [ShaderPresetLayerDescriptor] = []
    @Published private(set) var isLoading = false

    private var loadIdentity = ""

    func load(token newToken: String) async {
        let layer = ShaderWorkspaceLayerConfiguration(
            id: Self.stableLayerID(for: newToken),
            token: newToken
        )
        await load(layers: newToken.isEmpty ? [] : [layer])
    }

    func load(layers configurations: [ShaderWorkspaceLayerConfiguration]) async {
        let identity = configurations.map { "\($0.id):\($0.token)" }
            .joined(separator: "|")
        loadIdentity = identity
        guard !configurations.isEmpty else {
            layers = []
            passes = []
            isLoading = false
            return
        }

        isLoading = true
        let inspectedLayers: [ShaderPresetLayerDescriptor] = await Task.detached(
            priority: .userInitiated
        ) { () -> [ShaderPresetLayerDescriptor] in
            configurations.compactMap {
                (configuration: ShaderWorkspaceLayerConfiguration)
                    -> ShaderPresetLayerDescriptor? in
                guard let presetURL = ShaderPresetLibrary.resolve(
                    configuration.token
                ) else { return nil }
                let inspected = Self.inspect(presetURL: presetURL).map { pass in
                    ShaderPassDescriptor(
                        id: "\(configuration.id)|\(pass.id)",
                        name: pass.name,
                        sourceURL: pass.sourceURL,
                        parameterNames: pass.parameterNames,
                        systemImage: pass.systemImage,
                        options: pass.options
                    )
                }
                let parameters = (try? ShaderParams.parameters(at: presetURL))
                    ?? []
                return ShaderPresetLayerDescriptor(
                    id: configuration.id,
                    token: configuration.token,
                    name: Self.humanizedName(
                        presetURL.deletingPathExtension().lastPathComponent
                    ),
                    passes: inspected,
                    parameters: parameters,
                    presetAssignments: Self.presetAssignments(
                        presetURL: presetURL
                    )
                )
            }
        }.value
        guard !Task.isCancelled, loadIdentity == identity else { return }
        layers = inspectedLayers
        passes = inspectedLayers.flatMap { $0.passes }
        isLoading = false
    }

    nonisolated static func stableLayerID(for token: String) -> String {
        "shader-layer-\(ShaderWorkspacePresetEditor.stableHash(token))"
    }

    nonisolated static func inspect(
        presetURL: URL
    ) -> [ShaderPassDescriptor] {
        var visits = Set<String>()
        let preset = resolvedPreset(in: presetURL, visits: &visits)
        let count = Int(preset["shaders"]?.value ?? "")
        let indices = count.map { Array(0..<$0) }
            ?? preset.keys.compactMap(shaderIndex).sorted()
        return indices.compactMap { index in
            guard let shader = preset["shader\(index)"],
                  let url = resolve(shader.value, relativeTo: shader.sourceURL)
            else { return nil }
            var shaderVisits = Set<String>()
            let parameters = parameterNames(in: url, visits: &shaderVisits)
            let displayName = humanizedName(url.deletingPathExtension().lastPathComponent)
            let identity = "\(presetURL.standardizedFileURL.path)#\(index):\(url.path)"
            let options = passOptionPrefixes.compactMap { prefix -> ShaderPassDescriptor.Option? in
                guard let assignment = preset["\(prefix)\(index)"] else { return nil }
                return .init(key: prefix, value: assignment.value)
            }
            return ShaderPassDescriptor(
                id: identity,
                name: displayName,
                sourceURL: url,
                parameterNames: parameters,
                systemImage: ShaderFunctionIcon.systemImage(
                    for: displayName + " " + url.path
                ),
                options: options
            )
        }
    }

    /// Values outside the indexed pass table include authored parameter
    /// defaults and texture/LUT declarations. Keeping them in the generated
    /// workspace makes every layer independent of `#reference` merge order.
    nonisolated private static func presetAssignments(
        presetURL: URL
    ) -> [ShaderPassDescriptor.Option] {
        var visits = Set<String>()
        let preset = resolvedPreset(in: presetURL, visits: &visits)
        return preset.keys.sorted().compactMap { key in
            guard key != "shaders",
                  shaderIndex(key) == nil,
                  !isIndexedPassOption(key),
                  let assignment = preset[key] else { return nil }
            let value = absoluteResourceValue(
                assignment.value,
                sourceURL: assignment.sourceURL
            )
            return .init(key: key, value: value)
        }
    }

    nonisolated private static func isIndexedPassOption(_ key: String) -> Bool {
        passOptionPrefixes.contains { prefix in
            guard key.hasPrefix(prefix) else { return false }
            return Int(key.dropFirst(prefix.count)) != nil
        }
    }

    nonisolated private static func absoluteResourceValue(
        _ value: String,
        sourceURL: URL
    ) -> String {
        guard !value.hasPrefix("/"),
              !value.contains(";"),
              Double(value) == nil,
              value.lowercased() != "true",
              value.lowercased() != "false" else { return value }
        let candidate = sourceURL.deletingLastPathComponent()
            .appendingPathComponent(value).standardizedFileURL
        return FileManager.default.fileExists(atPath: candidate.path)
            ? candidate.path : value
    }

    private struct Assignment {
        let value: String
        let sourceURL: URL
    }

    private nonisolated static let passOptionPrefixes = [
        "filter_linear", "filter_linear_srgb", "wrap_mode", "frame_count_mod",
        "mipmap_input", "alias", "float_framebuffer", "srgb_framebuffer",
        "scale_type", "scale_type_x", "scale_type_y", "scale", "scale_x",
        "scale_y",
    ]

    /// Resolve inherited presets first, then replace matching assignments from
    /// the child. Every value retains the file it came from so relative shader
    /// paths still resolve after inheritance is flattened.
    nonisolated private static func resolvedPreset(
        in presetURL: URL,
        visits: inout Set<String>
    ) -> [String: Assignment] {
        let preset = presetURL.standardizedFileURL
        guard visits.insert(preset.path).inserted,
              let text = try? String(contentsOf: preset, encoding: .utf8)
        else { return [:] }

        let lines = text.components(separatedBy: .newlines)
        var result: [String: Assignment] = [:]
        for line in lines {
            guard let path = directivePath("#reference", in: line),
                  let reference = resolve(path, relativeTo: preset) else {
                continue
            }
            result.merge(
                resolvedPreset(in: reference, visits: &visits),
                uniquingKeysWith: { _, child in child }
            )
        }

        for (key, value) in assignments(from: lines) {
            result[key] = Assignment(value: value, sourceURL: preset)
        }
        return result
    }

    nonisolated private static func shaderIndex(_ key: String) -> Int? {
        guard key.hasPrefix("shader") else { return nil }
        return Int(key.dropFirst("shader".count))
    }

    nonisolated private static func parameterNames(
        in sourceURL: URL,
        visits: inout Set<String>
    ) -> Set<String> {
        let source = sourceURL.standardizedFileURL
        guard visits.insert(source.path).inserted,
              let text = try? String(contentsOf: source, encoding: .utf8)
        else { return [] }

        var names = Set<String>()
        for line in text.components(separatedBy: .newlines) {
            if let include = directivePath("#include", in: line),
               let includedURL = resolve(include, relativeTo: source) {
                names.formUnion(parameterNames(in: includedURL, visits: &visits))
            }
            guard let name = pragmaParameterName(in: line) else { continue }
            names.insert(name)
        }
        return names
    }

    nonisolated private static func pragmaParameterName(
        in line: String
    ) -> String? {
        let pattern = #"^\s*#pragma\s+parameter\s+([A-Za-z_][A-Za-z0-9_]*)\b"#
        guard let expression = try? NSRegularExpression(pattern: pattern),
              let match = expression.firstMatch(
                in: line,
                range: NSRange(line.startIndex..., in: line)
              ),
              let range = Range(match.range(at: 1), in: line) else {
            return nil
        }
        return String(line[range])
    }

    nonisolated private static func assignments(
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

    nonisolated private static func directivePath(
        _ directive: String,
        in line: String
    ) -> String? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix(directive) else { return nil }
        let tail = trimmed.dropFirst(directive.count)
            .trimmingCharacters(in: .whitespaces)
        let value = unquote(
            tail.hasPrefix("=") ? String(tail.dropFirst()) : tail
        )
        return value.isEmpty ? nil : value
    }

    nonisolated private static func unquote(_ source: String) -> String {
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

    nonisolated private static func resolve(
        _ path: String,
        relativeTo source: URL
    ) -> URL? {
        guard !path.isEmpty else { return nil }
        let candidate = path.hasPrefix("/")
            ? URL(fileURLWithPath: path)
            : source.deletingLastPathComponent().appendingPathComponent(path)
        let resolved = candidate.standardizedFileURL
        return FileManager.default.fileExists(atPath: resolved.path)
            ? resolved : nil
    }

    nonisolated static func humanizedName(_ raw: String) -> String {
        raw.replacingOccurrences(of: "_", with: " ")
            .replacingOccurrences(of: "-", with: " ")
            .split(separator: " ")
            .map { word in
                let text = String(word)
                let upper = text.uppercased()
                if ["CRT", "LCD", "HDR", "NTSC", "PAL", "VHS", "CAS", "FSR"]
                    .contains(upper) {
                    return upper
                }
                return text.prefix(1).uppercased() + text.dropFirst()
            }
            .joined(separator: " ")
    }
}

/// Keyword families are intentionally broad: downloaded shader packs use
/// inconsistent names, while their functional vocabulary is very stable.
enum ShaderFunctionIcon {
    nonisolated private static let families: [([String], String)] = [
        (["beam", "laser", "light-ray", "light ray"], "light.beacon.max"),
        (["mask", "masking"], "circle.lefthalf.filled"),
        (["curve", "curvature", "warp", "fisheye", "barrel"], "move.3d"),
        (["size", "resize", "scale", "zoom"], "arrow.up.left.and.arrow.down.right"),
        (["colour", "color", "palette", "tint", "hue"], "paintpalette"),
        (["hdr", "high dynamic range", "tonemap"], "sun.max"),
        (["bezel", "border", "surround"], "rectangle.inset.filled"),
        (["limiter", "clamp", "threshold", "clip"], "gauge"),
        (["display", "screen", "monitor"], "display"),
        (["digital", "integer", "quantize"], "number"),
        (["analog", "analogue", "composite", "s-video", "rf"], "waveform"),
        (["frame", "cadence"], "rectangle.on.rectangle"),
        (["interpolation", "bicubic", "bilinear", "spline"], "point.topleft.down.to.point.bottomright.curvepath"),
        (["gaussian"], "camera.filters"),
        (["anamorphic", "widescreen"], "rectangle.expand.vertical"),
        (["aperture", "grille", "grill"], "square.grid.3x3"),
        (["scanline", "scan-line", "scan line"], "line.3.horizontal"),
        (["shadow", "shadow-mask"], "circle.grid.cross"),
        (["bloom", "glow"], "sun.max.fill"),
        (["halation"], "sun.haze"),
        (["phosphor"], "sparkles"),
        (["blur", "soften", "smooth"], "drop"),
        (["sharp", "sharpen", "deblur", "detail", "cas"], "scope"),
        (["dither", "posterize", "posterise"], "circle.grid.cross"),
        (["noise", "grain", "static"], "aqi.medium"),
        (["film", "vhs", "tape"], "film"),
        (["ntsc"], "tv"),
        (["pal"], "tv.and.hifispeaker.fill"),
        (["gamma"], "slider.horizontal.3"),
        (["contrast"], "circle.righthalf.filled"),
        (["brightness", "luminance", "luma"], "sun.min"),
        (["saturation", "vibrance"], "paintbrush.pointed"),
        (["temperature", "white balance", "white-balance"], "thermometer.medium"),
        (["black level", "black-level", "black point"], "circle.fill"),
        (["white level", "white-level", "white point"], "circle"),
        (["deconvergence", "convergence", "chromatic", "aberration"], "camera.filters"),
        (["vignette"], "viewfinder.circle"),
        (["reflection", "reflect"], "rectangle.on.rectangle.angled"),
        (["ambient", "room-light", "room light"], "lightbulb"),
        (["motion", "velocity"], "wind"),
        (["ghost", "trail"], "square.stack.3d.up"),
        (["persistence", "afterimage", "after-image"], "clock.arrow.2.circlepath"),
        (["deinterlace", "bob", "weave"], "arrow.up.and.down.text.horizontal"),
        (["anti-alias", "antialias", "fxaa", "smaa"], "scribble.variable"),
        (["upscale", "super-resolution", "super resolution", "fsr", "xbr", "hq2", "hq3", "hq4"], "arrow.up.left.and.arrow.down.right"),
        (["downscale", "downsample"], "arrow.down.right.and.arrow.up.left"),
        (["rotate", "rotation"], "rotate.right"),
        (["crop", "cropping", "overscan"], "crop"),
        (["aspect", "ratio"], "aspectratio"),
        (["pixel", "pixellate", "pixelate"], "square.grid.3x3.fill"),
        (["lcd", "liquid crystal"], "rectangle.grid.3x2"),
        (["crt", "cathode", "tube"], "tv"),
        (["handheld", "gameboy", "portable"], "gamecontroller"),
        (["mesh", "grid", "lattice"], "grid"),
        (["texture", "paper", "canvas"], "square.3.layers.3d"),
        (["alpha", "opacity", "transparency"], "circle.dotted"),
        (["depth", "z-buffer", "zbuffer"], "square.3.layers.3d.down.right"),
        (["channel", "rgb", "bgr"], "circle.hexagongrid"),
        (["monochrome", "grayscale", "greyscale"], "circle.lefthalf.filled"),
        (["sepia", "warm"], "camera.filters"),
        (["emboss", "relief"], "mountain.2"),
        (["edge", "sobel", "outline"], "square.dashed"),
        (["cartoon", "cel-shade", "cel shade"], "wand.and.stars"),
        (["water", "ripple", "wave"], "water.waves"),
        (["time", "animation", "animated"], "clock.arrow.circlepath"),
        (["vertical", "height", "y-axis", "y axis"], "arrow.up.and.down"),
        (["horizontal", "width", "x-axis", "x axis"], "arrow.left.and.right"),
        (["offset", "position", "translate"], "arrow.up.and.down.and.arrow.left.and.right"),
        (["mipmap", "mip-map"], "square.stack.3d.up.fill"),
        (["wrap", "repeat", "mirror"], "repeat"),
        (["filter", "linear", "nearest"], "line.3.horizontal.decrease.circle"),
        (["jitter", "shake"], "waveform.path"),
        (["flicker", "rolling"], "bolt"),
        (["subpixel", "sub-pixel"], "square.grid.4x3.fill"),
        (["moire", "moire-pattern"], "circle.grid.2x2"),
    ]

    nonisolated static func systemImage(for source: String) -> String {
        let name = source.lowercased()
        return families.first { words, _ in words.contains { name.contains($0) } }?.1
            ?? "camera.filters"
    }
}

struct ShaderWorkspacePresetConfiguration: Sendable {
    let layers: [ShaderWorkspaceLayerConfiguration]
    let hiddenLayerIDs: Set<String>
}

/// Writes a small managed child preset instead of mutating downloaded or
/// bundled files. The child references the original, then replaces the ordered
/// pass table with only visible passes while preserving every indexed option.
enum ShaderWorkspacePresetEditor {
    private static let baseMarker = "# ARMSX2_WORKSPACE_BASE "
    private static let layersMarker = "# ARMSX2_WORKSPACE_LAYERS "
    private static let hiddenLayersMarker =
        "# ARMSX2_WORKSPACE_HIDDEN_LAYERS "

    nonisolated static func configuration(
        for token: String
    ) -> ShaderWorkspacePresetConfiguration {
        guard let url = ShaderPresetLibrary.resolve(token) else {
            return .init(layers: [], hiddenLayerIDs: [])
        }
        guard let text = try? String(contentsOf: url, encoding: .utf8) else {
            return singleLayerConfiguration(token)
        }

        if let layers: [ShaderWorkspaceLayerConfiguration] = decode(
            marker(layersMarker, in: text)
        ) {
            let valid = layers.filter {
                ShaderPresetLibrary.resolve($0.token) != nil
            }
            if !valid.isEmpty {
                let validIDs = Set(valid.map(\.id))
                let hidden: [String] = decode(
                    marker(hiddenLayersMarker, in: text)
                ) ?? []
                return .init(
                    layers: valid,
                    hiddenLayerIDs: Set(hidden).intersection(validIDs)
                )
            }
        }

        // Migrate the first workspace format, which stored one base preset and
        // pass-level ordering, into a single visible preset layer.
        if let encodedBase = marker(baseMarker, in: text),
           let baseData = Data(base64Encoded: encodedBase),
           let baseToken = String(data: baseData, encoding: .utf8),
           ShaderPresetLibrary.resolve(baseToken) != nil {
            return singleLayerConfiguration(baseToken)
        }
        return singleLayerConfiguration(token)
    }

    nonisolated static func write(
        layers: [ShaderWorkspaceLayerConfiguration],
        descriptors: [ShaderPresetLayerDescriptor],
        hiddenLayerIDs: Set<String>
    ) throws -> String? {
        guard !layers.isEmpty,
              ShaderPresetLibrary.prepareUserRoots() != nil,
              let root = ShaderPresetLibrary.savedPresetRoot else {
            return nil
        }

        let descriptorByID = Dictionary(
            uniqueKeysWithValues: descriptors.map { ($0.id, $0) }
        )
        let resolvedLayers = layers.compactMap { layer -> (URL, ShaderPresetLayerDescriptor)? in
            guard let url = ShaderPresetLibrary.resolve(layer.token),
                  let descriptor = descriptorByID[layer.id] else { return nil }
            return (url, descriptor)
        }
        guard !resolvedLayers.isEmpty else { return nil }

        let visibleLayers = resolvedLayers.filter { _, descriptor in
            !hiddenLayerIDs.contains(descriptor.id)
        }
        // The workspace presents layers like a graphics editor: the first row
        // is visually on top. Shader passes execute in forward order, so emit
        // the bottom layer first and the top layer last. This makes moving a
        // blur above scan-lines actually blur the composed scan-lines instead
        // of applying the blur underneath them.
        let compositingLayers = Array(visibleLayers.reversed())
        let visiblePasses = compositingLayers.flatMap { _, descriptor in
            descriptor.passes
        }
        var lines = [
            layersMarker + encode(layers),
            hiddenLayersMarker + encode(Array(hiddenLayerIDs).sorted()),
            "",
            "shaders = \(visiblePasses.count)",
        ]

        for (index, pass) in visiblePasses.enumerated() {
            lines.append(
                "shader\(index) = \(presetLiteral(pass.sourceURL.path))"
            )
            for option in pass.options {
                lines.append(
                    "\(option.key)\(index) = \(presetLiteral(option.value))"
                )
            }
        }

        var assignmentOrder: [String] = []
        var mergedAssignments: [String: String] = [:]
        // Merge layer-level assignments in the same bottom-to-top order as
        // the pass table. When two layers define the same global assignment,
        // the visually top layer therefore owns the final value.
        for (_, layer) in compositingLayers {
            for assignment in layer.presetAssignments {
                if assignment.key == "parameters"
                    || assignment.key == "textures" {
                    let oldValues = mergedAssignments[assignment.key]?
                        .split(separator: ";").map(String.init) ?? []
                    let newValues = assignment.value
                        .split(separator: ";").map(String.init)
                    let combined = oldValues + newValues.filter {
                        !oldValues.contains($0)
                    }
                    if mergedAssignments[assignment.key] == nil {
                        assignmentOrder.append(assignment.key)
                    }
                    mergedAssignments[assignment.key] = combined
                        .joined(separator: ";")
                } else {
                    if mergedAssignments[assignment.key] == nil {
                        assignmentOrder.append(assignment.key)
                    }
                    mergedAssignments[assignment.key] = assignment.value
                }
            }
        }
        if !assignmentOrder.isEmpty { lines.append("") }
        for key in assignmentOrder {
            guard let value = mergedAssignments[key] else { continue }
            lines.append("\(key) = \(presetLiteral(value))")
        }
        lines.append("")

        let stem = resolvedLayers.first!.0
            .deletingPathExtension().lastPathComponent
            .replacingOccurrences(
                of: #"[^A-Za-z0-9_-]+"#,
                with: "-",
                options: .regularExpression
            )
        // The renderer caches a compiled chain by preset path. Visibility used
        // to rewrite the same file, so the eye button changed the UI metadata
        // while the cached chain kept rendering the hidden layer. Include all
        // render-affecting workspace state in the generated path; selecting
        // that new token forces a clean runtime chain without mutating source
        // presets. Layer order is already represented by `layers`.
        let hiddenIdentity = hiddenLayerIDs.sorted().joined(separator: ",")
        let identity = layers.map { "\($0.id):\($0.token)" }
            .joined(separator: "|") + "|hidden:\(hiddenIdentity)"
        let hash = stableHash(identity)
        let output = root.appendingPathComponent(
            "\(stem)-ARMSX2-Workspace-\(hash)"
        ).appendingPathExtension(ShaderPresetLibrary.presetExtension)
        try lines.joined(separator: "\n")
            .write(to: output, atomically: true, encoding: .utf8)
        return ShaderPresetLibrary.token(for: output)
    }

    nonisolated private static func singleLayerConfiguration(
        _ token: String
    ) -> ShaderWorkspacePresetConfiguration {
        guard !token.isEmpty,
              ShaderPresetLibrary.resolve(token) != nil else {
            return .init(layers: [], hiddenLayerIDs: [])
        }
        return .init(
            layers: [
                .init(
                    id: ShaderPassLibrary.stableLayerID(for: token),
                    token: token
                )
            ],
            hiddenLayerIDs: []
        )
    }

    nonisolated private static func marker(
        _ prefix: String,
        in text: String
    ) -> String? {
        text.components(separatedBy: .newlines)
            .first { $0.hasPrefix(prefix) }
            .map { String($0.dropFirst(prefix.count)) }
    }

    nonisolated private static func encode<T: Encodable>(
        _ value: T
    ) -> String {
        guard let data = try? JSONEncoder().encode(value) else { return "" }
        return data.base64EncodedString()
    }

    nonisolated private static func decode<T: Decodable>(
        _ value: String?
    ) -> T? {
        guard let value,
              let data = Data(base64Encoded: value),
              let decoded = try? JSONDecoder().decode(T.self, from: data)
        else { return nil }
        return decoded
    }

    nonisolated private static func presetLiteral(_ value: String) -> String {
        let lowercased = value.lowercased()
        if Double(value) != nil || lowercased == "true" || lowercased == "false" {
            return value
        }
        let escaped = value
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }

    nonisolated static func stableHash(_ value: String) -> String {
        var hash: UInt64 = 14_695_981_039_346_656_037
        for byte in value.utf8 {
            hash ^= UInt64(byte)
            hash &*= 1_099_511_628_211
        }
        return String(hash, radix: 16)
    }
}
