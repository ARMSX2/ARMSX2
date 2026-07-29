// AccessibilityHUDMirror.swift — phone-side HUD mirrors which stay independent
// from the Metal game surface.
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

struct AccessibilityHUDMirror: View {
    @State private var battery: Int = -1
    @State private var thermalState: String = "Nominal"
    @State private var ramGB: Double = 0.0
    @State private var lowPower: Bool = false
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(accessibilityLabel)
            .accessibilityAddTraits(.updatesFrequently)
            .onReceive(timer) { _ in refresh() }
    }

    private var accessibilityLabel: String {
        var parts: [String] = []
        if battery >= 0 { parts.append("Battery \(battery)%") }
        parts.append("Thermal \(thermalState)")
        parts.append("RAM \(String(format: "%.1f", ramGB)) GB")
        if lowPower { parts.append("Low Power") }
        return "Status: " + parts.joined(separator: ", ")
    }

    private func refresh() {
        let stats = ARMSX2Bridge.deviceStatsForAccessibility()
        battery = (stats["battery"] as? NSNumber)?.intValue ?? -1
        thermalState = (stats["thermalState"] as? String) ?? "Nominal"
        ramGB = (stats["ramGB"] as? NSNumber)?.doubleValue ?? 0.0
        lowPower = (stats["lowPower"] as? NSNumber)?.boolValue ?? false
    }
}

private struct ExternalDisplayOSDMetrics {
    var valid = false
    var internalFPSValid = false
    var internalFPS = 0.0
    var vps = 0.0
    var speed = 0.0
    var targetSpeed = 0.0
    var cpuUsage = 0.0
    var cpuTime = 0.0
    var gsUsage = 0.0
    var gsTime = 0.0
    var vuUsage = 0.0
    var vuTime = 0.0
    var gpuUsage = 0.0
    var gpuTime = 0.0
    var minimumFrameTime = 0.0
    var averageFrameTime = 0.0
    var maximumFrameTime = 0.0
    var resolutionWidth = 0
    var resolutionHeight = 0
    var videoMode = ""
    var interlaceMode = ""
    var gsStats = ""
    var gsMemoryStats = ""
    var cpuName = ""
    var cpuBigCores = 0
    var cpuSmallCores = 0
    var cpuThreads = 0
    var gpuName = ""

    init() {}

    init(dictionary: [String: Any]) {
        func bool(_ key: String) -> Bool {
            (dictionary[key] as? NSNumber)?.boolValue ?? false
        }
        func double(_ key: String) -> Double {
            (dictionary[key] as? NSNumber)?.doubleValue ?? 0.0
        }
        func int(_ key: String) -> Int {
            (dictionary[key] as? NSNumber)?.intValue ?? 0
        }
        func string(_ key: String) -> String {
            dictionary[key] as? String ?? ""
        }

        valid = bool("valid")
        internalFPSValid = bool("internalFPSValid")
        internalFPS = double("internalFPS")
        vps = double("vps")
        speed = double("speed")
        targetSpeed = double("targetSpeed")
        cpuUsage = double("cpuUsage")
        cpuTime = double("cpuTime")
        gsUsage = double("gsUsage")
        gsTime = double("gsTime")
        vuUsage = double("vuUsage")
        vuTime = double("vuTime")
        gpuUsage = double("gpuUsage")
        gpuTime = double("gpuTime")
        minimumFrameTime = double("minimumFrameTime")
        averageFrameTime = double("averageFrameTime")
        maximumFrameTime = double("maximumFrameTime")
        resolutionWidth = int("resolutionWidth")
        resolutionHeight = int("resolutionHeight")
        videoMode = string("videoMode")
        interlaceMode = string("interlaceMode")
        gsStats = string("gsStats")
        gsMemoryStats = string("gsMemoryStats")
        cpuName = string("cpuName")
        cpuBigCores = int("cpuBigCores")
        cpuSmallCores = int("cpuSmallCores")
        cpuThreads = int("cpuThreads")
        gpuName = string("gpuName")
    }
}

