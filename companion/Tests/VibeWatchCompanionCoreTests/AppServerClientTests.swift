#if canImport(XCTest)
import Foundation
import XCTest
@testable import VibeWatchCompanionCore

final class AppServerClientTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_700_000_000)

    func testReadsRateLimitsByLimitIDFixture() throws {
        let client = AppServerClient(
            responseLines: [
                #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":27.5,"resetsAt":1700003600}}}}}"#,
            ],
            now: { self.now }
        )

        XCTAssertEqual(
            try client.readRateLimits(),
            QuotaSnapshot(remainingPercent: 72.5, resetInSeconds: 3_600)
        )
        XCTAssertEqual(client.sentMethods, ["account/rateLimits/read"])
    }

    func testReadsLegacyRateLimitsFixture() throws {
        let client = AppServerClient(
            responseLines: [
                #"{"id":1,"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":80,"resetsAt":1700000120}}}}"#,
            ],
            now: { self.now }
        )

        XCTAssertEqual(
            try client.readRateLimits(),
            QuotaSnapshot(remainingPercent: 20, resetInSeconds: 120)
        )
    }

    func testMalformedOrMissingDataNeverManufacturesQuota() {
        let fixtures = [
            #"{"id":1,"result":{}}"#,
            #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"resetsAt":1700000120}}}}}"#,
            #"{"id":1,"result":{"rateLimits":{"limitId":"other","primary":{"usedPercent":10,"resetsAt":1700000120}}}}"#,
            "not-json",
        ]

        for fixture in fixtures {
            let client = AppServerClient(responseLines: [fixture], now: { self.now })
            XCTAssertThrowsError(try client.readRateLimits()) { error in
                guard case .malformedRateLimits = error as? CompanionError else {
                    return XCTFail("expected malformedRateLimits, got \(error)")
                }
            }
        }
    }
}
#endif
