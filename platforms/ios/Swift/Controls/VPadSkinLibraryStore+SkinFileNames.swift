// VPadSkinLibraryStore+SkinFileNames.swift — virtual pad skin file names
// SPDX-License-Identifier: GPL-3.0+

import Foundation

extension VPadSkinLibraryStore {
    static func canonicalSkinFileName(forImportPath path: String) -> String? {
        let url = URL(fileURLWithPath: path)
        let exact = url.lastPathComponent.lowercased()
        let expected = Set([
            "ic_controller_up_button.png",
            "ic_controller_down_button.png",
            "ic_controller_left_button.png",
            "ic_controller_right_button.png",
            "ic_controller_cross_button.png",
            "ic_controller_circle_button.png",
            "ic_controller_square_button.png",
            "ic_controller_triangle_button.png",
            "ic_controller_l1_button.png",
            "ic_controller_r1_button.png",
            "ic_controller_l2_button.png",
            "ic_controller_r2_button.png",
            "ic_controller_start_button.png",
            "ic_controller_select_button.png",
            "ic_controller_l3_button.png",
            "ic_controller_r3_button.png",
            "ic_controller_analog_base_left.png",
            "ic_controller_analog_base_right.png",
            "ic_controller_analog_base.png",
            "ic_controller_analog_stick.png",
            "ic_controller_analog_button.png",
            "ic_controller_analog_stick_left.png",
            "ic_controller_analog_stick_right.png",
            "ic_controller_analog_button_left.png",
            "ic_controller_analog_button_right.png"
        ])
        if expected.contains(exact) {
            return exact
        }

        let stem = url.deletingPathExtension().lastPathComponent
            .lowercased()
            .replacingOccurrences(of: "[^a-z0-9]+", with: "_", options: .regularExpression)

        let pairs: [(String, String)] = [
            ("analog_base_left", "ic_controller_analog_base_left.png"),
            ("analog_base_right", "ic_controller_analog_base_right.png"),
            ("left_analog_base", "ic_controller_analog_base_left.png"),
            ("right_analog_base", "ic_controller_analog_base_right.png"),
            ("analog_base", "ic_controller_analog_base.png"),
            ("analog_stick_left", "ic_controller_analog_stick_left.png"),
            ("analog_stick_right", "ic_controller_analog_stick_right.png"),
            ("analog_button_left", "ic_controller_analog_button_left.png"),
            ("analog_button_right", "ic_controller_analog_button_right.png"),
            ("analog_stick", "ic_controller_analog_stick.png"),
            ("analog_button", "ic_controller_analog_button.png"),
            ("stick", "ic_controller_analog_stick.png"),
            ("base", "ic_controller_analog_base.png"),
            ("l1", "ic_controller_l1_button.png"),
            ("r1", "ic_controller_r1_button.png"),
            ("l2", "ic_controller_l2_button.png"),
            ("r2", "ic_controller_r2_button.png"),
            ("l3", "ic_controller_l3_button.png"),
            ("r3", "ic_controller_r3_button.png"),
            ("start", "ic_controller_start_button.png"),
            ("select", "ic_controller_select_button.png"),
            ("triangle", "ic_controller_triangle_button.png"),
            ("circle", "ic_controller_circle_button.png"),
            ("square", "ic_controller_square_button.png"),
            ("cross", "ic_controller_cross_button.png"),
            ("up", "ic_controller_up_button.png"),
            ("down", "ic_controller_down_button.png"),
            ("left", "ic_controller_left_button.png"),
            ("right", "ic_controller_right_button.png")
        ]

        return pairs.first { stem.contains($0.0) }?.1
    }
}
