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
        .executableTarget(name: "CodexWatchCompanion"),
    ]
)
