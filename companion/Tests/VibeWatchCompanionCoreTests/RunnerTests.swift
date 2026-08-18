#if canImport(XCTest)
import XCTest
import VibeWatchCompanionCoreTestSupport

final class RunnerTests: XCTestCase {
    func testSharedApprovalRunnerScenarios() throws {
        try Task4Scenarios.run(.approvalTransportAndRunner)
    }

    func testSharedAutomaticAndDemoWatchScenarios() throws {
        try Task4Scenarios.run(.appServerAndRunner)
    }
}
#else
#error("XCTest is unavailable. Install a full Xcode/macOS test toolchain; run swift run VibeWatchCompanionCoreVerification for the local dependency-free gate.")
#endif
