#if canImport(XCTest)
import Foundation
import XCTest
@testable import VibeWatchCompanionCore

final class OptionsTests: XCTestCase {
    private let deviceID = "550e8400-e29b-41d4-a716-446655440000"

    func testManualQuotaPreservesEveryValue() throws {
        let options = try CompanionOptions.parse([
            "tool", "--remaining", "42.5", "--reset", "3600",
            "--card", "workbuddy", "--credits", "425", "--total-credits", "1000",
            "--device-id", deviceID,
        ])

        XCTAssertEqual(
            options.mode,
            .manualQuota(QuotaSnapshot(
                remainingPercent: 42.5,
                resetInSeconds: 3_600,
                card: .workbuddy,
                credits: 425,
                totalCredits: 1_000
            ))
        )
    }

    func testAutoExplicitlySelectsAppServerQuota() throws {
        let options = try CompanionOptions.parse(["tool", "--auto", "--device-id", deviceID])
        XCTAssertEqual(options.mode, .automaticQuota)
    }

    func testRejectsUnknownCardAndInvalidCreditValues() {
        assertUsage(["tool", "--demo", "--card", "other"], "unknown card: other")
        assertUsage(["tool", "--demo", "--credits", "-1"], "--credits must be nonnegative")
        assertUsage(["tool", "--demo", "--total-credits", "0"], "--total-credits must be positive")
    }

    func testApprovalAcceptsTTLBounds() throws {
        for ttl in [5_000, 120_000] {
            let options = try CompanionOptions.parse([
                "tool", "--approval", "--summary", "Run tests", "--ttl-ms", String(ttl),
                "--device-id", deviceID,
            ])
            guard case .approval(let request) = options.mode else {
                return XCTFail("expected approval mode")
            }
            XCTAssertEqual(request.ttlMs, ttl)
        }
    }

    func testApprovalRejectsTTLOutsideBounds() {
        assertUsage(
            ["tool", "--approval", "--summary", "Run", "--ttl-ms", "4999", "--device-id", deviceID],
            "--ttl-ms must be between 5000 and 120000"
        )
        assertUsage(
            ["tool", "--approval", "--summary", "Run", "--ttl-ms", "120001", "--device-id", deviceID],
            "--ttl-ms must be between 5000 and 120000"
        )
    }

    func testApprovalSummaryLimitCountsUTF8Bytes() throws {
        _ = try CompanionOptions.parse([
            "tool", "--approval", "--summary", String(repeating: "a", count: 93) + "é",
            "--device-id", deviceID,
        ])
        assertUsage(
            ["tool", "--approval", "--summary", String(repeating: "a", count: 94) + "é", "--device-id", deviceID],
            "--summary must contain 1...95 UTF-8 bytes"
        )
    }

    func testRejectsIncompatibleModeFlagsAndIncompleteManualQuota() {
        assertUsage(["tool", "--auto", "--demo", "--device-id", deviceID], "mode flags are mutually exclusive")
        assertUsage(["tool", "--auto", "--remaining", "50", "--reset", "60", "--device-id", deviceID], "--auto cannot be combined with manual quota")
        assertUsage(["tool", "--remaining", "50", "--device-id", deviceID], "manual quota requires both --remaining and --reset")
    }

    func testEveryNonDemoWriteRequiresPinnedDevice() {
        assertUsage(["tool", "--auto"], "automatic quota requires --device-id")
        assertUsage(["tool", "--remaining", "50", "--reset", "60"], "manual quota requires --device-id")
        assertUsage(["tool", "--approval", "--summary", "Run tests"], "--approval requires --device-id")
        assertUsage(["tool", "--enter-bootloader"], "--enter-bootloader requires --device-id")
    }

    func testDemoAndJSONOnlyDoNotRequirePinnedDevice() throws {
        XCTAssertNoThrow(try CompanionOptions.parse(["tool", "--demo"]))
        XCTAssertNoThrow(try CompanionOptions.parse(["tool", "--json-only"]))
    }

    private func assertUsage(_ arguments: [String], _ message: String, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertThrowsError(try CompanionOptions.parse(arguments), file: file, line: line) { error in
            XCTAssertEqual(error as? CompanionError, .usage(message), file: file, line: line)
        }
    }
}
#endif
