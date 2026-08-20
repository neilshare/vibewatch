import CoreBluetooth
import Foundation

public protocol BLETransporting: AnyObject {
    func writeQuota(_ snapshot: QuotaSnapshot, deviceID: UUID) throws
    func requestApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws -> ApprovalDecisionV2

    func discoverDemoDevices() throws -> [UUID]
    func writeLegacyApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws
    func enterBootloader(deviceID: UUID) throws
    func runBridge(deviceID: UUID?, defaultCard: AgentCard) throws
}

public extension BLETransporting {
    func discoverDemoDevices() throws -> [UUID] {
        throw BLETransportError.transport("Demo discovery is not supported by this transport.")
    }

    func writeLegacyApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws {
        throw BLETransportError.transport("Legacy approvals are not supported by this transport.")
    }

    func enterBootloader(deviceID: UUID) throws {
        throw BLETransportError.transport("Bootloader entry is not supported by this transport.")
    }

    func runBridge(deviceID: UUID?, defaultCard: AgentCard) throws {
        throw BLETransportError.transport("Bridge mode is not supported by this transport.")
    }
}

public enum BLETransportError: Error, Equatable, LocalizedError {
    case deviceNotFound
    case timeout
    case protocolError(code: String, message: String)
    case transport(String)

    public var errorDescription: String? {
        switch self {
        case .deviceNotFound:
            return "Pinned Bluetooth device was not found."
        case .timeout:
            return "Timed out waiting for a matching approval decision."
        case .protocolError(_, let message):
            return message
        case .transport(let message):
            return message
        }
    }

    public var code: String {
        switch self {
        case .deviceNotFound: return "device_not_found"
        case .protocolError(let code, _): return code
        case .timeout, .transport: return "transport_error"
        }
    }
}

public enum ApprovalIndicationV2: Equatable, Sendable {
    case decision(ApprovalDecisionV2)
    case protocolError(ApprovalProtocolErrorV2)
}

public final class ApprovalResultMatcher {
    private let requestID: UUID
    private var delivered = false
    private let decoder = JSONDecoder()

    public init(requestID: UUID) {
        self.requestID = requestID
    }

    public func receive(_ data: Data) -> ApprovalIndicationV2? {
        struct Envelope: Decodable {
            let version: Int
            let kind: String
        }
        guard !delivered,
              let envelope = try? decoder.decode(Envelope.self, from: data),
              envelope.version == 2 else { return nil }

        let indication: ApprovalIndicationV2
        switch envelope.kind {
        case "approval_decision":
            guard let decision = try? decoder.decode(ApprovalDecisionV2.self, from: data),
                  decision.requestID == requestID else { return nil }
            indication = .decision(decision)
        case "error":
            // Firmware cannot recover a request ID from queue-full or
            // undecodable payloads. This matcher exists for exactly one
            // in-flight transaction, so an empty ID is associated here only;
            // every nonempty ID must still match exactly.
            guard let error = try? decoder.decode(ApprovalProtocolErrorV2.self, from: data),
                  error.requestID == nil || error.requestID == requestID else { return nil }
            indication = .protocolError(error)
        default:
            return nil
        }
        delivered = true
        return indication
    }
}

public final class BLETransport: BLETransporting {
    public static let serviceUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C01")
    public static let quotaUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C02")
    public static let approvalUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C03")
    public static let approvalResultUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C04")

    private let verbose: Bool
    private let progress: (String) -> Void

    public init(verbose: Bool = false, progress: @escaping (String) -> Void = { _ in }) {
        self.verbose = verbose
        self.progress = progress
    }

    public func writeQuota(_ snapshot: QuotaSnapshot, deviceID: UUID) throws {
        let payload = try Self.encoder.encode(snapshot)
        _ = try PinnedBLESession(
            deviceID: deviceID,
            operation: .quota(payload),
            verbose: verbose,
            progress: progress
        ).execute(timeout: 40)
    }

    public func requestApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws -> ApprovalDecisionV2 {
        let payload = try Self.encoder.encode(request)
        guard let decision = try PinnedBLESession(
            deviceID: deviceID,
            operation: .approval(payload, request.requestID),
            verbose: verbose,
            progress: progress
        ).execute(timeout: TimeInterval(request.ttlMs) / 1_000 + 5) else {
            throw BLETransportError.transport("Approval transaction ended without a decision.")
        }
        return decision
    }

