// CoverThumbnailView.swift - Cover thumbnail rendering and cache
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import ImageIO

struct CoverThumbnailView: View {
    let gameName: String
    let coverURL: URL?
    let coverSignature: String?
    let width: CGFloat
    let height: CGFloat
    var cornerRadius: CGFloat = 10

    @State private var image: UIImage?

    private var cacheID: String {
        "\(coverSignature ?? "placeholder")|\(Int(width))x\(Int(height))"
    }

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .strokeBorder(.white.opacity(0.12), lineWidth: 1)

            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: width, height: height)
                    .clipped()
            } else {
                VStack(spacing: 6) {
                    Image(systemName: gameName.lowercased().hasSuffix(".chd") ? "archivebox" : "opticaldisc")
                        .font(.system(size: 24, weight: .medium))
                    Text(gameName.lowercased().hasSuffix(".chd") ? "CHD" : "PS2")
                        .font(.caption2)
                        .fontWeight(.bold)
                }
                .foregroundStyle(.secondary)
            }
        }
        .frame(width: width, height: height)
        .clipShape(
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        )
        .task(id: cacheID) {
            await loadThumbnail()
        }
        .accessibilityHidden(true)
    }

    @MainActor
    private func loadThumbnail() async {
        guard let coverURL else {
            image = nil
            return
        }

        let scale = UIScreen.main.scale
        if let cached = CoverThumbnailCache.shared.cachedImage(
            for: coverURL,
            signature: coverSignature,
            width: width,
            height: height,
            scale: scale
        ) {
            image = cached
            return
        }

        let loadedImage = await CoverThumbnailCache.shared.thumbnail(
            for: coverURL,
            signature: coverSignature,
            width: width,
            height: height,
            scale: scale
        )
        guard !Task.isCancelled else { return }
        image = loadedImage
    }
}

/// Bounds ImageIO work while allowing a complete visible row to decode at once.
private actor CoverThumbnailDecodeLimiter {
    private let maximumConcurrentDecodes: Int
    private var activeDecodes = 0
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private var nextWaiterIndex = 0

    init(maximumConcurrentDecodes: Int) {
        self.maximumConcurrentDecodes = max(1, maximumConcurrentDecodes)
    }

    func acquire() async {
        if activeDecodes < maximumConcurrentDecodes {
            activeDecodes += 1
            return
        }
        await withCheckedContinuation { continuation in
            waiters.append(continuation)
        }
    }

    func release() {
        if nextWaiterIndex < waiters.count {
            let continuation = waiters[nextWaiterIndex]
            nextWaiterIndex += 1
            continuation.resume()
            return
        }

        waiters.removeAll(keepingCapacity: true)
        nextWaiterIndex = 0
        activeDecodes = max(0, activeDecodes - 1)
    }
}

/// Shares identical in-flight thumbnail requests and runs unique decodes
/// through a small utility-priority pool.
private actor CoverThumbnailDecodeCoordinator {
    static let shared = CoverThumbnailDecodeCoordinator()

    private struct Request {
        let token: UUID
        let task: Task<UIImage?, Never>
    }

    private let limiter = CoverThumbnailDecodeLimiter(
        // Four decodes fill the visible rail quickly without returning to the
        // unbounded work that can starve native scrolling.
        maximumConcurrentDecodes: 4
    )
    private var requests: [String: Request] = [:]

    func image(
        for key: String,
        priority: TaskPriority,
        decode: @escaping @Sendable () -> UIImage?
    ) async -> UIImage? {
        if let request = requests[key] {
            return await request.task.value
        }

        let token = UUID()
        let limiter = self.limiter
        let task: Task<UIImage?, Never> = Task.detached(priority: priority) {
            await limiter.acquire()
            guard !Task.isCancelled else {
                await limiter.release()
                return nil
            }
            let image = decode()
            await limiter.release()
            return Task.isCancelled ? nil : image
        }
        requests[key] = Request(token: token, task: task)

        let image = await task.value
        if requests[key]?.token == token {
            requests.removeValue(forKey: key)
        }
        return image
    }
}

