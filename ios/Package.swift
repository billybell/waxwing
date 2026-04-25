// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "WaxwingCompanion",
    platforms: [
        .iOS(.v17)
    ],
    products: [
        .library(
            name: "WaxwingCompanion",
            targets: ["WaxwingCompanion"]
        )
    ],
    targets: [
        .target(
            name: "WaxwingCompanion",
            path: "WaxwingCompanion/WaxwingCompanion",
            resources: [
                .process("Resources")
            ]
        ),
        .testTarget(
            name: "WaxwingCompanionTests",
            dependencies: ["WaxwingCompanion"],
            path: "WaxwingCompanion/WaxwingCompanionTests"
        )
    ]
)
