import Foundation

public struct CommandResult: Equatable, Sendable {
    public let stdout: String
    public let exitCode: Int

    public init(stdout: String = "", exitCode: Int = 0) {
        self.stdout = stdout
        self.exitCode = exitCode
    }
}

public struct ErrorOutputV2: Codable, Equatable, Sendable {
    public let version: Int
    public let kind: String
    public let code: String
    public let message: String

    public init(code: String, message: String) {
        self.version = 2
        self.kind = "error"
        self.code = code
        self.message = message
    }
}

public enum MachineOutput {
    public static func errorResult(for error: Error, exitCode: Int = 1) -> CommandResult {
        let code: String
        if let transport = error as? BLETransportError {
            code = transport.code
        } else if case .usage = error as? CompanionError {
            code = "usage_error"
        } else {
            code = "transport_error"
        }
        let response = ErrorOutputV2(code: code, message: error.localizedDescription)
        return CommandResult(stdout: encode(response), exitCode: exitCode)
    }

    public static func encode<T: Encodable>(_ value: T) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard let data = try? encoder.encode(value) else {
            return #"{"code":"transport_error","kind":"error","message":"Could not encode command result.","version":2}"#
        }
        return String(decoding: data, as: UTF8.self)
    }
}

public protocol CommandRunner {
    func run(options: CompanionOptions, emit: (String) -> Void) throws -> CommandResult
}

public final class Runner: CommandRunner {
    private let appServerFactory: (String) throws -> AppServerServing
    private let transport: any BLETransporting
    private let wait: (TimeInterval) -> Void
    private let shouldContinueWatching: (Int) -> Bool
    private let progress: (String) -> Void

    public init(
        appServerFactory: @escaping (String) throws -> AppServerServing = { try AppServerClient(codexPath: $0) },
        transport: any BLETransporting,
        wait: @escaping (TimeInterval) -> Void = { interval in
            RunLoop.current.run(until: Date().addingTimeInterval(interval))
        },
        shouldContinueWatching: @escaping (Int) -> Bool = { _ in true },
        progress: @escaping (String) -> Void = { _ in }
    ) {
        self.appServerFactory = appServerFactory
        self.transport = transport
        self.wait = wait
        self.shouldContinueWatching = shouldContinueWatching
        self.progress = progress
    }

    public func run(options: CompanionOptions, emit: (String) -> Void) throws -> CommandResult {
        do {
            return try runCommand(options: options, emit: emit)
        } catch {
            return MachineOutput.errorResult(for: error)
        }
    }

    private func runCommand(options: CompanionOptions, emit: (String) -> Void) throws -> CommandResult {
        if options.helpRequested {
            emit(CompanionOptions.help)
            return CommandResult()
        }

        switch options.mode {
        case .approval(let request):
            guard let deviceID = options.deviceIdentifier else {
                throw CompanionError.usage("--approval requires --device-id")
            }
            if options.legacyApproval {
                try transport.writeLegacyApproval(request, deviceID: deviceID)
                return CommandResult()
            }
            let decision = try transport.requestApproval(request, deviceID: deviceID)
            guard decision.requestID == request.requestID,
                  decision.version == 2,
                  decision.kind == "approval_decision" else {
                throw BLETransportError.transport("Transport returned an uncorrelated approval decision.")
            }
            return CommandResult(stdout: MachineOutput.encode(decision))

        case .enterBootloader:
            guard let deviceID = options.deviceIdentifier else {
                throw CompanionError.usage("--enter-bootloader requires --device-id")
            }
            try transport.enterBootloader(deviceID: deviceID)
            return CommandResult()

        case .manualQuota(let snapshot):
            guard let deviceID = options.deviceIdentifier else {
                throw CompanionError.usage("manual quota requires --device-id")
            }
            try transport.writeQuota(snapshot, deviceID: deviceID)
            return CommandResult(stdout: MachineOutput.encode(snapshot))

        case .demo(let snapshot):
            let devices = try transport.discoverDemoDevices()
            guard let deviceID = devices.first else { throw BLETransportError.deviceNotFound }
            progress(
                "Demo discovery mode: selected synthetic device \(deviceID.uuidString.lowercased()); "
                    + "bind future real writes with --device-id \(deviceID.uuidString.lowercased())."
            )
            return try runDemoQuotaLoop(
                snapshot: snapshot,
                deviceID: deviceID,
                options: options,
                emit: emit
            )

        case .automaticQuota:
            let client = try appServerFactory(options.codexPath)
            return try runQuotaLoop(client: client, options: options, emit: emit)

        case .bridge(let defaultCard):
            try transport.runBridge(deviceID: options.deviceIdentifier, defaultCard: defaultCard)
            return CommandResult()

        case .jsonOnly:
            var snapshot = try appServerFactory(options.codexPath).readRateLimits()
            applyOverrides(to: &snapshot, options: options)
            return CommandResult(stdout: MachineOutput.encode(snapshot))
        }
    }

    private func runQuotaLoop(
        client: AppServerServing,
        options: CompanionOptions,
        emit: (String) -> Void
    ) throws -> CommandResult {
        guard let deviceID = options.deviceIdentifier else {
            throw CompanionError.usage("automatic quota requires --device-id")
        }
        var completedIterations = 0
        var lastJSON = ""
        repeat {
            var snapshot = try client.readRateLimits()
            applyOverrides(to: &snapshot, options: options)
            try transport.writeQuota(snapshot, deviceID: deviceID)
            lastJSON = MachineOutput.encode(snapshot)
            completedIterations += 1
            if options.watch { emit(lastJSON) }
            guard options.watch, shouldContinueWatching(completedIterations) else { break }
            wait(options.interval)
        } while true
        return CommandResult(stdout: options.watch ? "" : lastJSON)
    }

    private func runDemoQuotaLoop(
        snapshot: QuotaSnapshot,
        deviceID: UUID,
        options: CompanionOptions,
        emit: (String) -> Void
    ) throws -> CommandResult {
        let json = MachineOutput.encode(snapshot)
        var completedIterations = 0
        repeat {
            try transport.writeQuota(snapshot, deviceID: deviceID)
            completedIterations += 1
            if options.watch { emit(json) }
            guard options.watch, shouldContinueWatching(completedIterations) else { break }
            wait(options.interval)
        } while true
        return CommandResult(stdout: options.watch ? "" : json)
    }

    private func applyOverrides(to snapshot: inout QuotaSnapshot, options: CompanionOptions) {
        snapshot.card = options.card
        if let credits = options.credits { snapshot.credits = credits }
        if let totalCredits = options.totalCredits { snapshot.totalCredits = totalCredits }
    }
}
