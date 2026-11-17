// ReportPhoto.swift — re-encodes a picked photo to an upright JPEG with no metadata segment
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import ImageIO
import UniformTypeIdentifiers

enum ReportPhoto {
    static let maxInputBytes = 50 << 20
    static let maxPixels = 2048
    private static let droppedMarkers: Set<UInt8> = [0xE1, 0xED, 0xFE]

    static func reencode(_ data: Data) -> Data? {
        guard data.count <= maxInputBytes, let source = CGImageSourceCreateWithData(data as CFData, nil),
              CGImageSourceGetCount(source) > 0 else { return nil }
        // Without the transform a portrait photo ends up sideways once the orientation tag is gone.
        let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                        kCGImageSourceCreateThumbnailWithTransform: true,
                                        kCGImageSourceThumbnailMaxPixelSize: maxPixels]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        let output = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(output, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: 0.85] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return withoutMetadata(output as Data)
    }

    // ImageIO still writes a small Exif block of its own, so every segment before the scan is checked by marker.
    private static func withoutMetadata(_ jpeg: Data) -> Data? {
        let bytes = [UInt8](jpeg)
        guard bytes.count > 4, bytes[0] == 0xFF, bytes[1] == 0xD8 else { return nil }
        var kept: [UInt8] = [0xFF, 0xD8]
        var index = 2
        while index + 4 <= bytes.count {
            guard bytes[index] == 0xFF else { return nil }
            let length = Int(bytes[index + 2]) << 8 | Int(bytes[index + 3])
            guard length >= 2, index + 2 + length <= bytes.count else { return nil }
            if bytes[index + 1] == 0xDA { return Data(kept + bytes[index...]) }
            if !droppedMarkers.contains(bytes[index + 1]) { kept += bytes[index..<index + 2 + length] }
            index += 2 + length
        }
        return nil
    }
}
