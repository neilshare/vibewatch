import Foundation
import VibeWatchCompanionCore

public enum Task4ScenarioGroup: Sendable {
    case models
    case options
    case appServerAndRunner
    case approvalTransportAndRunner
}

public struct Task4ScenarioFailure: Error, CustomStringConvertible {
    public let description: String
}

public enum Task4Scenarios {
    @discardableResult
    public static func run(_ group: Task4ScenarioGroup) throws -> Int {
        switch group {
        case .models:
            try runModelScenarios()
            return 3
        case .options:
            try runOptionScenarios()
            return 9
        case .appServerAndRunner:
            try runAppServerAndRunnerScenarios()
            return 7
        case .approvalTransportAndRunner:
            try runApprovalTransportAndRunnerScenarios()
            return 8
        }
    }

    @discardableResult
    public static func runAll() throws -> Int {
        try run(.models) + run(.options) + run(.appServerAndRunner) + run(.approvalTransportAndRunner)
    }

    private static func runModelScenarios() throws {
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
        try check(
            try JSONDecoder().decode(ApprovalRequestV2.self, from: Data(requestJSON.utf8)) == request,
            "approval UUID did not round trip canonically"
        )

        try check(
            ApprovalDecision.allCases.map(\.rawValue) == ["approve", "reject", "expired", "cancelled"],
            "decision raw values changed"
        )
        let decision = ApprovalDecisionV2(requestID: requestID, decision: .reject, decidedAtMs: 184_230)
        try check(
            String(decoding: try encoder.encode(decision), as: UTF8.self) == #"{"decided_at_ms":184230,"decision":"reject","kind":"approval_decision","request_id":"550e8400-e29b-41d4-a716-446655440000","version":2}"#,
            "decision JSON is not exact snake_case"
        )

        let quota = QuotaSnapshot(
            remainingPercent: 42.5, resetInSeconds: 3_600,
            card: .workbuddy, credits: 425, totalCredits: 1_000
        )
        try check(
            String(decoding: try encoder.encode(quota), as: UTF8.self) == #"{"card":"workbuddy","credits":425,"remaining_percent":42.5,"reset_in_seconds":3600,"total_credits":1000}"#,
            "quota JSON is not exact snake_case"
        )
    }

    private static func runOptionScenarios() throws {
        let deviceID = "550e8400-e29b-41d4-a716-446655440000"
        let quota = QuotaSnapshot(
            remainingPercent: 42.5, resetInSeconds: 3_600,
            card: .workbuddy, credits: 425, totalCredits: 1_000
        )
        let manual = try CompanionOptions.parse([
            "tool", "--remaining", "42.5", "--reset", "3600", "--card", "workbuddy",
            "--credits", "425", "--total-credits", "1000", "--device-id", deviceID,
        ])
        try check(manual.mode == .manualQuota(quota), "manual quota values were not preserved")
        try check(
            try CompanionOptions.parse(["tool", "--auto", "--device-id", deviceID]).mode == .automaticQuota,
            "--auto did not select automatic quota"
        )

        try expectUsage(["tool", "--demo", "--card", "other"], "unknown card: other")
        try expectUsage(["tool", "--demo", "--credits", "-1"], "--credits must be nonnegative")
        try expectUsage(["tool", "--demo", "--total-credits", "0"], "--total-credits must be positive")

        for ttl in [5_000, 120_000] {
            let parsed = try CompanionOptions.parse([
                "tool", "--approval", "--summary", "Run", "--ttl-ms", "\(ttl)", "--device-id", deviceID,
            ])
            guard case .approval(let value) = parsed.mode else {
                throw Task4ScenarioFailure(description: "TTL did not produce approval mode")
            }
            try check(value.ttlMs == ttl, "TTL boundary was not preserved")
        }
        try expectUsage(
            ["tool", "--approval", "--summary", "Run", "--ttl-ms", "4999", "--device-id", deviceID],
            "--ttl-ms must be between 5000 and 120000"
        )
        try expectUsage(
            ["tool", "--approval", "--summary", "Run", "--ttl-ms", "120001", "--device-id", deviceID],
            "--ttl-ms must be between 5000 and 120000"
        )

        _ = try CompanionOptions.parse([
            "tool", "--approval", "--summary", String(repeating: "a", count: 93) + "é", "--device-id", deviceID,
        ])
        try expectUsage(
            ["tool", "--approval", "--summary", String(repeating: "a", count: 94) + "é", "--device-id", deviceID],
            "--summary must contain 1...95 UTF-8 bytes"
        )

        try expectUsage(["tool", "--auto", "--demo", "--device-id", deviceID], "mode flags are mutually exclusive")
        try expectUsage(
            ["tool", "--auto", "--remaining", "50", "--reset", "60", "--device-id", deviceID],
            "--auto cannot be combined with manual quota"
        )
        try expectUsage(
            ["tool", "--remaining", "50", "--device-id", deviceID],
            "manual quota requires both --remaining and --reset"
        )

        try expectUsage(["tool", "--auto"], "automatic quota requires --device-id")
        try expectUsage(["tool", "--remaining", "50", "--reset", "60"], "manual quota requires --device-id")
        try expectUsage(["tool", "--approval", "--summary", "Run tests"], "--approval requires --device-id")
        try expectUsage(["tool", "--enter-bootloader"], "--enter-bootloader requires --device-id")
        _ = try CompanionOptions.parse(["tool", "--demo"])
        _ = try CompanionOptions.parse(["tool", "--json-only"])
    }

