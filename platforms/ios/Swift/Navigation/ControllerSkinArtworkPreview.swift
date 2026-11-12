// ControllerSkinArtworkPreview.swift — controller skin artwork preview
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// Local preview used while catalog artwork is loading and for bundled or
/// manually imported skins which do not have a remote preview URL. Keeping the
/// preview tied to the selected descriptor also prevents cycling from leaving
/// the previous skin's failed AsyncImage phase onscreen.
struct ControllerSkinArtworkPreview: View {
    let descriptor: VPadSkinDescriptor

    var body: some View {
        if let fullSkin = ControllerAsset.fullSkinImage(
            descriptor: descriptor,
            isLandscape: true
        ) ?? ControllerAsset.fullSkinImage(
            descriptor: descriptor,
            isLandscape: false
        ) {
            Image(uiImage: fullSkin)
                .resizable()
                .interpolation(.high)
                .antialiased(true)
                .scaledToFit()
        } else {
            controllerFace
        }
    }

    private var controllerFace: some View {
        GeometryReader { proxy in
            let width = proxy.size.width
            let height = proxy.size.height
            let unit = min(width / 9.2, height / 4.5)

            ZStack {
                shoulder(.L2, label: "L2", unit: unit)
                    .position(x: width * 0.16, y: height * 0.19)
                shoulder(.L1, label: "L1", unit: unit)
                    .position(x: width * 0.29, y: height * 0.19)
                shoulder(.R1, label: "R1", unit: unit)
                    .position(x: width * 0.71, y: height * 0.19)
                shoulder(.R2, label: "R2", unit: unit)
                    .position(x: width * 0.84, y: height * 0.19)

                dpad(unit: unit)
                    .position(x: width * 0.20, y: height * 0.57)

                padButton(.select, label: "SELECT", size: unit * 0.80)
                    .position(x: width * 0.45, y: height * 0.43)
                padButton(.start, label: "START", size: unit * 0.80)
                    .position(x: width * 0.55, y: height * 0.43)

                analogStick(isLeft: true, unit: unit)
                    .position(x: width * 0.40, y: height * 0.73)
                analogStick(isLeft: false, unit: unit)
                    .position(x: width * 0.60, y: height * 0.73)

                faceButtons(unit: unit)
                    .position(x: width * 0.80, y: height * 0.57)
            }
            .frame(width: width, height: height)
        }
        .padding(8)
    }

    private func shoulder(
        _ button: ARMSX2PadButton,
        label: String,
        unit: CGFloat
    ) -> some View {
        padButton(button, label: label, size: unit * 1.15)
            .frame(height: unit * 0.60)
    }

    private func dpad(unit: CGFloat) -> some View {
        ZStack {
            padButton(.up, label: "▲", size: unit)
                .offset(y: -unit * 0.52)
            padButton(.down, label: "▼", size: unit)
                .offset(y: unit * 0.52)
            padButton(.left, label: "◀", size: unit)
                .offset(x: -unit * 0.52)
            padButton(.right, label: "▶", size: unit)
                .offset(x: unit * 0.52)
        }
        .frame(width: unit * 2.1, height: unit * 2.1)
    }

    private func faceButtons(unit: CGFloat) -> some View {
        ZStack {
            padButton(.triangle, label: "△", size: unit)
                .offset(y: -unit * 0.52)
            padButton(.cross, label: "×", size: unit)
                .offset(y: unit * 0.52)
            padButton(.square, label: "□", size: unit)
                .offset(x: -unit * 0.52)
            padButton(.circle, label: "○", size: unit)
                .offset(x: unit * 0.52)
        }
        .frame(width: unit * 2.1, height: unit * 2.1)
    }

    private func analogStick(isLeft: Bool, unit: CGFloat) -> some View {
        let baseName = ControllerAsset.analogBaseFileName(
            isLeft: isLeft,
            descriptor: descriptor
        )
        let stickName = ControllerAsset.analogStickFileName(
            isLeft: isLeft,
            descriptor: descriptor
        )
        return ZStack {
            controllerImage(
                fileName: baseName,
                fallback: "",
                size: unit * 1.52
            )
            controllerImage(
                fileName: stickName,
                fallback: isLeft ? "L3" : "R3",
                size: unit * 0.86
            )
        }
        .frame(width: unit * 1.55, height: unit * 1.55)
    }

    private func padButton(
        _ button: ARMSX2PadButton,
        label: String,
        size: CGFloat
    ) -> some View {
        controllerImage(
            fileName: ControllerAsset.fileName(for: button),
            fallback: label,
            size: size
        )
    }

    private func controllerImage(
        fileName: String,
        fallback: String,
        size: CGFloat
    ) -> some View {
        ControllerAssetImage(
            fileName: fileName,
            fallback: fallback,
            fallbackColor: .white,
            fallbackFontSize: max(9, size * 0.24),
            skin: descriptor.virtualPadSkin,
            descriptor: descriptor
        )
        .frame(width: size, height: size)
    }
}
