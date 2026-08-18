import Foundation

public protocol BLEWriting: AnyObject {
    func writeQuota(_ payload: Data, deviceID: UUID?, verbose: Bool) throws
    func writeApproval(_ payload: Data, deviceID: UUID, verbose: Bool) throws
    func enterBootloader(deviceID: UUID, verbose: Bool) throws
}

public struct CommandResult: Equatable, Sendable {
    public let stdout: String
    public let exitCode: Int

    public init(stdout: String = "", exitCode: Int = 0) {
        self.stdout = stdout
        self.exitCode = exitCode
    }
}

public protocol CommandRunner {
    func run(options: CompanionOptions, emit: (String) -> Void) throws -> CommandResult
}

public final class Runner: CommandRunner {
    private let appServerFactory: (String) throws -> AppServerServing
    private let writer: any BLEWriting
    private let wait: (TimeInterval) -> Void
    private let shouldContinueWatching: (Int) -> Bool

    public init(
        appServerFactory: @escaping (String) throws -> AppServerServing = { try AppServerClient(codexPath: $0) },
        writer: any BLEWriting,
        wait: @escaping (TimeInterval) -> Void = { interval in
            RunLoop.current.run(until: Date().addingTimeInterval(interval))
        },
        shouldContinueWatching: @escaping (Int) -> Bool = { _ in true }
    ) {
        self.appServerFactory = appServerFactory
        self.writer = writer
        self.wait = wait
        self.shouldContinueWatching = shouldContinueWatching
    }

    public func run(options: CompanionOptions, emit: (String) -> Void) throws -> CommandResult {
        if options.helpRequested {
            emit(CompanionOptions.help)
            return CommandResult()
        }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]

        switch options.mode {
        case .approval(let request):
            struct LegacyApproval: Encodable {
                let method = "v.oai.approval_req"
                struct Params: Encodable {
                    let active = true
                    let type: String
                    let summary: String
                    let card: String
                }
                let params: Params
            }
            let payload = try encoder.encode(LegacyApproval(params: .init(
                type: request.operationType,
                summary: request.summary,
                card: request.card.rawValue
            )))
            guard let deviceID = options.deviceIdentifier else {
                throw CompanionError.usage("--approval requires --device-id")
            }
            try writer.writeApproval(payload, deviceID: deviceID, verbose: options.verbose)
            emit("✓ 已向 StopWatch 发送人工确认请求：[\(request.operationType)] \(request.summary) (Card: \(request.card.rawValue))")

        case .enterBootloader:
            guard let deviceID = options.deviceIdentifier else {
                throw CompanionError.usage("--enter-bootloader requires --device-id")
            }
            try writer.enterBootloader(deviceID: deviceID, verbose: options.verbose)
            emit("bootloader 命令已获 ATT ACK，随后 BLE 断开；仅在新 USB 串口出现后继续刷写")

        case .manualQuota(let snapshot):
            emit(try writeQuota(snapshot, options: options, encoder: encoder))

        case .demo(let snapshot):
            try runQuotaLoop(initial: snapshot, client: nil, options: options, encoder: encoder, emit: emit)

        case .automaticQuota:
            let client = try appServerFactory(options.codexPath)
            try runQuotaLoop(initial: nil, client: client, options: options, encoder: encoder, emit: emit)

        case .jsonOnly:
            var snapshot = try appServerFactory(options.codexPath).readRateLimits()
            applyOverrides(to: &snapshot, options: options)
            emit(try encode(snapshot, with: encoder))
        }
        return CommandResult()
    }

    private func runQuotaLoop(
        initial: QuotaSnapshot?,
        client: AppServerServing?,
        options: CompanionOptions,
        encoder: JSONEncoder,
        emit: (String) -> Void
    ) throws {
        var completedIterations = 0
        repeat {
            var snapshot: QuotaSnapshot
            if let initial {
                snapshot = initial
            } else if let client {
                snapshot = try client.readRateLimits()
                applyOverrides(to: &snapshot, options: options)
            } else {
                throw CompanionError.malformedRateLimits("没有可用的额度来源")
            }

            emit(try writeQuota(snapshot, options: options, encoder: encoder))
            completedIterations += 1
            guard options.watch, shouldContinueWatching(completedIterations) else { break }
            wait(options.interval)
        } while true
    }

    private func writeQuota(
        _ snapshot: QuotaSnapshot,
        options: CompanionOptions,
        encoder: JSONEncoder
    ) throws -> String {
        let payload = try encoder.encode(snapshot)
        let json = try encode(snapshot, with: encoder)
        try writer.writeQuota(payload, deviceID: options.deviceIdentifier, verbose: options.verbose)
        return "\(json)\n✓ 已写入 StopWatch：剩余 \(Int(snapshot.remainingPercent.rounded()))%，\(formatReset(seconds: snapshot.resetInSeconds)) 后重置"
    }

    private func encode(_ snapshot: QuotaSnapshot, with encoder: JSONEncoder) throws -> String {
        let data = try encoder.encode(snapshot)
        guard let json = String(data: data, encoding: .utf8) else {
            throw CompanionError.malformedRateLimits("无法编码额度 JSON")
        }
        return json
    }

    private func applyOverrides(to snapshot: inout QuotaSnapshot, options: CompanionOptions) {
        snapshot.card = options.card
        if let credits = options.credits { snapshot.credits = credits }
        if let totalCredits = options.totalCredits { snapshot.totalCredits = totalCredits }
    }

    private func formatReset(seconds: Int) -> String {
        if seconds >= 86_400 { return "\(seconds / 86_400)d \((seconds % 86_400) / 3_600)h" }
        if seconds >= 3_600 { return "\(seconds / 3_600)h \((seconds % 3_600) / 60)m" }
        return "\(max(0, seconds) / 60)m"
    }
}