    private static func runAppServerAndRunnerScenarios() throws {
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        let modern = makeClient([
            #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":27.5,"resetsAt":1700003600}}}}}"#,
        ], now: now)
        try check(
            try modern.client.readRateLimits() == QuotaSnapshot(remainingPercent: 72.5, resetInSeconds: 3_600),
            "modern App Server fixture parsed incorrectly"
        )
        let sentMethod = try JSONSerialization.jsonObject(with: Data(modern.output.lines[0].utf8)) as? [String: Any]
        try check(sentMethod?["method"] as? String == "account/rateLimits/read", "obsolete App Server method used")

        let legacy = makeClient([
            #"{"id":1,"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":80,"resetsAt":1700000120}}}}"#,
        ], now: now)
        try check(
            try legacy.client.readRateLimits() == QuotaSnapshot(remainingPercent: 20, resetInSeconds: 120),
            "legacy App Server fixture parsed incorrectly"
        )

        for malformed in [
            #"{"id":1,"result":{}}"#,
            #"{"id":1,"result":{"rateLimitsByLimitId":{"codex":{"primary":{"resetsAt":1700000120}}}}}"#,
            #"{"id":1,"result":{"rateLimits":{"limitId":"other","primary":{"usedPercent":10,"resetsAt":1700000120}}}}"#,
            "not-json",
        ] {
            try expectMalformed(makeClient([malformed], now: now).client)
        }

        let malformedParser = AppServerResponseParser()
        malformedParser.consume(Data("not-json\n".utf8))
        try check(isMalformed(malformedParser.failure), "production parser seam did not reject malformed JSON")

        let eofParser = AppServerResponseParser()
        eofParser.consume(Data(#"{"id":1,"result":{"rateLimits""#.utf8))
        eofParser.finish()
        try check(isMalformed(eofParser.failure), "production parser seam did not report malformed EOF")

        let deviceID = "550e8400-e29b-41d4-a716-446655440000"
        let options = try CompanionOptions.parse([
            "tool", "--auto", "--watch", "--device-id", deviceID,
        ])
        let appServer = SequenceAppServer([
            QuotaSnapshot(remainingPercent: 80, resetInSeconds: 60),
            QuotaSnapshot(remainingPercent: 70, resetInSeconds: 120),
        ])
        let writer = RecordingWriter()
        var completed = false
        var emitted: [String] = []
        var emittedBeforeCompletion: [Bool] = []
        var waits = 0
        let runner = Runner(
            appServerFactory: { _ in appServer },
            transport: writer,
            wait: { _ in waits += 1 },
            shouldContinueWatching: { completedIterations in completedIterations < 2 }
        )
        let result = try runner.run(options: options) { output in
            emitted.append(output)
            emittedBeforeCompletion.append(!completed)
        }
        completed = true
        try check(emitted.count == 2, "watch mode did not stream exactly two iterations")
        try check(emittedBeforeCompletion == [true, true], "watch output was buffered until completion")
        try check(result.stdout.isEmpty, "watch output accumulated in CommandResult storage")
        try check(writer.quotaWrites == 2 && waits == 1, "finite watch loop did not perform two writes and one wait")
    }

    private static func runApprovalTransportAndRunnerScenarios() throws {
        let requestID = UUID(uuidString: "550e8400-e29b-41d4-a716-446655440000")!
        let otherID = UUID(uuidString: "7d444840-9dc0-11d1-b245-5ffdce74fad2")!
        let deviceID = UUID(uuidString: "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")!
        let request = ApprovalRequestV2(
            requestID: requestID, card: .codex, agentID: 0,
            operationType: "EXEC", summary: "Run tests", ttlMs: 5_000
        )
        let options = try CompanionOptions.parse([
            "tool", "--approval", "--request-id", requestID.uuidString.lowercased(),
            "--summary", "Run tests", "--ttl-ms", "5000", "--device-id", deviceID.uuidString,
        ])

        for decision in ApprovalDecision.allCases {
            let response = ApprovalDecisionV2(
                requestID: requestID, decision: decision, decidedAtMs: 184_230
            )
            let transport = FakeTransport(approvalResult: .success(response))
            let result = try Runner(transport: transport).run(options: options) { _ in }
            try check(result.exitCode == 0, "\(decision.rawValue) was not a delivered success")
            try check(
                result.stdout == expectedDecisionJSON(decision, requestID: requestID),
                "\(decision.rawValue) result was not exact sorted-key JSON"
            )
            try check(transport.requestedApprovals == [request], "runner changed the approval request")
            try check(transport.deviceIDs == [deviceID], "runner did not preserve the pinned device")
        }

        let mismatch = ApprovalDecisionV2(requestID: otherID, decision: .approve, decidedAtMs: 1)
        let matcher = ApprovalResultMatcher(requestID: requestID)
        let encoder = JSONEncoder()
        try check(matcher.receive(try encoder.encode(mismatch)) == nil, "mismatched indication was delivered")
        let matching = ApprovalDecisionV2(requestID: requestID, decision: .approve, decidedAtMs: 2)
        let matchingData = try encoder.encode(matching)
        try check(matcher.receive(matchingData) == matching, "matching indication was not delivered")
        try check(matcher.receive(matchingData) == nil, "duplicate indication was delivered twice")

        let timeout = FakeTransport(approvalResult: .failure(BLETransportError.timeout))
        let timeoutResult = try Runner(transport: timeout).run(options: options) { _ in }
        try check(timeoutResult.exitCode == 1, "transport timeout did not fail")
        try check(
            timeoutResult.stdout == #"{"code":"transport_error","kind":"error","message":"Timed out waiting for a matching approval decision.","version":2}"#,
            "transport timeout error JSON is unstable"
        )

        let missing = FakeTransport(approvalResult: .failure(BLETransportError.deviceNotFound))
        let missingResult = try Runner(transport: missing).run(options: options) { _ in }
        try check(missingResult.exitCode == 1, "missing pinned device did not fail")
        try check(
            missingResult.stdout == #"{"code":"device_not_found","kind":"error","message":"Pinned Bluetooth device was not found.","version":2}"#,
            "missing-device error JSON is unstable"
        )

        let wrongResponse = FakeTransport(approvalResult: .success(mismatch))
        let wrongResult = try Runner(transport: wrongResponse).run(options: options) { _ in }
        try check(wrongResult.exitCode == 1, "runner accepted a mismatched transport response")
        try check(wrongResult.stdout.contains(#""code":"transport_error""#), "mismatch lacked transport_error code")
    }

    private static func expectedDecisionJSON(_ decision: ApprovalDecision, requestID: UUID) -> String {
        #"{"decided_at_ms":184230,"decision":"\#(decision.rawValue)","kind":"approval_decision","request_id":"\#(requestID.uuidString.lowercased())","version":2}"#
    }

    private static func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try condition() else { throw Task4ScenarioFailure(description: message) }
    }

    private static func expectUsage(_ arguments: [String], _ message: String) throws {
        do {
            _ = try CompanionOptions.parse(arguments)
            throw Task4ScenarioFailure(description: "expected usage error: \(message)")
        } catch let error as CompanionError {
            try check(error == .usage(message), "expected usage '\(message)', got '\(error.localizedDescription)'")
        }
    }

    private static func expectMalformed(_ client: AppServerClient) throws {
        do {
            _ = try client.readRateLimits()
            throw Task4ScenarioFailure(description: "malformed App Server fixture manufactured quota")
        } catch CompanionError.malformedRateLimits {
            // Expected.
        }
    }

    private static func isMalformed(_ error: CompanionError?) -> Bool {
        guard case .malformedRateLimits = error else { return false }
        return true
    }

    private static func makeClient(_ lines: [String], now: Date) -> (client: AppServerClient, output: LineRecorder) {
        var responses = lines
        let output = LineRecorder()
        return (
            AppServerClient(
                lineInput: { responses.isEmpty ? nil : responses.removeFirst() },
                lineOutput: { output.lines.append($0) },
                now: { now }
            ),
            output
        )
    }
}

private final class LineRecorder {
    var lines: [String] = []
}

private final class SequenceAppServer: AppServerServing {
    private var snapshots: [QuotaSnapshot]

    init(_ snapshots: [QuotaSnapshot]) {
        self.snapshots = snapshots
    }

    func readRateLimits() throws -> QuotaSnapshot {
        guard !snapshots.isEmpty else {
            throw Task4ScenarioFailure(description: "watch requested too many snapshots")
        }
        return snapshots.removeFirst()
    }
}

private final class RecordingWriter: BLETransporting {
    var quotaWrites = 0

    func writeQuota(_ snapshot: QuotaSnapshot, deviceID: UUID) throws {
        quotaWrites += 1
    }

    func requestApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws -> ApprovalDecisionV2 {
        throw BLETransportError.transport("unexpected approval")
    }
}

private final class FakeTransport: BLETransporting {
    let approvalResult: Result<ApprovalDecisionV2, Error>
    var requestedApprovals: [ApprovalRequestV2] = []
    var deviceIDs: [UUID] = []

    init(approvalResult: Result<ApprovalDecisionV2, Error>) {
        self.approvalResult = approvalResult
    }

    func writeQuota(_ snapshot: QuotaSnapshot, deviceID: UUID) throws {}

    func requestApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws -> ApprovalDecisionV2 {
        requestedApprovals.append(request)
        deviceIDs.append(deviceID)
        return try approvalResult.get()
    }
}
