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

// MARK: - Prepared Source
//
// A photo, reduced to its grayscale 128×128 buffer. Produced once per
// photo pick and held in @State; slider/palette changes operate on this
// 64-KiB buffer instead of the 30–50 MiB full-res original. This is what
// lets the contrast/brightness sliders be smooth without OOMing.

struct PreparedSource: Sendable {
    /// Grayscale luminance, row-major, values in [0, 1]. Length is
    /// `size * size`.
    let grayscale: [Float]
    let size: Int
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

    // MARK: - Prepare (expensive, runs once per photo)

    /// Reduce a full-resolution UIImage down to a 128×128 grayscale buffer
    /// with EXIF orientation and center-crop already baked in. This is
    /// the ONLY step that touches the full-resolution pixel buffer;
    /// subsequent slider/rotation/palette changes work from the returned
    /// `PreparedSource`.
    static func prepare(source: UIImage) -> PreparedSource? {
        let sz = outputSize
        guard let resized = centerCropAndResize(source, to: sz) else {
            return nil
        }
        guard let cgImage = resized.cgImage else { return nil }
        let width = cgImage.width
        let height = cgImage.height
        let pixelCount = width * height
        guard let pixelData = extractRGBA(from: cgImage, width: width, height: height) else {
            return nil
        }

        var gray = [Float](repeating: 0, count: pixelCount)
        for i in 0..<pixelCount {
            let r = Float(pixelData[i * 4]) / 255.0
            let g = Float(pixelData[i * 4 + 1]) / 255.0
            let b = Float(pixelData[i * 4 + 2]) / 255.0
            gray[i] = 0.299 * r + 0.587 * g + 0.114 * b
        }
        return PreparedSource(grayscale: gray, size: sz)
    }

    // MARK: - Render (cheap, runs per slider tick)

    /// Apply contrast/brightness, rotation, dither, and palette to a
    /// `PreparedSource`. Working set is ~256 KB regardless of the
    /// original photo size — this is what makes the sliders cheap.
    static func render(
        prepared: PreparedSource,
        palette: WaxwingPalette,
        rotationSteps: Int = 0,
        contrast: Float = 1.15,
        brightness: Float = 0.0
    ) -> (image: UIImage, pngData: Data)? {
        let size = prepared.size
        let pixelCount = size * size
        let levels: Float = 3.0  // 0,1,2,3
        let steps = ((rotationSteps % 4) + 4) % 4

        // Apply contrast + brightness, then ordered dithering, in one pass.
        // Rotation is folded in by remapping (x, y) → source coordinates;
        // the prepared buffer is never copied.
        var indices = [UInt8](repeating: 0, count: pixelCount)
        for y in 0..<size {
            for x in 0..<size {
                let (sx, sy): (Int, Int)
                switch steps {
                case 1: sx = y;          sy = size - 1 - x
                case 2: sx = size - 1 - x; sy = size - 1 - y
                case 3: sx = size - 1 - y; sy = x
                default: sx = x;         sy = y
                }
                let si = sy * size + sx
                let v = max(0, min(1, (prepared.grayscale[si] - 0.5) * contrast + 0.5 + brightness))
                let threshold = (Float(bayer4x4[y % 4][x % 4]) + 0.5) / 16.0
                let dithered = v * levels + (threshold - 0.5)
                indices[y * size + x] = UInt8(max(0, min(3, Int(round(dithered)))))
            }
        }

        // Map indices → palette RGBA.
        let paletteColors = palette.colors
        var outputPixels = [UInt8](repeating: 0, count: pixelCount * 4)
        for i in 0..<pixelCount {
            let pc = paletteColors[Int(indices[i])]
            outputPixels[i * 4]     = pc.r
            outputPixels[i * 4 + 1] = pc.g
            outputPixels[i * 4 + 2] = pc.b
            outputPixels[i * 4 + 3] = 255
        }

        guard let outputImage = imageFromRGBA(outputPixels, width: size, height: size),
              let pngData = outputImage.pngData() else {
            return nil
        }
        return (outputImage, pngData)
    }

    // MARK: - One-shot convenience (caller doesn't want to manage PreparedSource)

    /// Equivalent to `prepare` followed by `render`. Allocates the
    /// PreparedSource, uses it once, then discards it.
    static func process(
        source: UIImage,
        palette: WaxwingPalette,
        contrast: Float = 1.15,
        brightness: Float = 0.0
    ) -> (image: UIImage, pngData: Data)? {
        guard let prepared = prepare(source: source) else { return nil }
        return render(prepared: prepared, palette: palette,
                      contrast: contrast, brightness: brightness)
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

    /// Center-crop the source image to a square and resize to `size x
    /// size`, baking in EXIF orientation. Drawing directly into the
    /// 128×128 destination through one `UIGraphicsImageRenderer` avoids
    /// any intermediate full-resolution allocation. EXIF orientation is
    /// applied via `UIImage.draw(in:)`, which honours `imageOrientation`
    /// automatically — no separate "normalize" pass is needed, which
    /// also avoids the full-size renderer that used to drive the OOM.
    private static func centerCropAndResize(_ image: UIImage, to size: Int) -> UIImage? {
        // Compute the largest centered square in the image's *visual*
        // (orientation-adjusted) coordinate space.
        let visualSize = image.size      // already accounts for orientation
        let side = min(visualSize.width, visualSize.height)
        let dx = (visualSize.width - side) / 2
        let dy = (visualSize.height - side) / 2

        let format = UIGraphicsImageRendererFormat()
        format.scale = 1.0
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(
            size: CGSize(width: size, height: size),
            format: format
        )
        return renderer.image { _ in
            // Draw the image into a 128×128 canvas with the source square
            // mapped over the full output. UIImage.draw applies EXIF
            // orientation, so the bytes we read back are upright.
            let target = CGRect(
                x: -dx * (CGFloat(size) / side),
                y: -dy * (CGFloat(size) / side),
                width: visualSize.width * (CGFloat(size) / side),
                height: visualSize.height * (CGFloat(size) / side)
            )
            image.draw(in: target)
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