struct CoverThumbnailPreheatItem: Hashable, Sendable {
    let url: URL
    let signature: String?
}

final class CoverThumbnailCache: @unchecked Sendable {
    static let shared = CoverThumbnailCache()

    private final class WeakImage {
        weak var value: UIImage?
        init(_ value: UIImage) { self.value = value }
    }

    private let cache = NSCache<NSString, UIImage>()
    private let stateLock = NSLock()
    private var latestImageBySource: [String: WeakImage] = [:]
    private var generation: UInt64 = 0
    private var acceptsImages = true

    private init() {
        // Match Master's smooth revisit behavior: scrolling back to a recently
        // visible card should not immediately decode it again.
        cache.countLimit = 768
        cache.totalCostLimit = 96 * 1024 * 1024
    }

    func cachedImage(for url: URL, signature: String?, width: CGFloat, height: CGFloat, scale: CGFloat) -> UIImage? {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard acceptsImages else { return nil }
        return cache.object(forKey: cacheKey(for: url, signature: signature, width: width, height: height, scale: scale))
    }

    /// Returns the selected card's already-decoded cover when possible. A user
    /// can tap before CoverThumbnailView's asynchronous task finishes, so the
    /// transition performs one bounded downsample rather than showing a
    /// placeholder or cancelling the animation.
    func imageForGameplayTransition(
        for url: URL,
        signature: String?,
        width: CGFloat,
        height: CGFloat,
        scale: CGFloat
    ) -> UIImage? {
        if let cached = cachedImage(
            for: url,
            signature: signature,
            width: width,
            height: height,
            scale: scale
        ) {
            return cached
        }
        stateLock.lock()
        let visibleCardImage = acceptsImages
            ? latestImageBySource[
                sourceKey(for: url, signature: signature)
            ]?.value
            : nil
        stateLock.unlock()
        if let visibleCardImage {
            return visibleCardImage
        }

        let sourceOptions = [
            kCGImageSourceShouldCache: false,
        ] as CFDictionary
        let thumbnailOptions = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize:
                max(1, Int(max(width, height) * scale)),
        ] as CFDictionary

        if let source = CGImageSourceCreateWithURL(
            url as CFURL,
            sourceOptions
        ),
           let cgImage = CGImageSourceCreateThumbnailAtIndex(
            source,
            0,
            thumbnailOptions
           ) {
            return UIImage(cgImage: cgImage, scale: scale, orientation: .up)
        }

