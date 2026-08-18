import Foundation

public enum CompanionError: Error, Equatable, LocalizedError {
    case appServer(String)
    case malformedRateLimits(String)
    case bluetooth(String)
    case usage(String)

    public var errorDescription: String? {
        switch self {
        case .appServer(let message), .malformedRateLimits(let message),
             .bluetooth(let message), .usage(let message):
            return message
        }
    }
}

public enum CompanionMode: Equatable, Sendable {
    case approval(ApprovalRequestV2)
    case enterBootloader
    case automaticQuota
    case manualQuota(QuotaSnapshot)
    case demo(QuotaSnapshot)
    case jsonOnly
}

public struct CompanionOptions: Equatable, Sendable {
    public let codexPath: String
    public let mode: CompanionMode
    public let watch: Bool
    public let interval: TimeInterval
    public let verbose: Bool
    public let deviceIdentifier: UUID?
    public let card: AgentCard?
    public let credits: Double?
    public let totalCredits: Double?
    public let legacyApproval: Bool
    public let helpRequested: Bool

    public static func parse(
        _ arguments: [String],
        defaultCodexPath: @autoclosure () -> String = CompanionOptions.defaultCodexPath()
    ) throws -> CompanionOptions {
        var codexPath: String?
        var explicitModes = 0
        var auto = false
        var demo = false
        var jsonOnly = false
        var approval = false
        var bootloader = false
        var watch = false
        var interval: TimeInterval = 60
        var verbose = false
        var deviceIdentifier: UUID?
        var card: AgentCard?
        var credits: Double?
        var totalCredits: Double?
        var remaining: Double?
        var reset: Int?
        var requestID: UUID?
        var agentID = 0
        var operationType = "EXEC"
        var summary = "Run Command"
        var ttlMs = 30_000
        var legacyApproval = false
        var helpRequested = false

        var index = arguments.isEmpty ? 0 : 1
        func nextValue(_ option: String) throws -> String {
            index += 1
            guard index < arguments.count else { throw CompanionError.usage("\(option) requires a value") }
            return arguments[index]
        }

        while index < arguments.count {
            let argument = arguments[index]
            switch argument {
            case "--codex-path": codexPath = try nextValue(argument)
            case "--auto": auto = true; explicitModes += 1
            case "--demo": demo = true; explicitModes += 1
            case "--json-only": jsonOnly = true; explicitModes += 1
            case "--approval": approval = true; explicitModes += 1
            case "--enter-bootloader": bootloader = true; explicitModes += 1
            case "--remaining":
                let raw = try nextValue(argument)
                guard let value = Double(raw), value.isFinite, (0...100).contains(value) else {
                    throw CompanionError.usage("--remaining must be between 0 and 100")
                }
                remaining = value
            case "--reset":
                let raw = try nextValue(argument)
                guard let value = Int(raw), value >= 0 else { throw CompanionError.usage("--reset must be a nonnegative integer") }
                reset = value
            case "--card", "--agent":
                let raw = try nextValue(argument).lowercased()
                guard let value = AgentCard(rawValue: raw) else { throw CompanionError.usage("unknown card: \(raw)") }
                card = value
            case "--credits", "--credit", "--balance":
                let raw = try nextValue(argument)
                guard let value = Double(raw), value.isFinite else { throw CompanionError.usage("--credits requires a number") }
                guard value >= 0 else { throw CompanionError.usage("--credits must be nonnegative") }
                credits = value
            case "--total-credits", "--total":
                let raw = try nextValue(argument)
                guard let value = Double(raw), value.isFinite else { throw CompanionError.usage("--total-credits requires a number") }
                guard value > 0 else { throw CompanionError.usage("--total-credits must be positive") }
                totalCredits = value
            case "--request-id":
                let raw = try nextValue(argument)
                guard raw == raw.lowercased(), let value = UUID(uuidString: raw) else {
                    throw CompanionError.usage("--request-id requires a canonical lowercase UUID")
                }
                requestID = value
            case "--agent-id":
                let raw = try nextValue(argument)
                guard let value = Int(raw), (0...5).contains(value) else { throw CompanionError.usage("--agent-id must be between 0 and 5") }
                agentID = value
            case "--type": operationType = try nextValue(argument)
            case "--summary": summary = try nextValue(argument)
            case "--ttl-ms":
                let raw = try nextValue(argument)
                guard let value = Int(raw) else { throw CompanionError.usage("--ttl-ms requires an integer") }
                ttlMs = value
            case "--legacy-approval": legacyApproval = true
            case "--watch": watch = true
            case "--interval":
                let raw = try nextValue(argument)
                guard let value = Double(raw), value.isFinite, value >= 10 else { throw CompanionError.usage("--interval must be at least 10 seconds") }
                interval = value
            case "--device-id":
                let raw = try nextValue(argument)
                guard let value = UUID(uuidString: raw) else { throw CompanionError.usage("--device-id requires a valid CoreBluetooth UUID") }
                deviceIdentifier = value
            case "--verbose", "-v": verbose = true
            case "--help", "-h": helpRequested = true
            default: throw CompanionError.usage("unknown argument: \(argument); use --help")
            }
            index += 1
        }

        if explicitModes > 1 { throw CompanionError.usage("mode flags are mutually exclusive") }
        if (remaining == nil) != (reset == nil) { throw CompanionError.usage("manual quota requires both --remaining and --reset") }
        let isManual = remaining != nil
        if auto && isManual { throw CompanionError.usage("--auto cannot be combined with manual quota") }
        if isManual && (demo || jsonOnly || approval || bootloader) { throw CompanionError.usage("manual quota cannot be combined with another mode") }
        if !approval && (requestID != nil || agentID != 0 || operationType != "EXEC" || summary != "Run Command" || ttlMs != 30_000 || legacyApproval) {
            throw CompanionError.usage("approval options require --approval")
        }
        if operationType.utf8.isEmpty || operationType.utf8.count > 23 { throw CompanionError.usage("--type must contain 1...23 UTF-8 bytes") }
        if summary.utf8.isEmpty || summary.utf8.count > 95 { throw CompanionError.usage("--summary must contain 1...95 UTF-8 bytes") }
        if !(5_000...120_000).contains(ttlMs) { throw CompanionError.usage("--ttl-ms must be between 5000 and 120000") }
        if watch && (approval || bootloader || jsonOnly || isManual) { throw CompanionError.usage("--watch is only valid for automatic or demo quota") }

        let mode: CompanionMode
        if approval {
            guard deviceIdentifier != nil else { throw CompanionError.usage("--approval requires --device-id") }
            mode = .approval(ApprovalRequestV2(
                requestID: requestID ?? UUID(), card: card ?? .codex, agentID: agentID,
                operationType: operationType, summary: summary, ttlMs: ttlMs
            ))
        } else if bootloader {
            guard deviceIdentifier != nil else { throw CompanionError.usage("--enter-bootloader requires --device-id") }
            mode = .enterBootloader
        } else if demo {
            mode = .demo(QuotaSnapshot(
                remainingPercent: (card == .workbuddy || credits != nil) ? 85 : 59,
                resetInSeconds: card == .workbuddy ? 0 : 3_600, card: card,
                credits: credits ?? (card == .workbuddy ? 1_250 : nil),
                totalCredits: totalCredits ?? (card == .workbuddy ? 1_500 : nil)
            ))
        } else if jsonOnly {
            mode = .jsonOnly
        } else if let remaining, let reset {
            guard deviceIdentifier != nil else { throw CompanionError.usage("manual quota requires --device-id") }
            mode = .manualQuota(QuotaSnapshot(
                remainingPercent: remaining, resetInSeconds: reset, card: card,
                credits: credits, totalCredits: totalCredits
            ))
        } else {
            guard deviceIdentifier != nil || helpRequested else { throw CompanionError.usage("automatic quota requires --device-id") }
            mode = .automaticQuota
        }

        return CompanionOptions(
            codexPath: codexPath ?? defaultCodexPath(), mode: mode, watch: watch,
            interval: interval, verbose: verbose, deviceIdentifier: deviceIdentifier,
            card: card, credits: credits, totalCredits: totalCredits,
            legacyApproval: legacyApproval, helpRequested: helpRequested
        )
    }

