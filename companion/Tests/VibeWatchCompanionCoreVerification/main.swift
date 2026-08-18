import Foundation
import VibeWatchCompanionCore

private struct CheckFailure: Error, CustomStringConvertible {
    let description: String
}

private final class LineRecorder {
    var lines: [String] = []
}

private func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
    guard try condition() else { throw CheckFailure(description: message) }
}

private func expectUsage(_ arguments: [String], _ message: String) throws {
    do {
        _ = try CompanionOptions.parse(arguments)
        throw CheckFailure(description: "expected usage error: \(message)")
    } catch let error as CompanionError {
        try check(error == .usage(message), "expected usage '\(message)', got '\(error.localizedDescription)'")
    }
}

private func fixtureClient(
    _ lines: [String],
    sentLines: LineRecorder,
    now: Date
) -> AppServerClient {
    var responses = lines
    return AppServerClient(
        lineInput: { responses.isEmpty ? nil : responses.removeFirst() },
        lineOutput: { sentLines.lines.append($0) },
        now: { now }
    )
}

@main
private struct Task4Verification {
    static func main() throws {
        let requestID = UUID(uuidString: "550e8400-e29b-41d4-a716-446655440000")!
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]

        let request = ApprovalRequestV2(
            requestID: requestID, card: .codex, agentID: 0,
            operationType: "EXEC", summary: "Run tests", ttlMs: 30_000
        )
        let requestJSON = String(decoding: try encoder.encode(request), as: UTF8.self)
        try check(
            requestJSON == #"{"agent_id":0,"card":"codex","kind":"approval_request","operation_type":"EXEC","request_id":"550e8400-e29b-41d4-a716-446655440000","summary":"Run tests","ttl_ms":30000,"version":2}"#,
            "approval JSON is not exact snake_case"
        )
        try check(try JSONDecoder().decode(ApprovalRequestV2.self, from: Data(requestJSON.utf8)) == request, "approval UUID did not round trip canonically")

        try check(ApprovalDecision.allCases.map(\.rawValue) == ["approve", "reject", "expired", "cancelled"], "decision raw values changed")
        let decision = ApprovalDecisionV2(requestID: requestID, decision: .reject, decidedAtMs: 184_230)
        try check(
            String(decoding: try encoder.encode(decision), as: UTF8.self) == #"{"decided_at_ms":184230,"decision":"reject","kind":"approval_decision","request_id":"550e8400-e29b-41d4-a716-446655440000","version":2}"#,
            "decision JSON is not exact snake_case"
        )

        let quota = QuotaSnapshot(remainingPercent: 42.5, resetInSeconds: 3_600, card: .workbuddy, credits: 425, totalCredits: 1_000)
        try check(
            String(decoding: try encoder.encode(quota), as: UTF8.self) == #"{"card":"workbuddy","credits":425,"remaining_percent":42.5,"reset_in_seconds":3600,"total_credits":1000}"#,
            "quota JSON is not exact snake_case"
        )

        let deviceID = "550e8400-e29b-41d4-a716-446655440000"
        let manual = try CompanionOptions.parse([
            "tool", "--remaining", "42.5", "--reset", "3600", "--card", "workbuddy",
            "--credits", "425", "--total-credits", "1000", "--device-id", deviceID,
        ])
        try check(manual.mode == .manualQuota(quota), "manual quota values were not preserved")
        try check(try CompanionOptions.parse(["tool", "--auto", "--device-id", deviceID]).mode == .automaticQuota, "--auto did not select automatic quota")

        try expectUsage(["tool", "--demo", "--card", "other"], "unknown card: other")
        try expectUsage(["tool", "--demo", "--credits", "-1"], "--credits must be nonnegative")
        try expectUsage(["tool", "--demo", "--total-credits", "0"], "--total-credits must be positive")
        for ttl in [5_000, 120_000] {
            let parsed = try CompanionOptions.parse(["tool", "--approval", "--summary", "Run", "--ttl-ms", "\(ttl)", "--device-id", deviceID])
            guard case .approval(let value) = parsed.mode else { throw CheckFailure(description: "TTL did not produce approval mode") }
            try check(value.ttlMs == ttl, "TTL boundary was not preserved")
        }
        try expectUsage(["tool", "--approval", "--summary", "Run", "--ttl-ms", "4999", "--device-id", deviceID], "--ttl-ms must be between 5000 and 120000")
        try expectUsage(["tool", "--approval", "--summary", "Run", "--ttl-ms", "120001", "--device-id", deviceID], "--ttl-ms must be between 5000 and 120000")
        _ = try CompanionOptions.parse(["tool", "--approval", "--summary", String(repeating: "a", count: 93) + "é", "--device-id", deviceID])
        try expectUsage(["tool", "--approval", "--summary", String(repeating: "a", count: 94) + "é", "--device-id", deviceID], "--summary must contain 1...95 UTF-8 bytes")
        try expectUsage(["tool", "--auto", "--demo", "--device-id", deviceID], "mode flags are mutually exclusive")
        try expectUsage(["tool", "--auto", "--remaining", "50", "--reset", "60", "--device-id", deviceID], "--auto cannot be combined with manual quota")
        try expectUsage(["tool", "--remaining", "50", "--device-id", deviceID], "manual quota requires both --remaining and --reset")
        try expectUsage(["tool", "--auto"], "automatic quota requires --device-id")
        try expectUsage(["tool", "--remaining", "50", "--reset", "60"], "manual quota requires --device-id")
        try expectUsage(["tool", "--approval", "--summary", "Run tests"], "--approval requires --device-id")
        try expectUsage(["tool", "--enter-bootloader"], "--enter-bootloader requires --device-id")
        _ = try CompanionOptions.parse(["tool", "--demo"])
        _ = try CompanionOptions.parse(["tool", "--json-only"])

        let now = Date(timeIntervalSince1970: 1_700_000_000)
        var sent = LineRecorder()
        let modern = fixtureClient([
            #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":27.5,"resetsAt":1700003600}}}}}"#,
        ], sentLines: sent, now: now)
        try check(try modern.readRateLimits() == QuotaSnapshot(remainingPercent: 72.5, resetInSeconds: 3_600), "modern App Server fixture parsed incorrectly")
        let sentMethod = try JSONSerialization.jsonObject(with: Data(sent.lines[0].utf8)) as? [String: Any]
        try check(sentMethod?["method"] as? String == "account/rateLimits/read", "obsolete App Server method used")

        sent = LineRecorder()
        let legacy = fixtureClient([
            #"{"id":1,"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":80,"resetsAt":1700000120}}}}"#,
        ], sentLines: sent, now: now)
        try check(try legacy.readRateLimits() == QuotaSnapshot(remainingPercent: 20, resetInSeconds: 120), "legacy App Server fixture parsed incorrectly")

        for malformed in [#"{"id":1,"result":{}}"#, #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"resetsAt":1700000120}}}}}"#, "not-json"] {
            sent = LineRecorder()
            let client = fixtureClient([malformed], sentLines: sent, now: now)
            do {
                _ = try client.readRateLimits()
                throw CheckFailure(description: "malformed App Server fixture manufactured quota")
            } catch CompanionError.malformedRateLimits {
                // Expected.
            }
        }

        print("PASS: 15 Task 4 behavior groups")
    }
}
