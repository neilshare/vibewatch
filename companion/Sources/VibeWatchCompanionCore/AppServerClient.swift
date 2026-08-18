import CoreFoundation
import Foundation

public protocol AppServerServing: AnyObject {
    func readRateLimits() throws -> QuotaSnapshot
}

public final class AppServerResponseParser {
    private var buffer = Data()
    private var responses: [Int: [String: Any]] = [:]
    public private(set) var failure: CompanionError?

    public init() {}

    public func consume(_ data: Data) {
        guard failure == nil else { return }
        guard !data.isEmpty else {
            finish()
            return
        }
        buffer.append(data)
        while let newline = buffer.firstIndex(of: 0x0A) {
            let line = Data(buffer[..<newline])
            buffer.removeSubrange(...newline)
            parseLine(line)
            if failure != nil { return }
        }
    }

    public func finish() {
        guard failure == nil else { return }
        if !buffer.isEmpty {
            let finalLine = buffer
            buffer.removeAll(keepingCapacity: false)
            parseLine(finalLine)
        }
        if failure == nil {
            failure = .malformedRateLimits("Codex App Server 在返回匹配响应前结束输出")
        }
    }

    public func takeResponse(id: Int) -> [String: Any]? {
        responses.removeValue(forKey: id)
    }

    public func hasResponse(id: Int) -> Bool {
        responses[id] != nil
    }

    private func parseLine(_ line: Data) {
        guard !line.isEmpty else { return }
        guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else {
            failure = .malformedRateLimits("Codex App Server 返回了无效 JSON")
            return
        }
        guard let id = (object["id"] as? NSNumber)?.intValue else { return }
        responses[id] = object
    }
}

public final class AppServerClient: AppServerServing {
    private let process: Process?
    private let inputPipe: Pipe?
    private let outputPipe: Pipe?
    private let errorPipe: Pipe?
    private let condition = NSCondition()
    private let responseParser = AppServerResponseParser()
    private var stderrTail = ""
    private var nextID = 1
    private var lineInput: (() throws -> String?)?
    private var lineOutput: ((String) throws -> Void)?
    private let now: () -> Date
    private var recordedMethods: [String] = []