/// Visible performance OSD rendered by SwiftUI on the iPhone only.
///
/// The core deliberately omits ImGui overlays from the dedicated external
/// surface. This mirror appears only while that surface is actually active;
/// with HDMI disconnected or the feature disabled, the original core OSD is
/// left completely unchanged.
struct ExternalDisplayPhoneOSD: View {
    @State private var settings = SettingsStore.shared
    @State private var externalDisplayActive = false
    @State private var metrics = ExternalDisplayOSDMetrics()
    @State private var deviceStatsLine = ""
    @State private var deviceStatsSeverity = 0

    private let timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        Group {
            if shouldShowOverlay {
                GeometryReader { proxy in
                    osdContent
                        .frame(
                            maxWidth: max(1, min(620, proxy.size.width - 16)),
                            alignment: .leading
                        )
                        .padding(.top, max(8, proxy.safeAreaInsets.top + 8))
                        .padding(.leading, max(8, proxy.safeAreaInsets.leading + 8))
                        .padding(.trailing, max(8, proxy.safeAreaInsets.trailing + 8))
                        .frame(
                            maxWidth: .infinity,
                            maxHeight: .infinity,
                            alignment: overlayAlignment
                        )
                }
                .allowsHitTesting(false)
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(readoutLines.joined(separator: ", "))
                .accessibilityAddTraits(.updatesFrequently)
            }
        }
        .onAppear(perform: refresh)
        .onReceive(timer) { _ in refresh() }
    }

    private var shouldShowOverlay: Bool {
        externalDisplayActive &&
            metrics.valid &&
            settings.osdPerformancePosition != 0 &&
            !readoutLines.isEmpty
    }

    private var overlayAlignment: Alignment {
        settings.osdPerformancePosition == 1 ? .topLeading : .topTrailing
    }

    private var osdContent: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Array(readoutLines.enumerated()), id: \.offset) { index, line in
                Text(line)
                    .foregroundStyle(lineColor(at: index))
                    .lineLimit(1)
                    .minimumScaleFactor(0.55)
            }
        }
        .font(.system(size: 11, weight: .semibold, design: .monospaced))
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(.black.opacity(0.72), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .stroke(.white.opacity(0.12), lineWidth: 0.5)
        }
    }

    private var readoutLines: [String] {
        var lines: [String] = []
        var headline: [String] = []

        if settings.osdShowFPS {
            headline.append(metrics.internalFPSValid
                ? String(format: "FPS: %.2f", metrics.internalFPS)
                : "FPS: N/A")
        }
        if settings.osdShowVPS {
            headline.append(String(format: "VPS: %.2f", metrics.vps))
        }
        if settings.osdShowSpeed {
            let target = metrics.targetSpeed == 0
                ? "Max"
                : String(format: "%.0f%%", metrics.targetSpeed * 100)
            headline.append(String(format: "Speed: %.0f%% (T: %@)", metrics.speed, target))
        }
        if settings.osdShowVersion {
            let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
            headline.append(version.isEmpty ? "ARMSX2 iOS" : "ARMSX2 iOS \(version)")
        }
        if !headline.isEmpty {
            lines.append(headline.joined(separator: " | "))
        }

        if settings.osdShowDeviceStats, !deviceStatsLine.isEmpty {
            lines.append(deviceStatsLine)
        }
        if settings.osdShowGSStats {
            if !metrics.gsStats.isEmpty { lines.append(metrics.gsStats) }
            if !metrics.gsMemoryStats.isEmpty { lines.append(metrics.gsMemoryStats) }
        }
        if settings.osdShowResolution, metrics.resolutionWidth > 0, metrics.resolutionHeight > 0 {
            let mode = [metrics.videoMode, metrics.interlaceMode]
                .filter { !$0.isEmpty }
                .joined(separator: " ")
            lines.append("\(metrics.resolutionWidth)x\(metrics.resolutionHeight)" + (mode.isEmpty ? "" : " \(mode)"))
        }
        if settings.osdShowHardwareInfo {
            if !metrics.cpuName.isEmpty {
                var topology: String
                if metrics.cpuSmallCores > 0 {
                    topology = "\(metrics.cpuBigCores)P/\(metrics.cpuSmallCores)E/\(metrics.cpuThreads)T"
                } else {
                    topology = "\(metrics.cpuBigCores)C/\(metrics.cpuThreads)T"
                }
                lines.append("CPU: \(metrics.cpuName) (\(topology))")
            }
            if !metrics.gpuName.isEmpty {
                lines.append("GPU: \(metrics.gpuName)")
            }
        }
        if settings.osdShowCPU {
            lines.append(formatProcessor("EE", usage: metrics.cpuUsage, time: metrics.cpuTime))
            lines.append(formatProcessor("GS", usage: metrics.gsUsage, time: metrics.gsTime))
            if metrics.vuUsage > 0 || metrics.vuTime > 0 {
                lines.append(formatProcessor("VU", usage: metrics.vuUsage, time: metrics.vuTime))
            }
        }
        if settings.osdShowGPU {
            lines.append(formatProcessor("GPU", usage: metrics.gpuUsage, time: metrics.gpuTime))
        }
        if settings.osdShowFrameTimes {
            lines.append(String(
                format: "Frame: Min %.2fms | Avg %.2fms | Max %.2fms",
                metrics.minimumFrameTime,
                metrics.averageFrameTime,
                metrics.maximumFrameTime
            ))
        }

        return lines
    }

    private func formatProcessor(_ name: String, usage: Double, time: Double) -> String {
        if usage >= 99.95 {
            return String(format: "%@: 100%% (%.2fms)", name, time)
        }
        return String(format: "%@: %.1f%% (%.2fms)", name, usage, time)
    }

    private func lineColor(at index: Int) -> Color {
        let hasHeadline = settings.osdShowFPS ||
            settings.osdShowVPS ||
            settings.osdShowSpeed ||
            settings.osdShowVersion
        if index == 0, hasHeadline, settings.osdShowSpeed {
            if metrics.speed < 95 { return Color(red: 1.0, green: 0.39, blue: 0.39) }
            if metrics.speed > 105 { return Color(red: 0.39, green: 1.0, blue: 0.39) }
        }

        let deviceLineIndex = hasHeadline ? 1 : 0
        if settings.osdShowDeviceStats, index == deviceLineIndex {
            if deviceStatsSeverity >= 2 { return Color(red: 1.0, green: 0.39, blue: 0.39) }
            if deviceStatsSeverity == 1 { return Color(red: 1.0, green: 0.86, blue: 0.39) }
        }
        return .white
    }

    private func refresh() {
        let active = ARMSX2Bridge.isDedicatedExternalDisplayActive()
        externalDisplayActive = active
        guard active else {
            metrics = ExternalDisplayOSDMetrics()
            deviceStatsLine = ""
            return
        }

        metrics = ExternalDisplayOSDMetrics(
            dictionary: ARMSX2Bridge.externalDisplayPerformanceMetrics()
        )

        let stats = ARMSX2Bridge.deviceStatsForAccessibility()
        let battery = (stats["battery"] as? NSNumber)?.intValue ?? -1
        let thermal = stats["thermalState"] as? String ?? "Nominal"
        let ram = (stats["ramGB"] as? NSNumber)?.doubleValue ?? 0.0
        let lowPower = (stats["lowPower"] as? NSNumber)?.boolValue ?? false
        deviceStatsSeverity = thermal == "Serious" ? 2 : (thermal == "Fair" ? 1 : 0)
        let batteryText = battery >= 0 ? "\(battery)%" : "--"
        deviceStatsLine = String(
            format: "Battery: %@ | Heat: %@ | RAM: %.1f GB%@",
            batteryText,
            thermal,
            ram,
            lowPower ? " | Low Power" : ""
        )
    }
}
