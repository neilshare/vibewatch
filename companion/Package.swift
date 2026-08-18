// swift-tools-version: 5.10

import PackageDescription

let package = Package(
    name: "CodexWatchCompanion",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "CodexWatchCompanion", targets: ["CodexWatchCompanion"]),
        .executable(name: "codex-watch-companion", targets: ["CodexWatchCompanion"]),
    ],
    targets: [
        .target(name: "VibeWatchCompanionCore"),
        .executableTarget(
            name: "CodexWatchCompanion",
            dependencies: ["VibeWatchCompanionCore"]
        ),
        .testTarget(
            name: "VibeWatchCompanionCoreTests",
            dependencies: ["VibeWatchCompanionCore"]
        ),
        .executableTarget(
            name: "VibeWatchCompanionCoreVerification",
            dependencies: ["VibeWatchCompanionCore"],
            path: "Tests/VibeWatchCompanionCoreVerification"
        ),
    ]
)