    public static var help: String {
        """
        Usage: codex-watch-companion [--auto | --remaining P --reset S | --demo | --json-only]
          --auto             Read Codex quota from App Server
          --remaining P      Manual remaining percentage (requires --reset)
          --reset S          Manual reset interval in seconds (requires --remaining)
          --approval         Send an approval request
          --request-id UUID  Fixed lowercase request UUID for retry/testing
          --agent-id N       Agent slot 0...5
          --type T           Approval operation type (1...23 UTF-8 bytes)
          --summary S        Approval summary (1...95 UTF-8 bytes)
          --ttl-ms N         Approval TTL, 5000...120000 (default 30000)
          --legacy-approval  Use the one-release legacy approval payload
          --device-id UUID   Pinned CoreBluetooth device for every real write
          --demo             Write a synthetic quota snapshot using demo discovery
          --json-only        Print App Server quota JSON without Bluetooth
          --watch            Continuously refresh automatic/demo quota
          --interval N       Refresh interval, at least 10 seconds
          --codex-path P     Explicit codex executable path
          -v, --verbose      Print progress
        """
    }

    private static func executableInPath(named executable: String) -> String? {
        let path = ProcessInfo.processInfo.environment["PATH"] ?? ""
        for directory in path.split(separator: ":", omittingEmptySubsequences: true) {
            let candidate = URL(fileURLWithPath: String(directory)).appendingPathComponent(executable).path
            if FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
        }
        return nil
    }

    public static func defaultCodexPath() -> String {
        if let candidate = executableInPath(named: "codex") { return candidate }
        for candidate in [
            "/Applications/ChatGPT.app/Contents/Resources/codex",
            "/opt/homebrew/bin/codex", "/usr/local/bin/codex",
        ] where FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
        return "/usr/local/bin/codex"
    }
}
