import UIKit
import CoreGraphics

// ============================================================
// Waxwing Image Processor
// Bayer 4x4 ordered dithering → 4-color indexed PNG at 128x128.
// Three selectable color palettes; all share the same dither.
// ============================================================

/// A named 4-color palette for the Waxwing micro-image format.
struct PaletteColor: Equatable, Hashable {
    let r: UInt8
    let g: UInt8
    let b: UInt8
}

struct WaxwingPalette: Identifiable, Hashable {
    let id: String
    let name: String
    let subtitle: String
    let colors: [PaletteColor]  // exactly 4, dark → light

    /// SwiftUI-friendly hex strings for UI swatches
    let hexColors: [String]

    /// Background color (darkest tone)
    var backgroundColor: UIColor {
        let c = colors[0]
        return UIColor(red: CGFloat(c.r)/255, green: CGFloat(c.g)/255, blue: CGFloat(c.b)/255, alpha: 1)
    }
}

/// All available palettes — order matches the React prototype.
enum WaxwingPalettes {
    static let cedar = WaxwingPalette(
        id: "cedar",
        name: "Cedar",
        subtitle: "Warm woodblock",
        colors: [PaletteColor(r:0x2D,g:0x1F,b:0x0B), PaletteColor(r:0x8B,g:0x69,b:0x14), PaletteColor(r:0xC4,g:0xA3,b:0x5A), PaletteColor(r:0xF5,g:0xF0,b:0xE1)],
        hexColors: ["#2D1F0B","#8B6914","#C4A35A","#F5F0E1"]
    )

    static let waxseal = WaxwingPalette(
        id: "waxseal",
        name: "Waxseal",
        subtitle: "Crimson stamp",
        colors: [PaletteColor(r:0x1A,g:0x05,b:0x05), PaletteColor(r:0xB2,g:0x22,b:0x22), PaletteColor(r:0xD4,g:0x83,b:0x6A), PaletteColor(r:0xF5,g:0xF0,b:0xE1)],
        hexColors: ["#1A0505","#B22222","#D4836A","#F5F0E1"]
    )

    static let signal = WaxwingPalette(
        id: "signal",
        name: "Signal",
        subtitle: "Electric mesh",
        colors: [PaletteColor(r:0x0A,g:0x0A,b:0x0A), PaletteColor(r:0x1B,g:0x3A,b:0x2D), PaletteColor(r:0x2E,g:0xC4,b:0xB6), PaletteColor(r:0xCB,g:0xF3,b:0xF0)],
        hexColors: ["#0A0A0A","#1B3A2D","#2EC4B6","#CBF3F0"]
    )

    static let all: [WaxwingPalette] = [cedar, waxseal, signal]

    static func palette(for id: String) -> WaxwingPalette {
        all.first(where: { $0.id == id }) ?? cedar
    }
}

// MARK: - Processor

enum WaxwingImageProcessor {

    /// Fixed output resolution.
    static let outputSize = 128

    /// Bayer 4x4 threshold matrix (values 0–15).
    private static let bayer4x4: [[Int]] = [
        [ 0, 8, 2,10],
        [12, 4,14, 6],
        [ 3,11, 1, 9],
        [15, 7,13, 5]
    ]

    // MARK: Public

    /// Process a UIImage into a 128x128 Waxwing micro-image.
    /// Returns the processed UIImage (RGB, suitable for preview) and the PNG data.
    ///
    /// - Parameters:
    ///   - source: The original photo.
    ///   - palette: Which color palette to apply.
    ///   - contrast: Contrast multiplier (default 1.15).
    ///   - brightness: Brightness offset (default 0).
    /// - Returns: Tuple of `(previewImage, pngData)`, or nil on failure.
    static func process(
        source: UIImage,
        palette: WaxwingPalette,
        contrast: Float = 1.15,
        brightness: Float = 0.0
    ) -> (image: UIImage, pngData: Data)? {
        let sz = outputSize

        // 1. Center-crop and resize to 128x128
        guard let resized = centerCropAndResize(source, to: sz) else { return nil }

        // 2. Extract pixel data
        guard let cgImage = resized.cgImage else { return nil }
        let width = cgImage.width
        let height = cgImage.height
        let pixelCount = width * height

        // Get raw RGBA bytes
        guard let pixelData = extractRGBA(from: cgImage, width: width, height: height) else { return nil }

        // 3. Convert to grayscale (0.0 – 1.0)
        var gray = [Float](repeating: 0, count: pixelCount)
        for i in 0..<pixelCount {
            let r = Float(pixelData[i * 4]) / 255.0
            let g = Float(pixelData[i * 4 + 1]) / 255.0
            let b = Float(pixelData[i * 4 + 2]) / 255.0
            gray[i] = 0.299 * r + 0.587 * g + 0.114 * b
        }

        // 4. Apply contrast + brightness
        for i in 0..<pixelCount {
            gray[i] = max(0, min(1, (gray[i] - 0.5) * contrast + 0.5 + brightness))
        }

        // 5. Bayer 4x4 ordered dithering → 4-level indices
        var indices = [UInt8](repeating: 0, count: pixelCount)
        let levels: Float = 3.0  // 0,1,2,3
        for y in 0..<height {
            for x in 0..<width {
                let i = y * width + x
                let threshold = (Float(bayer4x4[y % 4][x % 4]) + 0.5) / 16.0
                let dithered = gray[i] * levels + (threshold - 0.5)
                indices[i] = UInt8(max(0, min(3, Int(round(dithered)))))
            }
        }

        // 6. Map indices to palette RGBA
        let paletteColors = palette.colors
        var outputPixels = [UInt8](repeating: 0, count: pixelCount * 4)
        for i in 0..<pixelCount {
            let pc = paletteColors[Int(indices[i])]
            outputPixels[i * 4]     = pc.r
            outputPixels[i * 4 + 1] = pc.g
            outputPixels[i * 4 + 2] = pc.b
            outputPixels[i * 4 + 3] = 255
        }

        // 7. Build UIImage for preview (premultipliedLast — safe for UIKit/SwiftUI)
        guard let outputImage = imageFromRGBA(outputPixels, width: width, height: height) else { return nil }

        // 8. Encode as standard RGBA PNG for preview / size estimation.
        //    Alpha stripping is deferred to upload time via
        //    `stripAlphaForUpload(_:)` to avoid allocating an extra
        //    CGContext on every slider adjustment.
        guard let pngData = outputImage.pngData() else { return nil }

        return (outputImage, pngData)
    }