    public init(codexPath: String) throws {
        let process = Process()
        let inputPipe = Pipe()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        self.process = process
        self.inputPipe = inputPipe
        self.outputPipe = outputPipe
        self.errorPipe = errorPipe
        now = Date.init

        process.executableURL = URL(fileURLWithPath: codexPath)
        process.arguments = ["app-server", "--listen", "stdio://"]
        process.standardInput = inputPipe
        process.standardOutput = outputPipe
        process.standardError = errorPipe

        outputPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.consume(handle.availableData)
        }
        errorPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            guard let self,
                  let text = String(data: handle.availableData, encoding: .utf8),
                  !text.isEmpty else { return }
            condition.lock()
            stderrTail = String((stderrTail + text).suffix(2_000))
            condition.unlock()
        }

        do {
            try process.run()
        } catch {
            throw CompanionError.appServer("无法启动 Codex App Server（\(codexPath)）：\(error.localizedDescription)")
        }

        try send([
            "method": "initialize",
            "id": 0,
            "params": [
                "clientInfo": [
                    "name": "codex_watch_companion",
                    "title": "Codex Watch Companion",
                    "version": "0.1.0",
                ],
                "capabilities": [
                    "optOutNotificationMethods": [
                        "item/agentMessage/delta",
                        "item/reasoning/textDelta",
                    ],
                ],
            ],
        ])
        _ = try waitForResponse(id: 0, timeout: 12)
        try send(["method": "initialized", "params": [:]])
    }

    public init(
        lineInput: @escaping () throws -> String?,
        lineOutput: @escaping (String) throws -> Void = { _ in },
        now: @escaping () -> Date = Date.init
    ) {
        process = nil
        inputPipe = nil
        outputPipe = nil
        errorPipe = nil
        self.lineInput = lineInput
        self.lineOutput = lineOutput
        self.now = now
    }

    convenience init(responseLines: [String], now: @escaping () -> Date = Date.init) {
        var lines = responseLines
        self.init(lineInput: { lines.isEmpty ? nil : lines.removeFirst() }, now: now)
    }

    var sentMethods: [String] {
        condition.lock()
        defer { condition.unlock() }
        return recordedMethods
    }

    deinit {
        outputPipe?.fileHandleForReading.readabilityHandler = nil
        errorPipe?.fileHandleForReading.readabilityHandler = nil
        if process?.isRunning == true { process?.terminate() }
    }

    public func readRateLimits() throws -> QuotaSnapshot {
        let id = nextID
        nextID += 1
        try send(["method": "account/rateLimits/read", "id": id, "params": [:]])
        let response = try waitForResponse(id: id, timeout: 20)

        if let error = response["error"] as? [String: Any] {
            throw CompanionError.appServer("Codex 额度请求失败：\(error["message"] ?? error)")
        }
        guard let result = response["result"] as? [String: Any] else {
            throw CompanionError.malformedRateLimits("Codex 没有返回 rateLimits result")
        }

        let bucket: [String: Any]?
        if let buckets = result["rateLimitsByLimitId"] as? [String: Any],
           let codex = buckets["codex"] as? [String: Any] {
            bucket = codex
        } else if let legacy = result["rateLimits"] as? [String: Any],
                  legacy["limitId"] as? String == "codex" {
            bucket = legacy
        } else {
            bucket = nil
        }

        guard let primary = bucket?["primary"] as? [String: Any],
              let usedNumber = primary["usedPercent"] as? NSNumber,
              let resetNumber = primary["resetsAt"] as? NSNumber else {
            throw CompanionError.malformedRateLimits("没有找到 Codex primary 额度窗口")
        }
        let used = usedNumber.doubleValue
        let resetsAt = resetNumber.doubleValue
        guard CFGetTypeID(usedNumber) != CFBooleanGetTypeID(),
              CFGetTypeID(resetNumber) != CFBooleanGetTypeID(),
              used.isFinite, (0...100).contains(used),
              resetsAt.isFinite, resetsAt >= 0, resetsAt <= Double(Int.max) else {
            throw CompanionError.malformedRateLimits("Codex primary 额度窗口包含无效数值")
        }

        return QuotaSnapshot(
            remainingPercent: 100 - used,
            resetInSeconds: max(0, Int(resetsAt - now().timeIntervalSince1970))
        )
    }

    private func send(_ object: [String: Any]) throws {
        if let method = object["method"] as? String {
            condition.lock()
            recordedMethods.append(method)
            condition.unlock()
        }
        var data = try JSONSerialization.data(withJSONObject: object)
        if let lineOutput, let line = String(data: data, encoding: .utf8) {
            try lineOutput(line)
            return
        }
        guard let inputPipe else { return }
        data.append(0x0A)
        try inputPipe.fileHandleForWriting.write(contentsOf: data)
    }

    private func waitForResponse(id: Int, timeout: TimeInterval) throws -> [String: Any] {
        if let lineInput {
            while let line = try lineInput() {
                var data = Data(line.utf8)
                if data.last != 0x0A { data.append(0x0A) }
                responseParser.consume(data)
                if let response = responseParser.takeResponse(id: id) { return response }
                if let failure = responseParser.failure { throw failure }
            }
            responseParser.finish()
            if let response = responseParser.takeResponse(id: id) { return response }
            throw responseParser.failure ?? CompanionError.malformedRateLimits("Codex App Server 没有返回匹配的 rateLimits 响应")
        }

        guard let process else {
            throw CompanionError.appServer("Codex App Server transport 未配置")
        }
        let deadline = Date().addingTimeInterval(timeout)
        condition.lock()
        defer { condition.unlock() }
        while responseParser.failure == nil,
              !responseParser.hasResponse(id: id),
              process.isRunning,
              Date() < deadline {
            condition.wait(until: deadline)
        }
        if let response = responseParser.takeResponse(id: id) { return response }
        if let failure = responseParser.failure { throw failure }
        if !process.isRunning {
            responseParser.finish()
            if let response = responseParser.takeResponse(id: id) { return response }
            if let failure = responseParser.failure { throw failure }
        }
        let detail = stderrTail.isEmpty ? "无错误输出" : stderrTail.trimmingCharacters(in: .whitespacesAndNewlines)
        throw CompanionError.appServer("等待 Codex App Server 响应超时：\(detail)")
    }

    private func consume(_ data: Data) {
        condition.lock()
        responseParser.consume(data)
        condition.broadcast()
        condition.unlock()
    }
}
