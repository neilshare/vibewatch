#if canImport(XCTest)
import Foundation
import XCTest
@testable import VibeWatchCompanionCore

final class ProtocolModelsTests: XCTestCase {
    private let requestID = UUID(uuidString: "550e8400-e29b-41d4-a716-446655440000")!

    func testApprovalRequestUsesExactSnakeCaseJSONAndCanonicalUUID() throws {
        let request = ApprovalRequestV2(
            requestID: requestID,
            card: .codex,
            agentID: 0,
            operationType: "EXEC",
            summary: "Run tests",
            ttlMs: 30_000
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]

        let json = String(decoding: try encoder.encode(request), as: UTF8.self)

        XCTAssertEqual(
            json,
            #"{"agent_id":0,"card":"codex","kind":"approval_request","operation_type":"EXEC","request_id":"550e8400-e29b-41d4-a716-446655440000","summary":"Run tests","ttl_ms":30000,"version":2}"#
        )
        XCTAssertEqual(try JSONDecoder().decode(ApprovalRequestV2.self, from: Data(json.utf8)), request)
    }

    func testDecisionValuesAndSnakeCaseJSON() throws {
        XCTAssertEqual(ApprovalDecision.allCases.map(\.rawValue), ["approve", "reject", "expired", "cancelled"])
        let decision = ApprovalDecisionV2(
            version: 2,
            kind: "approval_decision",
            requestID: requestID,
            decision: .reject,
            decidedAtMs: 184_230
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]

        XCTAssertEqual(
            String(decoding: try encoder.encode(decision), as: UTF8.self),
            #"{"decided_at_ms":184230,"decision":"reject","kind":"approval_decision","request_id":"550e8400-e29b-41d4-a716-446655440000","version":2}"#
        )
    }

    func testQuotaSnapshotUsesSnakeCaseJSON() throws {
        let snapshot = QuotaSnapshot(
            remainingPercent: 42.5,
            resetInSeconds: 3_600,
            card: .workbuddy,
            credits: 425,
            totalCredits: 1_000
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]

        XCTAssertEqual(
            String(decoding: try encoder.encode(snapshot), as: UTF8.self),
            #"{"card":"workbuddy","credits":425,"remaining_percent":42.5,"reset_in_seconds":3600,"total_credits":1000}"#
        )
    }
}
#endif