        return UIImage(contentsOfFile: url.path)
    }

    func thumbnail(
        for url: URL,
        signature: String?,
        width: CGFloat,
        height: CGFloat,
        scale: CGFloat,
        priority: TaskPriority = .userInitiated
    ) async -> UIImage? {
        let key = cacheKey(for: url, signature: signature, width: width, height: height, scale: scale)
        guard let request = beginRequest(for: key) else { return nil }
        if let cachedImage = request.cachedImage {
            return cachedImage
        }
        let requestGeneration = request.generation

        let path = url.path
        let maxPixelSize = thumbnailPixelSize(width: width, height: height, scale: scale)
        let image = await CoverThumbnailDecodeCoordinator.shared.image(
            for: (key as String) + "|generation:\(requestGeneration)",
            priority: priority
        ) {
            guard self.isCurrentGeneration(requestGeneration) else { return nil }
            return autoreleasepool {
                Self.decodeThumbnail(
                    at: url,
                    path: path,
                    maxPixelSize: maxPixelSize,
                    scale: scale
                )
            }
        }

        guard !Task.isCancelled, let image else { return nil }
        let cost = max(1, Int(image.size.width * image.scale * image.size.height * image.scale * 4))
        guard insert(
            image,
            for: key,
            sourceKey: sourceKey(for: url, signature: signature),
            cost: cost,
            generation: requestGeneration
        ) else { return nil }

        return image
    }

    /// Warms only the small look-ahead window selected by the cover-flow
    /// controller. These requests remain utility priority and share in-flight
    /// work with visible CoverThumbnailView instances.
    func preheat(
        _ items: [CoverThumbnailPreheatItem],
        width: CGFloat,
        height: CGFloat,
        scale: CGFloat
    ) async {
        await withTaskGroup(of: Void.self) { group in
            for item in items {
                group.addTask(priority: .utility) { [self] in
                    _ = await thumbnail(
                        for: item.url,
                        signature: item.signature,
                        width: width,
                        height: height,
                        scale: scale,
                        priority: .utility
                    )
                }
            }
        }
    }

    private static func decodeThumbnail(
        at url: URL,
        path: String,
        maxPixelSize: Int,
        scale: CGFloat
    ) -> UIImage? {
        guard !Task.isCancelled else { return nil }
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        let thumbnailOptions = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
        ] as CFDictionary

        guard let source = CGImageSourceCreateWithURL(
            url as CFURL,
            sourceOptions
        ),
              let cgImage = CGImageSourceCreateThumbnailAtIndex(
                source,
                0,
                thumbnailOptions
              ) else {
            NSLog("[ARMSX2 iOS Covers] thumbnail decode failed %@", path)
            return nil
        }

        guard !Task.isCancelled else { return nil }
        return UIImage(cgImage: cgImage, scale: scale, orientation: .up)
    }

    static func signature(for url: URL?) -> String? {
        guard let url else { return nil }
        let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
        let modified = values?.contentModificationDate?.timeIntervalSince1970 ?? 0
        let size = values?.fileSize ?? 0
        return "\(url.path)|\(modified)|\(size)"
    }

    func activateForMenu() {
        stateLock.lock()
        defer { stateLock.unlock() }
        if !acceptsImages {
            generation &+= 1
        }
        acceptsImages = true
    }

    func releaseForGameplay() {
        stateLock.lock()
        generation &+= 1
        acceptsImages = false
        cache.removeAllObjects()
        latestImageBySource.removeAll(keepingCapacity: false)
        stateLock.unlock()
    }

    func releaseInactiveLibraryImages() {
        stateLock.lock()
        generation &+= 1
        cache.removeAllObjects()
        latestImageBySource.removeAll(keepingCapacity: false)
        stateLock.unlock()
    }

    private func isCurrentGeneration(_ value: UInt64) -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return acceptsImages && generation == value
    }

    private func beginRequest(for key: NSString) -> (generation: UInt64, cachedImage: UIImage?)? {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard acceptsImages else { return nil }
        return (generation, cache.object(forKey: key))
    }

    private func insert(
        _ image: UIImage,
        for key: NSString,
        sourceKey: String,
        cost: Int,
        generation requestGeneration: UInt64
    ) -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard acceptsImages, generation == requestGeneration else { return false }
        cache.setObject(image, forKey: key, cost: cost)
        // Keep only a weak source lookup; NSCache remains the sole cache owner
        // and can still evict decoded covers under memory pressure.
        latestImageBySource[sourceKey] = WeakImage(image)
        return true
    }

    private func sourceKey(for url: URL, signature: String?) -> String {
        signature ?? url.path
    }

    private func cacheKey(for url: URL, signature: String?, width: CGFloat, height: CGFloat, scale: CGFloat) -> NSString {
        let pixels = thumbnailPixelSize(width: width, height: height, scale: scale)
        return "\(signature ?? url.path)|\(pixels)@\(scale)" as NSString
    }

    private func thumbnailPixelSize(width: CGFloat, height: CGFloat, scale: CGFloat) -> Int {
        // Reuse a decode while sliders change by a few pixels.
        max(32, Int(ceil(max(width, height) * scale / 32)) * 32)
    }
}