    // MARK: - Upload encoding

    /// Re-encode PNG data as RGB (no alpha channel) for upload to the Pico.
    ///
    /// Call this **once** at upload time — not during live preview — to
    /// avoid the extra CGContext allocation on every slider adjustment.
    /// Returns the smaller RGB PNG, or falls back to the original data
    /// if the device/OS doesn't support the noneSkipLast bitmap path.
    static func stripAlphaForUpload(_ pngData: Data) -> Data {
        guard let uiImage = UIImage(data: pngData),
              let cgImage = uiImage.cgImage else {
            return pngData
        }
        let w = cgImage.width
        let h = cgImage.height
        if let rgbData = rgbPNGData(from: cgImage, width: w, height: h) {
            return rgbData
        }
        return pngData   // fallback — keep original RGBA
    }

    // MARK: - Internal helpers

    /// Normalize a UIImage so its `imageOrientation` is `.up` and
    /// the raw pixel buffer matches the visual display.
    ///
    /// Camera photos typically have orientation metadata (e.g. `.right`
    /// for portrait shots). CGImage ignores this, so all downstream
    /// pixel work would see the raw (rotated) buffer.  Drawing through
    /// UIGraphicsImageRenderer bakes the transform into the pixels.
    private static func normalizeOrientation(_ image: UIImage) -> UIImage {
        guard image.imageOrientation != .up else { return image }
        let format = UIGraphicsImageRendererFormat()
        format.scale = image.scale
        let renderer = UIGraphicsImageRenderer(size: image.size, format: format)
        return renderer.image { _ in
            image.draw(at: .zero)
        }
    }

    /// Center-crop the source image to a square and resize to `size x size`.
    ///
    /// The input is first orientation-normalized so that `cgImage` pixel
    /// dimensions match the visual layout regardless of EXIF metadata.
    private static func centerCropAndResize(_ image: UIImage, to size: Int) -> UIImage? {
        let normalized = normalizeOrientation(image)
        guard let cg = normalized.cgImage else { return nil }
        let w = cg.width
        let h = cg.height
        let side = min(w, h)
        let cropRect = CGRect(
            x: (w - side) / 2,
            y: (h - side) / 2,
            width: side,
            height: side
        )
        guard let cropped = cg.cropping(to: cropRect) else { return nil }

        let format = UIGraphicsImageRendererFormat()
        format.scale = 1.0
        let renderer = UIGraphicsImageRenderer(
            size: CGSize(width: size, height: size),
            format: format
        )
        return renderer.image { ctx in
            UIImage(cgImage: cropped).draw(in: CGRect(x: 0, y: 0, width: size, height: size))
        }
    }

    /// Extract raw RGBA pixel data from a CGImage.
    private static func extractRGBA(from cgImage: CGImage, width: Int, height: Int) -> [UInt8]? {
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        guard let context = CGContext(
            data: &pixels,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else { return nil }
        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))
        return pixels
    }

    /// Create a UIImage from raw RGBA bytes.
    ///
    /// Uses `premultipliedLast` for broad UIKit / SwiftUI compatibility.
    /// Alpha is kept throughout the editing pipeline; it is only stripped
    /// at upload time via `stripAlphaForUpload(_:)`.
    private static func imageFromRGBA(_ pixels: [UInt8], width: Int, height: Int) -> UIImage? {
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        var mutablePixels = pixels
        guard let context = CGContext(
            data: &mutablePixels,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else { return nil }
        guard let cgImage = context.makeImage() else { return nil }
        return UIImage(cgImage: cgImage)
    }

    /// Encode a CGImage as an RGB PNG (no alpha channel) for smaller files.
    ///
    /// Draws the source image into a `noneSkipLast` context so the
    /// resulting CGImage has no alpha plane.  `pngData()` then writes
    /// PNG color type 2 (RGB) instead of type 6 (RGBA), saving ~25%
    /// on pixel data — meaningful for the 128×128 images sent over BLE.
    ///
    /// Returns nil if the device/OS doesn't support this path, so
    /// callers should fall back to regular `pngData()`.
    private static func rgbPNGData(from cgImage: CGImage, width: Int, height: Int) -> Data? {
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let context = CGContext(
            data: nil,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
        ) else { return nil }
        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))
        guard let rgbImage = context.makeImage() else { return nil }
        return UIImage(cgImage: rgbImage).pngData()
    }
}