    public func discoverDemoDevices() throws -> [UUID] {
        try DemoDiscoverySession(verbose: verbose, progress: progress).discover(timeout: 5)
    }

    public func writeLegacyApproval(_ request: ApprovalRequestV2, deviceID: UUID) throws {
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
        let payload = try Self.encoder.encode(LegacyApproval(params: .init(
            type: request.operationType,
            summary: request.summary,
            card: request.card.rawValue
        )))
        _ = try PinnedBLESession(
            deviceID: deviceID,
            operation: .legacyApproval(payload),
            verbose: verbose,
            progress: progress
        ).execute(timeout: 40)
    }

    public func enterBootloader(deviceID: UUID) throws {
        let payload = Data(#"{"op":"enter_bootloader","version":1,"confirm":true}"#.utf8)
        _ = try PinnedBLESession(
            deviceID: deviceID,
            operation: .bootloader(payload),
            verbose: verbose,
            progress: progress
        ).execute(timeout: 40)
    }

    public func runBridge(deviceID: UUID?, defaultCard: AgentCard) throws {
        try BridgeBLESession(
            targetDeviceID: deviceID,
            defaultCard: defaultCard,
            verbose: verbose,
            progress: progress
        ).run()
    }

    private static var encoder: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return encoder
    }
}

private enum PinnedOperation {
    case quota(Data)
    case approval(Data, UUID)
    case legacyApproval(Data)
    case bootloader(Data)

    var payload: Data {
        switch self {
        case .quota(let data), .approval(let data, _), .legacyApproval(let data), .bootloader(let data):
            return data
        }
    }

    var characteristicUUIDs: [CBUUID] {
        switch self {
        case .quota, .bootloader:
            return [BLETransport.quotaUUID]
        case .approval:
            return [BLETransport.approvalUUID, BLETransport.approvalResultUUID]
        case .legacyApproval:
            return [BLETransport.approvalUUID]
        }
    }
}

