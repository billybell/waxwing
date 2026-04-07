import SwiftUI
import MapKit

/// Full-screen image detail view.
///
/// First tap on a grid thumbnail opens this view showing a larger image.
/// Tapping the image flips it to reveal metadata: uploader, date/time,
/// and a map pin if the image was geo-tagged.  A back arrow at the top
/// returns to the grid.
struct ImageDetailView: View {
    @EnvironmentObject var bleManager: BLEManager
    @Environment(\.dismiss) private var dismiss

    let fileName: String
    let image: UIImage
    let caption: String?
    let palette: WaxwingPalette

    /// Metadata fetched from the node's sidecar file.
    @State private var metadata: ImageMetadata?
    @State private var metadataLoaded = false

    /// Controls the card flip animation.
    @State private var showingDetails = false

    var body: some View {
        ZStack {
            Color(hex: palette.hexColors[0])
                .ignoresSafeArea()

            VStack(spacing: 0) {
                // Top bar
                topBar

                Spacer()

                // Flippable card
                flipCard
                    .onTapGesture {
                        withAnimation(.spring(response: 0.5, dampingFraction: 0.8)) {
                            showingDetails.toggle()
                        }
                    }

                // Caption
                if let caption, !caption.isEmpty {
                    Text(caption)
                        .font(.subheadline)
                        .foregroundStyle(Color(hex: palette.hexColors[3]))
                        .padding(.top, 12)
                        .padding(.horizontal, 24)
                        .multilineTextAlignment(.center)
                }

                // Hint text
                Text(showingDetails ? "Tap to see image" : "Tap image for details")
                    .font(.caption2)
                    .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.5))
                    .padding(.top, 8)

                Spacer()
            }
        }
        .navigationBarHidden(true)
        .onAppear {
            fetchMetadata()
        }
    }

    // MARK: - Top Bar

    private var topBar: some View {
        HStack {
            Button {
                dismiss()
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "chevron.left")
                        .font(.body.weight(.semibold))
                    Text("Back")
                        .font(.body)
                }
                .foregroundStyle(Color(hex: palette.hexColors[2]))
            }
            .padding(.leading, 16)

            Spacer()

            Text(displayName)
                .font(.caption)
                .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.6))
                .padding(.trailing, 16)
        }
        .padding(.top, 8)
        .padding(.bottom, 4)
    }

    // MARK: - Flip Card

    private var flipCard: some View {
        ZStack {
            // Front: large image
            imageFace
                .opacity(showingDetails ? 0 : 1)
                .rotation3DEffect(
                    .degrees(showingDetails ? 180 : 0),
                    axis: (x: 0, y: 1, z: 0)
                )

            // Back: details
            detailsFace
                .opacity(showingDetails ? 1 : 0)
                .rotation3DEffect(
                    .degrees(showingDetails ? 0 : -180),
                    axis: (x: 0, y: 1, z: 0)
                )
        }
        .frame(maxWidth: 320, maxHeight: 380)
    }

    // MARK: - Image Face (Front)

    private var imageFace: some View {
        VStack(spacing: 0) {
            Image(uiImage: image)
                .resizable()
                .interpolation(.none)
                .scaledToFit()
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .shadow(color: .black.opacity(0.3), radius: 8, y: 4)
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: 16)
                .fill(Color(hex: palette.hexColors[0]))
                .shadow(color: .black.opacity(0.2), radius: 12, y: 6)
        )
    }

    // MARK: - Details Face (Back)

    private var detailsFace: some View {
        VStack(spacing: 16) {
            if !metadataLoaded {
                ProgressView()
                    .tint(Color(hex: palette.hexColors[2]))
                Text("Loading details...")
                    .font(.caption)
                    .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.6))
            } else if let meta = metadata {
                detailsContent(meta)
            } else {
                noMetadataContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(20)
        .background(
            RoundedRectangle(cornerRadius: 16)
                .fill(Color(hex: palette.hexColors[0]).opacity(0.95))
                .overlay(
                    RoundedRectangle(cornerRadius: 16)
                        .stroke(Color(hex: palette.hexColors[2]).opacity(0.3), lineWidth: 1)
                )
                .shadow(color: .black.opacity(0.2), radius: 12, y: 6)
        )
    }

    private func detailsContent(_ meta: ImageMetadata) -> some View {
        VStack(spacing: 16) {
            // Uploader
            if let uploader = meta.uploader, !uploader.isEmpty {
                HStack(spacing: 8) {
                    Image(systemName: "person.circle.fill")
                        .font(.title2)
                        .foregroundStyle(Color(hex: palette.hexColors[2]))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Shared by")
                            .font(.caption2)
                            .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.5))
                        Text(uploader)
                            .font(.subheadline.weight(.medium))
                            .foregroundStyle(Color(hex: palette.hexColors[3]))
                    }
                    Spacer()
                }
            }

            // Timestamp
            HStack(spacing: 8) {
                Image(systemName: "clock")
                    .font(.title3)
                    .foregroundStyle(Color(hex: palette.hexColors[2]))
                VStack(alignment: .leading, spacing: 2) {
                    Text("Uploaded")
                        .font(.caption2)
                        .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.5))
                    Text(meta.timestamp, style: .date)
                        .font(.subheadline)
                        .foregroundStyle(Color(hex: palette.hexColors[3]))
                    Text(meta.timestamp, style: .time)
                        .font(.caption)
                        .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.7))
                }
                Spacer()
            }

            // Map (if geo-tagged)
            if let coord = meta.coordinate {
                mapSection(coord)
            }
        }
    }

    private func mapSection(_ coordinate: CLLocationCoordinate2D) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 4) {
                Image(systemName: "mappin.circle.fill")
                    .foregroundStyle(Color(hex: palette.hexColors[2]))
                Text("Location")
                    .font(.caption2)
                    .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.5))
                Spacer()
                Text(String(format: "%.4f, %.4f", coordinate.latitude, coordinate.longitude))
                    .font(.caption2.monospaced())
                    .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.5))
            }

            Map(initialPosition: .region(
                MKCoordinateRegion(
                    center: coordinate,
                    span: MKCoordinateSpan(latitudeDelta: 0.01, longitudeDelta: 0.01)
                )
            )) {
                Marker("", coordinate: coordinate)
                    .tint(Color(hex: palette.hexColors[2]))
            }
            .frame(height: 140)
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .allowsHitTesting(false)
        }
    }

    private var noMetadataContent: some View {
        VStack(spacing: 12) {
            Image(systemName: "info.circle")
                .font(.system(size: 32))
                .foregroundStyle(Color(hex: palette.hexColors[2]).opacity(0.5))

            Text("No metadata available")
                .font(.subheadline)
                .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.6))

            Text("This image was uploaded without location or identity data.")
                .font(.caption)
                .foregroundStyle(Color(hex: palette.hexColors[3]).opacity(0.4))
                .multilineTextAlignment(.center)
        }
    }

    // MARK: - Helpers

    private func fetchMetadata() {
        bleManager.readFileMeta(name: fileName) { meta in
            metadata = meta
            metadataLoaded = true
        }
    }

    private var displayName: String {
        // Strip "waxwing_" prefix and extension for cleaner display
        var clean = fileName
        if clean.lowercased().hasPrefix("waxwing_") {
            clean = String(clean.dropFirst(8))
        }
        if let dotIndex = clean.lastIndex(of: ".") {
            clean = String(clean[..<dotIndex])
        }
        return clean
    }
}
