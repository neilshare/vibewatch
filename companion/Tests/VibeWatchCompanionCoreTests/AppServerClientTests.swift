#if canImport(XCTest)
import XCTest
import VibeWatchCompanionCoreTestSupport

final class AppServerClientTests: XCTestCase {
    func testSharedAppServerAndRunnerScenarios() throws {
        try Task4Scenarios.run(.appServerAndRunner)
    }
}
#else
#error("XCTest is unavailable. Install a full Xcode/macOS test toolchain; run swift run VibeWatchCompanionCoreVerification for the local dependency-free gate.")
#endif
