// PatchStore+Download.swift — patch database download
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension PatchStore {
    // MARK: - Database download

    func downloadFromDatabase(forISO iso: String, asCheat: Bool) async {
        let requestGeneration = presentationGeneration
        let crc = iso == isoName ? currentCRC : Self.formattedCRC(ARMSX2Bridge.gameSettings(forISO: iso)["crc"] as? String)
        guard !crc.isEmpty else {
            applyFeedback(identityState.guidance ?? "Database matching is unavailable for this game.", kind: .information)
            return
        }

        let templates = (asCheat ? cheatDatabaseURLTemplates : patchDatabaseURLTemplates)
            .filter { Self.isConfiguredDatabaseTemplate($0) }
        guard !templates.isEmpty else {
            let kind = asCheat ? "cheat" : "patch"
            applyFeedback("No \(kind) database source is configured. Add a trusted URL in Advanced.", kind: .information)
            return
        }

        let serial = iso == isoName ? currentSerial : PadLayoutGameIdentity.normalizedSerial(ARMSX2Bridge.gameSettings(forISO: iso)["serial"] as? String)
        let title = iso == isoName ? currentTitle : (ARMSX2Bridge.gameMetadata(forISO: iso)["title"] ?? iso)

        isDownloading = true
        defer {
            if requestGeneration == presentationGeneration {
                isDownloading = false
            }
        }

        var succeededSources: [String] = []
        var notFoundCount = 0
        var firstError: String?

        for template in templates {
            guard !Task.isCancelled, requestGeneration == presentationGeneration else { return }
            guard let url = resolvedDatabaseURL(template: template, serial: serial, crc: crc, title: title) else {
                continue
            }
            let sourceName = PatchSource.databaseName(forTemplate: template)

            do {
                var request = URLRequest(url: url)
                request.timeoutInterval = 10
                request.cachePolicy = .reloadIgnoringLocalCacheData
                let (data, response) = try await URLSession.shared.data(for: request)
                guard !Task.isCancelled, requestGeneration == presentationGeneration else { return }
                guard let http = response as? HTTPURLResponse else {
                    if firstError == nil { firstError = "Could not download from \(sourceName)." }
                    continue
                }
                if http.statusCode == 404 {
                    notFoundCount += 1
                    continue
                }
                guard http.statusCode == 200 else {
                    if firstError == nil { firstError = "\(sourceName) returned status \(http.statusCode)." }
                    continue
                }
                guard data.count <= Self.maxPatchDownloadBytes else {
                    if firstError == nil { firstError = "The file from \(sourceName) is too large to be a valid patch." }
                    continue
                }
                guard let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .ascii) else {
                    if firstError == nil { firstError = "The file from \(sourceName) was not a valid patch." }
                    continue
                }
                guard case .valid = PnachParser.parse(text) else {
                    if firstError == nil { firstError = "The file from \(sourceName) was not a valid patch." }
                    continue
                }
                guard !Task.isCancelled, requestGeneration == presentationGeneration else { return }
                let outcome = writePatch(
                    text: text,
                    forISO: iso,
                    asCheat: asCheat,
                    autoEnable: false,
                    source: .database,
                    sourceDetail: sourceName
                )
                if outcome.isSuccess {
                    succeededSources.append(sourceName)
                } else {
                    if firstError == nil { firstError = "\(sourceName): \(outcome.message)" }
                }
            } catch {
                guard !Task.isCancelled, requestGeneration == presentationGeneration else { return }
                if firstError == nil { firstError = "Could not reach \(sourceName). Check your connection or URL." }
            }
        }

        guard !Task.isCancelled, requestGeneration == presentationGeneration else { return }

        let kindWord = asCheat ? "Cheat" : "Patch"
        if !succeededSources.isEmpty {
            // Refresh so newly installed entries show up without reopening the manager.
            loadInstalled(forISO: iso, launchContext: launchContext)
            let list = succeededSources.joined(separator: ", ")
            applyFeedback("\(kindWord) downloaded from \(list). Enable the entries you want.", kind: .success)
        } else if notFoundCount == templates.count {
            applyFeedback(
                asCheat
                    ? "No cheat found in any configured database."
                    : "No patch found in any configured database.",
                kind: .information
            )
        } else if let firstError {
            applyFeedback(firstError, kind: .error)
        } else {
            applyFeedback("Could not download from any configured database.", kind: .error)
        }
    }

    private func resolvedDatabaseURL(template: String, serial: String, crc: String, title: String) -> URL? {
        guard Self.isConfiguredDatabaseTemplate(template) else { return nil }
        if template.contains("${serial}") && serial.isEmpty { return nil }
        if template.contains("${crc}") && crc.isEmpty { return nil }
        if template.contains("${title}") && title.isEmpty { return nil }

        let filled = template
            .replacingOccurrences(of: "${serial}", with: urlComponent(serial))
            .replacingOccurrences(of: "${crc}", with: urlComponent(crc))
            .replacingOccurrences(of: "${title}", with: urlComponent(title))
        guard !filled.contains("${") else { return nil }
        guard let url = URL(string: filled) else { return nil }
        // Defense-in-depth: even with percent-encoded substitutions, only allow a real
        // http(s) URL whose host is not a loopback/link-local target.
        return Self.isSafeDownloadURL(url) ? url : nil
    }

    private func urlComponent(_ value: String) -> String {
        var allowed = CharacterSet.urlPathAllowed
        allowed.remove(charactersIn: "/")
        return value.addingPercentEncoding(withAllowedCharacters: allowed) ?? value
    }

    // http is permitted alongside https so existing users who configured a plain-http
    // source keep working; https remains the default and the recommendation.
    static func isConfiguredDatabaseTemplate(_ template: String) -> Bool {
        template.hasPrefix("http://") || template.hasPrefix("https://")
    }

    /// Caps downloaded patch files so a misconfigured or hostile source cannot push an
    /// arbitrarily large payload through the single-template download path.
    private static let maxPatchDownloadBytes = 2_000_000

    /// Validates a resolved download URL for the current single-template surface: only
    /// http/https with a real, non-loopback host. Broader private-range blocking and
    /// redirect policing belong with the multi-source repository work.
    private static func isSafeDownloadURL(_ url: URL) -> Bool {
        guard let scheme = url.scheme?.lowercased(),
              scheme == "http" || scheme == "https" else { return false }
        guard let host = url.host, !host.isEmpty else { return false }
        return !isDisallowedHost(host)
    }

    private static func isDisallowedHost(_ host: String) -> Bool {
        let lower = host.lowercased()
        let candidate = lower.hasPrefix("[") && lower.hasSuffix("]")
            ? String(lower.dropFirst().dropLast())
            : lower
        if candidate == "localhost" { return true }
        // IPv4 loopback (127.0.0.0/8) and link-local (169.254.0.0/16).
        if candidate.hasPrefix("127.") { return true }
        if candidate.hasPrefix("169.254.") { return true }
        // IPv6 loopback.
        if candidate == "::1" { return true }
        return false
    }

    static func formattedCRC(_ value: String?) -> String {
        let normalized = PadLayoutGameIdentity.normalizedCRC(value)
        guard let crc = UInt32(normalized, radix: 16), crc != 0 else { return "" }
        return String(format: "%08X", crc)
    }
}
