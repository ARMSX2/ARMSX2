// ReportRedaction.swift — ordered rewrite rules that keep paths, account names and device ids out of a report
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ReportRedaction {
    struct Context {
        let roots: [String]
        let bundleID: String
        let userNames: [String]
        fileprivate let userRegex: NSRegularExpression?
        fileprivate let bundleRegex: NSRegularExpression?

        // False when names exist but cannot be matched, so the caller must not ship text redacted with this context.
        var namesCovered: Bool { userNames.isEmpty || userRegex != nil }

        // Shorter names would hit ordinary words, and longer ones are not account names.
        init(roots: [String], bundleID: String, userNames: [String]) {
            var spellings = Set<String>()
            for root in roots {
                var path = root
                while path.count > 1, path.hasSuffix("/") { path.removeLast() }
                guard path.count > 1 else { continue }
                spellings.insert(path)
                if path.hasPrefix("/private/var/") {
                    spellings.insert(String(path.dropFirst(8)))
                } else if path.hasPrefix("/var/") {
                    spellings.insert("/private" + path)
                }
            }
            // A root inside another one (tmp under home) is left to the outer root, which keeps its subfolder name.
            self.roots = spellings.filter { inner in !spellings.contains { inner.hasPrefix($0 + "/") } }
                .sorted { $0.count > $1.count }
            self.bundleID = bundleID
            var names = Set(userNames.map { $0.trimmingCharacters(in: .whitespaces) }.filter { nameLengths.contains($0.count) })
            // Past the cap no regex is built, so a flood never reaches compilation.
            if names.count <= nameCap {
                names = names.filter { name in
                    placeholderText.range(of: wholeWord(NSRegularExpression.escapedPattern(for: name)),
                                          options: [.regularExpression, .caseInsensitive]) == nil
                }
            }
            self.userNames = names.sorted()
            userRegex = names.isEmpty || names.count > nameCap ? nil : try? NSRegularExpression(
                pattern: wholeWord(self.userNames.map(NSRegularExpression.escapedPattern(for:)).joined(separator: "|")),
                options: .caseInsensitive)
            bundleRegex = bundleID.isEmpty ? nil : try? NSRegularExpression(
                pattern: NSRegularExpression.escapedPattern(for: bundleID) + #"(?:\.[A-Z0-9]{10})?"#)
        }

        // Reads a small INI, so call it off the main actor.
        static func live(documents: URL) -> Context {
            var names: [String] = []
            for file in ["PCSX2-iOS.ini", "ARMSX2-iOS.ini"] {
                if let text = try? String(contentsOf: documents.appendingPathComponent(file), encoding: .utf8),
                   let value = iniValue(in: text, section: "Achievements", key: "Username"), !value.isEmpty {
                    names = [value]
                    break
                }
            }
            return Context(roots: [NSHomeDirectory(), Bundle.main.bundlePath, NSTemporaryDirectory()],
                           bundleID: Bundle.main.bundleIdentifier ?? "", userNames: names)
        }

        func adding(userNames extra: [String]) -> Context {
            Context(roots: roots, bundleID: bundleID, userNames: userNames + extra)
        }
    }

    fileprivate static let nameLengths = 3...64
    fileprivate static let nameCap = 64

    // The texts the rules write: a name matching inside one would corrupt it and trip the wire on every build.
    fileprivate static let placeholderText = "<container> <external> <icloud> <simruntime> <user> <udid> <redacted> <bundle-id> <ra-user> "
        + "<user-agent> /user_<ra-user>.png /UserPic/<ra-user>.png"

    // Lookarounds, not \b, so a name that starts or ends on a non-word character still matches.
    fileprivate static func wholeWord(_ body: String) -> String { "(?<!\\w)(?:" + body + ")(?!\\w)" }

    private static let secretKeys: Set<String> = ["Username", "Token", "LoginTimestamp"]
    private static let tripwireNeedles = ["ARMSX2-iOS/v", "/var/mobile", "/Containers/", "PCSX2-iOS-secrets", "CoreSimulator"]

    private static let uuid = "[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"
    private static let externalPrefix =
        #"(?:(?:/private)?/var/mobile/Containers/Shared/AppGroup/"# + uuid + #"/File Provider Storage"#
        + #"|(?:/private)?/var/mobile/Library/LiveFiles|/Volumes/[^/\n"']+)"#
    private static let icloudPrefix = #"(?:/[^\s"'=/]+)*/Mobile Documents/com~apple~CloudDocs"#
    private static let fileTail =
        #"(?:/[^/\n"']+)*?/([^/\n"']+?\.(?:iso|chd|cso|zso|bin|cue|elf|gz|img|mdf|nrg))(?![A-Za-z0-9])"#
    private static let bareTail = #"(?:/[^"'\n]*)?"#

    private static let externalFile = try! NSRegularExpression(pattern: externalPrefix + fileTail, options: .caseInsensitive)
    private static let icloudFile = try! NSRegularExpression(pattern: icloudPrefix + fileTail, options: .caseInsensitive)
    private static let externalBare = try! NSRegularExpression(pattern: externalPrefix + bareTail, options: .caseInsensitive)
    private static let icloudBare = try! NSRegularExpression(pattern: icloudPrefix + bareTail, options: .caseInsensitive)
    private static let leftoverContainer = try! NSRegularExpression(
        pattern: #"(?:(?:/private)?/var/(?:mobile/)?|/Users/[^/\s"']+/Library/Developer/CoreSimulator/Devices/"# + uuid
            + #"/data/)[Cc]ontainers/(?:[A-Za-z]+/){1,2}"# + uuid)
    private static let nestedContainer = try! NSRegularExpression(
        pattern: #"<container>/Documents/(?:(?:LiveContainer/)?Data/Application/"# + uuid + #"|Applications/[^/\n"']+?\.app)"#)
    private static let simulatorRuntime = try! NSRegularExpression(
        pattern: #"[^\s"']*/Library/Developer/CoreSimulator/Profiles/Runtimes/[^/\n"']+\.simruntime/Contents/Resources/RuntimeRoot"#)
    private static let buildMachineUser = try! NSRegularExpression(pattern: #"/Users/(?!<)[^/\s"'<]+"#)
    private static let udid = try! NSRegularExpression(pattern: #"(?<![0-9A-Fa-f-])[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}(?![0-9A-Fa-f-])"#)
    private static let identifierKeys = try! NSRegularExpression(
        pattern: #"("(?:crashReporterKey|deviceIdentifierForVendor|codeSigningTeamID|bootSessionUUID)"\s*:\s*)"[^"]*""#)
    // Each pattern captures the text before the name, the name, and the text after it.
    private static let nameShapes: [(needle: String, regex: NSRegularExpression)] = [
        ("login with user '", #"(login with user ')([^'\n]+)(')"#),
        ("Attempting to log in ", #"(Attempting to log in )(.+?)( \(with (?:password|token)\))"#),
        // rcheevos prints the display name, which can differ from the INI username by more than case.
        (" logged in successfully", #"(Achievements: )(.+?)( logged in successfully)"#),
        ("Achievements: Logging ", #"(Achievements: Logging )(.+?)( out\b)"#),
    ].map { ($0.0, try! NSRegularExpression(pattern: $0.1)) }
    // `_` is a word character, so the name regex cannot match inside user_<name>.png.
    private static let imageShape = try! NSRegularExpression(pattern: #"(/(?:user_|UserPic/))([^/\s'"]+)(\.png)"#)
    static let loginNeedles = nameShapes.map(\.needle)
    private static let userAgent = try! NSRegularExpression(pattern: #"ARMSX2-iOS/v[^\s"]*"#)

    // Every rule works on single lines, so a caller may cut its input at newlines.
    static func redact(_ text: String, context: Context) -> String {
        var result = autoreleasepool { text.replacingOccurrences(of: "\\/", with: "/") }
        for root in context.roots {
            autoreleasepool { result = result.replacingOccurrences(of: root, with: "<container>") }
        }
        apply(externalFile, "<external>/$1", to: &result)
        apply(icloudFile, "<icloud>/$1", to: &result)
        apply(externalBare, "<external>", to: &result)
        apply(icloudBare, "<icloud>", to: &result)
        apply(leftoverContainer, "<container>", to: &result)
        apply(nestedContainer, "<container>", to: &result)
        apply(simulatorRuntime, "<simruntime>", to: &result)
        apply(buildMachineUser, "/Users/<user>", to: &result)
        apply(udid, "<udid>", to: &result)
        apply(identifierKeys, "$1\"<redacted>\"", to: &result)
        if let regex = context.bundleRegex { apply(regex, "<bundle-id>", to: &result) }
        for shape in nameShapes { apply(shape.regex, "$1<ra-user>$3", to: &result) }
        apply(imageShape, "$1<ra-user>$3", to: &result)
        if let regex = context.userRegex { apply(regex, "<ra-user>", to: &result) }
        apply(userAgent, "<user-agent>", to: &result)
        return result
    }

    static func redactINI(_ text: String, context: Context) -> String {
        var section = ""
        let lines = text.components(separatedBy: "\n").map { line -> String in
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("["), trimmed.hasSuffix("]") {
                section = String(trimmed.dropFirst().dropLast())
            } else if section == "Achievements", let equals = line.firstIndex(of: "="),
                      secretKeys.contains(line[..<equals].trimmingCharacters(in: .whitespaces)) {
                return line[...equals] + " <redacted>"
            }
            return line
        }
        return redact(lines.joined(separator: "\n"), context: context)
    }

    static func loginNames(in line: String) -> [String] {
        nameShapes.flatMap { shape in
            shape.regex.matches(in: line, range: NSRange(line.startIndex..., in: line)).compactMap {
                Range($0.range(at: 2), in: line).map { String(line[$0]) }
            }
        }
    }

    // Labels only: a hit never carries the matched text into a message.
    static func tripwireHits(in text: String, context: Context) -> [String] {
        var hits = tripwireNeedles.filter { text.contains($0) }
        if let regex = context.userRegex, regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil {
            hits.append("<ra-user>")
        }
        return hits
    }

    static func iniValue(in text: String, section: String, key: String) -> String? {
        var current = ""
        for line in text.components(separatedBy: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("["), trimmed.hasSuffix("]") {
                current = String(trimmed.dropFirst().dropLast())
            } else if current == section, let equals = trimmed.firstIndex(of: "="),
                      trimmed[..<equals].trimmingCharacters(in: .whitespaces) == key {
                return trimmed[trimmed.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }

    // A pool per pass, since the bridged NSString copies of a large chunk pile up otherwise.
    private static func apply(_ regex: NSRegularExpression, _ template: String, to text: inout String) {
        autoreleasepool {
            text = regex.stringByReplacingMatches(
                in: text, range: NSRange(location: 0, length: text.utf16.count), withTemplate: template)
        }
    }
}
