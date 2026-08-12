// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "AfterDark",
    platforms: [.macOS(.v14)],   // .v14 for the settings inspector (SwiftUI .inspector)
    targets: [
        .target(
            name: "ADCShim"
        ),
        .target(
            name: "AfterDarkKit",
            dependencies: ["ADCShim"],
            resources: [.copy("Resources")]
        ),
        .executableTarget(
            name: "AfterDark",
            dependencies: ["AfterDarkKit"]
        ),
        .executableTarget(
            name: "adrender",
            dependencies: ["AfterDarkKit"]
        ),
        .testTarget(
            name: "AfterDarkKitTests",
            dependencies: ["AfterDarkKit"]
        ),
    ]
)
