#if canImport(XCTest)
import XCTest
import VibeWatchCompanionCoreTestSupport

final class BLETransportTests: XCTestCase {
    func testSharedApprovalDecisionAndProtocolErrorScenarios() throws {
        try Task4Scenarios.run(.approvalTransportAndRunner)
    }
}
#else
#error("XCTest is unavailable. Install a full Xcode/macOS test toolchain; run swift run VibeWatchCompanionCoreVerification for the local dependency-free gate.")
#endif
