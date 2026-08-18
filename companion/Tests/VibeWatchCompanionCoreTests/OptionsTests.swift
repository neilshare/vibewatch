#if canImport(XCTest)
import XCTest
import VibeWatchCompanionCoreTestSupport

final class OptionsTests: XCTestCase {
    func testSharedOptionScenarios() throws {
        try Task4Scenarios.run(.options)
    }
}
#else
#error("XCTest is unavailable. Install a full Xcode/macOS test toolchain; run swift run VibeWatchCompanionCoreVerification for the local dependency-free gate.")
#endif
