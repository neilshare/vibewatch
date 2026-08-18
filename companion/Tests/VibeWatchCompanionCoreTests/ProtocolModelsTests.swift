#if canImport(XCTest)
import XCTest
import VibeWatchCompanionCoreTestSupport

final class ProtocolModelsTests: XCTestCase {
    func testSharedModelScenarios() throws {
        try Task4Scenarios.run(.models)
    }
}
#else
#error("XCTest is unavailable. Install a full Xcode/macOS test toolchain; run swift run VibeWatchCompanionCoreVerification for the local dependency-free gate.")
#endif