private final class PinnedBLESession: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private enum State {
        case idle
        case discovering
        case connecting
        case discoveringServices
        case discoveringCharacteristics
        case enablingIndications
        case writing
        case awaitingDecision
        case awaitingBootloaderDisconnect
        case finished
    }

    private let deviceID: UUID
    private let operation: PinnedOperation
    private let verbose: Bool
    private let progress: (String) -> Void
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var state = State.idle
    private var result: Result<ApprovalDecisionV2?, Error>?
    private var matcher: ApprovalResultMatcher?
    private var writeAcknowledged = false
    private var pendingDecision: ApprovalDecisionV2?

    init(deviceID: UUID, operation: PinnedOperation, verbose: Bool, progress: @escaping (String) -> Void) {
        self.deviceID = deviceID
        self.operation = operation
        self.verbose = verbose
        self.progress = progress
        if case .approval(_, let requestID) = operation {
            matcher = ApprovalResultMatcher(requestID: requestID)
        }
        super.init()
    }

    @discardableResult
    func execute(timeout: TimeInterval) throws -> ApprovalDecisionV2? {
        guard operation.payload.count <= 512 else {
            throw BLETransportError.transport("BLE payload exceeds the 512-byte firmware limit.")
        }
        manager = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
        let deadline = Date().addingTimeInterval(timeout)
        while result == nil, Date() < deadline {
            RunLoop.current.run(mode: .default, before: min(deadline, Date().addingTimeInterval(0.05)))
        }
        if result == nil {
            if peripheral == nil {
                finish(.failure(BLETransportError.deviceNotFound))
            } else {
                finish(.failure(BLETransportError.timeout))
            }
        }
        guard let result else {
            throw BLETransportError.transport("Bluetooth operation ended without a result.")
        }
        return try result.get()
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            guard state == .idle else { return }
            state = .discovering
            let retrieved = central.retrievePeripherals(withIdentifiers: [deviceID])
                .first(where: { $0.identifier == deviceID })
            if let retrieved {
                use(retrieved, central: central)
            } else {
                log("Scanning only for pinned peripheral \(deviceID.uuidString.lowercased()).")
                central.scanForPeripherals(withServices: [BLETransport.serviceUUID])
            }
        case .unauthorized:
            finish(.failure(BLETransportError.transport("Bluetooth access is not authorized.")))
        case .unsupported:
            finish(.failure(BLETransportError.transport("CoreBluetooth is not supported on this Mac.")))
        case .poweredOff:
            finish(.failure(BLETransportError.transport("Bluetooth is powered off.")))
        case .resetting, .unknown:
            break
        @unknown default:
            finish(.failure(BLETransportError.transport("Unknown CoreBluetooth state.")))
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover discovered: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard state == .discovering, discovered.identifier == deviceID else { return }
        use(discovered, central: central)
    }

    func centralManager(_ central: CBCentralManager, didConnect connected: CBPeripheral) {
        guard connected.identifier == deviceID, connected === peripheral else {
            central.cancelPeripheralConnection(connected)
            return
        }
        state = .discoveringServices
        log("Connected to pinned peripheral; discovering private service.")
        connected.discoverServices([BLETransport.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect failed: CBPeripheral, error: Error?) {
        guard failed.identifier == deviceID else { return }
        finish(.failure(BLETransportError.transport(errorMessage("Could not connect to pinned peripheral", error))))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral disconnected: CBPeripheral, error: Error?) {
        guard disconnected === peripheral, state != .finished else { return }
        if state == .awaitingBootloaderDisconnect {
            finish(.success(nil))
        } else {
            finish(.failure(BLETransportError.transport(errorMessage("Peripheral disconnected before completion", error))))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            finish(.failure(BLETransportError.transport(errorMessage("Service discovery failed", error))))
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == BLETransport.serviceUUID }) else {
            finish(.failure(BLETransportError.transport("Pinned peripheral does not expose the private service.")))
            return
        }
        state = .discoveringCharacteristics
        peripheral.discoverCharacteristics(operation.characteristicUUIDs, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error {
            finish(.failure(BLETransportError.transport(errorMessage("Characteristic discovery failed", error))))
            return
        }
        let characteristics = service.characteristics ?? []
        switch operation {
        case .quota, .bootloader:
            guard let writable = characteristics.first(where: { $0.uuid == BLETransport.quotaUUID }) else {
                finish(.failure(BLETransportError.transport("Quota characteristic is missing.")))
                return
            }
            write(operation.payload, to: writable, on: peripheral)
        case .legacyApproval:
            guard let writable = characteristics.first(where: { $0.uuid == BLETransport.approvalUUID }) else {
                finish(.failure(BLETransportError.transport("Approval characteristic is missing.")))
                return
            }
            write(operation.payload, to: writable, on: peripheral)
        case .approval:
            guard let writable = characteristics.first(where: { $0.uuid == BLETransport.approvalUUID }),
                  let resultCharacteristic = characteristics.first(where: { $0.uuid == BLETransport.approvalResultUUID }) else {
                finish(.failure(BLETransportError.transport("Approval request/result characteristics are missing.")))
                return
            }
            guard resultCharacteristic.properties.contains(.indicate) else {
                finish(.failure(BLETransportError.transport("Approval result characteristic does not support indications.")))
                return
            }
            guard writable.properties.contains(.write) else {
                finish(.failure(BLETransportError.transport("Approval request requires write-with-response support.")))
                return
            }
            state = .enablingIndications
            log("Enabling approval-result indications before writing request.")
            peripheral.setNotifyValue(true, for: resultCharacteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLETransport.approvalResultUUID, state == .enablingIndications else { return }
        if let error {
            finish(.failure(BLETransportError.transport(errorMessage("Could not enable approval-result indications", error))))
            return
        }
        guard characteristic.isNotifying else {
            finish(.failure(BLETransportError.transport("Approval-result indication subscription was not confirmed.")))
            return
        }
        guard case .approval(let payload, _) = operation,
              let service = characteristic.service,
              let writable = service.characteristics?.first(where: { $0.uuid == BLETransport.approvalUUID }) else {
            finish(.failure(BLETransportError.transport("Approval request characteristic became unavailable.")))
            return
        }
        write(payload, to: writable, on: peripheral)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            finish(.failure(BLETransportError.transport(errorMessage("ATT write failed", error))))
            return
        }
        writeAcknowledged = true
        switch operation {
        case .approval:
            state = .awaitingDecision
            log("Approval request received ATT acknowledgement; waiting for correlated decision.")
            if let pendingDecision { finish(.success(pendingDecision)) }
        case .bootloader:
            state = .awaitingBootloaderDisconnect
            log("Bootloader command received ATT acknowledgement; waiting for reboot disconnect.")
        case .quota, .legacyApproval:
            // Legacy approval intentionally ends at the .03 ATT acknowledgement.
            finish(.success(nil))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLETransport.approvalResultUUID else { return }
        if let error {
            finish(.failure(BLETransportError.transport(errorMessage("Approval-result indication failed", error))))
            return
        }
        guard let data = characteristic.value, let indication = matcher?.receive(data) else { return }
        switch indication {
        case .decision(let decision):
            pendingDecision = decision
            if writeAcknowledged { finish(.success(decision)) }
        case .protocolError(let error):
            finish(.failure(BLETransportError.protocolError(code: error.code, message: error.message)))
        }
    }

    private func use(_ selected: CBPeripheral, central: CBCentralManager) {
        guard selected.identifier == deviceID, peripheral == nil else { return }
        peripheral = selected
        selected.delegate = self
        central.stopScan()
        state = .connecting
        if selected.state == .connected {
            state = .discoveringServices
            selected.discoverServices([BLETransport.serviceUUID])
        } else {
            central.connect(selected)
        }
    }

    private func write(_ data: Data, to characteristic: CBCharacteristic, on peripheral: CBPeripheral) {
        guard characteristic.properties.contains(.write) else {
            finish(.failure(BLETransportError.transport("Characteristic requires write-with-response support.")))
            return
        }
        state = .writing
        log("Writing \(data.count) bytes with ATT response.")
        peripheral.writeValue(data, for: characteristic, type: .withResponse)
    }

    private func finish(_ result: Result<ApprovalDecisionV2?, Error>) {
        guard self.result == nil else { return }
        self.result = result
        state = .finished
        manager?.stopScan()
        if let peripheral { manager?.cancelPeripheralConnection(peripheral) }
    }

    private func log(_ message: String) {
        if verbose { progress(message) }
    }

    private func errorMessage(_ prefix: String, _ error: Error?) -> String {
        guard let error else { return "\(prefix)." }
        let value = error as NSError
        return "\(prefix): \(value.domain) code \(value.code)."
    }
}

private final class DemoDiscoverySession: NSObject, CBCentralManagerDelegate {
    private let verbose: Bool
    private let progress: (String) -> Void
    private var manager: CBCentralManager!
    private var identifiers = Set<UUID>()
    private var failure: Error?

    init(verbose: Bool, progress: @escaping (String) -> Void) {
        self.verbose = verbose
        self.progress = progress
    }

    func discover(timeout: TimeInterval) throws -> [UUID] {
        manager = CBCentralManager(delegate: self, queue: nil)
        let deadline = Date().addingTimeInterval(timeout)
        while failure == nil, Date() < deadline {
            RunLoop.current.run(mode: .default, before: min(deadline, Date().addingTimeInterval(0.05)))
        }
        manager.stopScan()
        if let failure { throw failure }
        return identifiers.sorted { $0.uuidString < $1.uuidString }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if verbose { progress("Demo discovery is scanning broadly by service/name.") }
            let connected = central.retrieveConnectedPeripherals(withServices: [
                BLETransport.serviceUUID,
                CBUUID(string: "1812"),
                CBUUID(string: "180F")
            ])
            for p in connected {
                let name = p.name ?? ""
                if name.starts(with: "Vibe Watch") || name.starts(with: "StopWatch") || name == "Codex Micro" {
                    identifiers.insert(p.identifier)
                    if verbose { progress("Found already-connected device \(name) [\(p.identifier.uuidString)]") }
                }
            }
            central.scanForPeripherals(withServices: nil)
        case .unauthorized:
            failure = BLETransportError.transport("Bluetooth access is not authorized.")
        case .unsupported:
            failure = BLETransportError.transport("CoreBluetooth is not supported on this Mac.")
        case .poweredOff:
            failure = BLETransportError.transport("Bluetooth is powered off.")
        case .resetting, .unknown:
            break
        @unknown default:
            failure = BLETransportError.transport("Unknown CoreBluetooth state.")
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertised = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        guard advertised.contains(BLETransport.serviceUUID)
                || name.starts(with: "Vibe Watch")
                || name.starts(with: "StopWatch")
                || name == "Codex Micro" else { return }
        identifiers.insert(peripheral.identifier)
        if verbose { progress("Demo found \(name.isEmpty ? "unnamed device" : name) RSSI=\(RSSI).") }
    }
}

import AppKit

private final class BridgeBLESession: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let targetDeviceID: UUID?
    private let defaultCard: AgentCard
    private let verbose: Bool
    private let progress: (String) -> Void

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var isRunning = true

    init(targetDeviceID: UUID?, defaultCard: AgentCard, verbose: Bool, progress: @escaping (String) -> Void) {
        self.targetDeviceID = targetDeviceID
        self.defaultCard = defaultCard
        self.verbose = verbose
        self.progress = progress
        super.init()
    }

    func run() throws {
        central = CBCentralManager(delegate: self, queue: nil)
        progress("VibeWatch Native macOS Bridge is running. Listening for events...")
        while isRunning {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.25))
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else { return }
        progress("Bluetooth is powered on. Discovering VibeWatch...")

        // 1. Check connected devices first
        let connected = central.retrieveConnectedPeripherals(withServices: [
            BLETransport.serviceUUID,
            CBUUID(string: "1812"),
            CBUUID(string: "180F")
        ])
        for p in connected {
            let name = p.name ?? ""
            if let target = targetDeviceID, p.identifier == target {
                connect(p)
                return
            } else if targetDeviceID == nil && (name.starts(with: "Vibe Watch") || name.starts(with: "StopWatch") || name == "Codex Micro") {
                connect(p)
                return
            }
        }

        // 2. Scan if not found in connected list
        central.scanForPeripherals(withServices: nil)
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        if let target = targetDeviceID, peripheral.identifier == target {
            central.stopScan()
            connect(peripheral)
        } else if targetDeviceID == nil && (name.starts(with: "Vibe Watch") || name.starts(with: "StopWatch") || name == "Codex Micro") {
            central.stopScan()
            connect(peripheral)
        }
    }

    private func connect(_ p: CBPeripheral) {
        self.peripheral = p
        p.delegate = self
        progress("Connecting to \(p.name ?? "Device") [\(p.identifier.uuidString)]...")
        if p.state == .connected {
            p.discoverServices([BLETransport.serviceUUID])
        } else {
            central.connect(p, options: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        progress("Connected to VibeWatch! Discovering private GATT services...")
        peripheral.discoverServices([BLETransport.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        progress("VibeWatch disconnected. Waiting for reconnect...")
        central.scanForPeripherals(withServices: nil)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for s in services where s.uuid == BLETransport.serviceUUID {
            peripheral.discoverCharacteristics([BLETransport.approvalResultUUID], for: s)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for c in characteristics where c.uuid == BLETransport.approvalResultUUID {
            peripheral.setNotifyValue(true, for: c)
            progress("Subscribed to VibeWatch hardware event stream! Ready for PTT & Keys.")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLETransport.approvalResultUUID, let data = characteristic.value else { return }
        guard let text = String(data: data, encoding: .utf8) else { return }

        // Parse JSON event: {"m":"v.oai.hid","p":{"k":"AG01","act":1,"c":"ANTIGRAVITY"}}
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let params = json["p"] as? [String: Any],
              let key = params["k"] as? String else {
            if verbose { progress("Raw event: \(text)") }
            return
        }

        let act = (params["act"] as? Int) ?? 0
        let card = (params["c"] as? String) ?? defaultCard.rawValue
        let pressed = (act == 1)

        handleHardwareEvent(key: key, act: act, card: card, pressed: pressed)
    }

    private func handleHardwareEvent(key: String, act: Int, card: String, pressed: Bool) {
        let cardUpper = card.uppercased()
        let targetApp: String
        if cardUpper.contains("ANTI") || cardUpper.contains("GRAVITY") {
            targetApp = "Antigravity"
        } else if cardUpper.contains("BUDDY") {
            targetApp = "Workbuddy"
        } else {
            targetApp = "ChatGPT"
        }

        if key.starts(with: "AG") && pressed {
            progress(">>> [SLOT KEY \(key)] Card: \(card) -> Activating \(targetApp)...")
            activateApp(targetApp)
        } else if key == "ACT10" { // PTT Start
            progress(">>> [PTT START] Activating \(targetApp) & Triggering Doubao Voice Input...")
            activateApp(targetApp)
        } else if key == "ACT11" { // PTT End
            progress(">>> [PTT END] Voice input finished into \(targetApp) Prompt.")
        } else if key == "ACT07" || key == "ACT08" {
            let decision = (key == "ACT07") ? "APPROVE (OK)" : "REJECT (NG)"
            progress(">>> [HARDWARE APPROVAL] Decision: \(decision) for \(card)")
        }
    }

    private func activateApp(_ name: String) {
        DispatchQueue.main.async {
            for app in NSWorkspace.shared.runningApplications {
                if let appName = app.localizedName, appName.localizedCaseInsensitiveContains(name) {
                    app.activate(options: [.activateIgnoringOtherApps, .activateAllWindows])
                    return
                }
            }
        }
    }
}
