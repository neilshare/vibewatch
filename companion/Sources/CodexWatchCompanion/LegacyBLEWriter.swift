import CoreBluetooth
import Foundation
import VibeWatchCompanionCore

private let quotaServiceUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C01")
private let quotaWriteUUID = CBUUID(string: "7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C02")
private let hidServiceUUID = CBUUID(string: "1812")

private enum BLEWritePurpose {
    case quota
    case enterBootloader
}
private enum BLEWriteOutcome {
    case completed
    case commandAcknowledgedAndDisconnected
}

private final class BLEQuotaWriter: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private let payload: Data
    private let verbose: Bool
    private let expectedIdentifier: UUID?
    private let purpose: BLEWritePurpose
    private var finished = false
    private var result: Result<BLEWriteOutcome, Error>?
    private var pendingPeripherals: [CBPeripheral] = []
    private var rejectedIdentifiers = Set<UUID>()
    private var writeAcknowledgedAt: Date?

    init(
        payload: Data,
        verbose: Bool,
        expectedIdentifier: UUID?,
        purpose: BLEWritePurpose = .quota
    ) {
        self.payload = payload
        self.verbose = verbose
        self.expectedIdentifier = expectedIdentifier
        self.purpose = purpose
        super.init()
    }

    func write(timeout: TimeInterval = 40) throws -> BLEWriteOutcome {
        guard payload.count <= 512 else {
            throw CompanionError.bluetooth("额度 payload 超过固件的 512-byte 上限")
        }
        manager = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )

        let discoveryDeadline = Date().addingTimeInterval(timeout)
        while !finished {
            let now = Date()
            if let writeAcknowledgedAt {
                if now.timeIntervalSince(writeAcknowledgedAt) >= 5.0 {
                    finish(.failure(CompanionError.bluetooth(
                        "bootloader 命令已获 ATT ACK，但设备在 5 秒内没有断开；未确认进入下载模式"
                    )))
                    continue
                }
            } else if now >= discoveryDeadline {
                break
            }
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
        }
        if !finished {
            if let peripheral {
                manager.cancelPeripheralConnection(peripheral)
            }
            throw CompanionError.bluetooth("未在 \(Int(timeout)) 秒内发现带有专属额度服务的 StopWatch")
        }
        guard let result else {
            throw CompanionError.bluetooth("BLE 写入没有返回结果")
        }
        return try result.get()
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            // A bonded HOGP peripheral can already be connected by macOS while
            // CoreBluetooth has never cached this app's private quota service.
            // In that state it stops advertising and therefore cannot be found
            // by a scan or retrieveConnectedPeripherals(withServices:). An
            // explicitly pinned CoreBluetooth identifier is still retrievable;
            // connect to that exact object and discover the service directly.
            let pinned = expectedIdentifier.map {
                central.retrievePeripherals(withIdentifiers: [$0])
            } ?? []
            let quotaConnected = central.retrieveConnectedPeripherals(withServices: [quotaServiceUUID])
            let hidConnected = central.retrieveConnectedPeripherals(withServices: [hidServiceUUID])
            pendingPeripherals = uniquePeripherals(pinned + quotaConnected + hidConnected)
                .filter(matchesExpectedDevice)
            if let peripheral = nextPendingPeripheral() {
                if verbose {
                    if purpose == .enterBootloader {
                        print("BLE 已开启；找到已绑定的 StopWatch…")
                    } else {
                        print("BLE 已开启；找到已连接的 StopWatch [\(peripheral.identifier)]…")
                    }
                }
                use(peripheral, with: central)
            } else {
                if verbose {
                    if let expectedIdentifier {
                        if purpose == .enterBootloader {
                            print("BLE 已开启；只扫描已绑定的 StopWatch…")
                        } else {
                            print("BLE 已开启；只扫描已绑定的 StopWatch [\(expectedIdentifier)]…")
                        }
                    } else {
                        print("BLE 已开启；扫描 StopWatch 专属服务（demo 发现模式）…")
                    }
                }
                // Scan broadly because a bonded HID connection can retain an
                // older service cache after firmware gains the private quota
                // service. Candidate names/UUIDs are checked before connect,
                // and the service itself is verified before any write.
                central.scanForPeripherals(withServices: nil)
            }
        case .unauthorized:
            finish(.failure(CompanionError.bluetooth("macOS 未授权此程序使用蓝牙")))
        case .unsupported:
            finish(.failure(CompanionError.bluetooth("这台 Mac 不支持 CoreBluetooth")))
        case .poweredOff:
            finish(.failure(CompanionError.bluetooth("Mac 蓝牙当前已关闭")))
        case .resetting, .unknown:
            break
        @unknown default:
            finish(.failure(CompanionError.bluetooth("未知蓝牙状态：\(central.state.rawValue)")))
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard self.peripheral == nil else { return }
        guard matchesExpectedDevice(peripheral) else {
            if verbose {
                if purpose == .enterBootloader {
                    print("忽略未绑定的 peripheral")
                } else {
                    print("忽略未绑定的 peripheral [\(peripheral.identifier)]")
                }
            }
            return
        }
        if expectedIdentifier == nil {
            let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
            guard name.starts(with: "Vibe Watch") || name.starts(with: "StopWatch") || name == "Codex Micro" else { return }
        }
        guard !rejectedIdentifiers.contains(peripheral.identifier) else { return }
        if verbose {
            let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? "未命名设备"
            if purpose == .enterBootloader {
                print("发现 \(name) RSSI=\(RSSI)")
            } else {
                print("发现 \(name) [\(peripheral.identifier)] RSSI=\(RSSI)")
            }
        }
        use(peripheral, with: central)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        if verbose { print("BLE 已连接；发现额度服务…") }
        peripheral.discoverServices([quotaServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        if expectedIdentifier == nil {
            rejectAndContinue(peripheral, reason: "连接失败：\(safeErrorDescription(error))")
        } else {
            finish(.failure(CompanionError.bluetooth("连接 StopWatch 失败：\(safeErrorDescription(error))")))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            finish(.failure(CompanionError.bluetooth("发现 StopWatch 服务失败：\(safeErrorDescription(error))")))
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == quotaServiceUUID }) else {
            if expectedIdentifier == nil {
                rejectAndContinue(peripheral, reason: "没有 StopWatch 专属额度服务")
            } else {
                finish(.failure(CompanionError.bluetooth("已绑定设备没有暴露预期的额度服务")))
            }
            return
        }
        peripheral.discoverCharacteristics([quotaWriteUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error {
            finish(.failure(CompanionError.bluetooth("发现额度写入特征失败：\(safeErrorDescription(error))")))
            return
        }
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == quotaWriteUUID }) else {
            finish(.failure(CompanionError.bluetooth("设备没有暴露预期的额度写入特征")))
            return
        }

        if characteristic.properties.contains(.write) {
            if verbose {
                let label = purpose == .enterBootloader ? "bootloader 命令" : "额度快照"
                print("写入 \(payload.count) bytes \(label)…")
            }
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else if characteristic.properties.contains(.writeWithoutResponse) {
            guard purpose == .quota else {
                finish(.failure(CompanionError.bluetooth("bootloader 命令要求带响应的 ATT 写入")))
                return
            }
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
            finish(.success(.completed))
        } else {
            finish(.failure(CompanionError.bluetooth("额度特征不可写")))
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            finish(.failure(CompanionError.bluetooth("BLE 写入失败：\(safeErrorDescription(error))")))
        } else if purpose == .enterBootloader {
            writeAcknowledgedAt = Date()
            if verbose { print("bootloader 命令已收到 ATT ACK；等待设备重启…") }
        } else {
            finish(.success(.completed))
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard self.peripheral === peripheral else { return }
        if purpose == .enterBootloader, writeAcknowledgedAt != nil {
            finish(.success(.commandAcknowledgedAndDisconnected))
            return
        }
        guard !finished else { return }
        finish(.failure(CompanionError.bluetooth(
            "StopWatch 在写入完成前断开：\(safeErrorDescription(error, fallback: "连接已关闭"))"
        )))
    }

    private func finish(_ result: Result<BLEWriteOutcome, Error>) {
        guard !finished else { return }
        self.result = result
        finished = true
        manager?.stopScan()
        if let peripheral {
            manager?.cancelPeripheralConnection(peripheral)
        }
    }

    private func use(_ peripheral: CBPeripheral, with central: CBCentralManager) {
        guard self.peripheral == nil else { return }
        self.peripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        if verbose { print("CoreBluetooth peripheral state=\(peripheral.state.rawValue)") }
        if peripheral.state == .connected {
            if verbose { print("BLE 已由 macOS 连接；直接发现额度服务…") }
            peripheral.discoverServices([quotaServiceUUID])
            return
        }
        // retrieveConnectedPeripherals reports a system-level link. This
        // process still calls connect so CoreBluetooth establishes app-level
        // ownership and delivers didConnect before service discovery.
        central.connect(peripheral)
    }

    private func matchesExpectedDevice(_ peripheral: CBPeripheral) -> Bool {
        guard let expectedIdentifier else { return true }
        return peripheral.identifier == expectedIdentifier
    }

    private func safeErrorDescription(_ error: Error?, fallback: String = "未知错误") -> String {
        guard let error else { return fallback }
        if purpose == .enterBootloader {
            // NSError domain/code preserves actionable CoreBluetooth context
            // without echoing an arbitrary localized string that could embed
            // the bound peripheral identifier.
            let cocoaError = error as NSError
            return "\(cocoaError.domain) code \(cocoaError.code)"
        }
        return error.localizedDescription
    }

    private func uniquePeripherals(_ peripherals: [CBPeripheral]) -> [CBPeripheral] {
        var seen = Set<UUID>()
        return peripherals.filter { seen.insert($0.identifier).inserted }
    }

    private func nextPendingPeripheral() -> CBPeripheral? {
        while !pendingPeripherals.isEmpty {
            let candidate = pendingPeripherals.removeFirst()
            if !rejectedIdentifiers.contains(candidate.identifier) {
                return candidate
            }
        }
        return nil
    }

    private func rejectAndContinue(_ rejected: CBPeripheral, reason: String) {
        if verbose {
            if purpose == .enterBootloader {
                print("跳过候选设备：\(reason)")
            } else {
                print("跳过 [\(rejected.identifier)]：\(reason)")
            }
        }
        rejectedIdentifiers.insert(rejected.identifier)
        manager.cancelPeripheralConnection(rejected)
        peripheral = nil
        if let next = nextPendingPeripheral() {
            use(next, with: manager)
        } else {
            manager.scanForPeripherals(withServices: nil)
        }
    }
}




final class LegacyBLEWriter: BLEWriting {
    func writeQuota(_ payload: Data, deviceID: UUID?, verbose: Bool) throws {
        _ = try BLEQuotaWriter(
            payload: payload,
            verbose: verbose,
            expectedIdentifier: deviceID
        ).write()
    }

    func writeApproval(_ payload: Data, deviceID: UUID, verbose: Bool) throws {
        _ = try BLEQuotaWriter(
            payload: payload,
            verbose: verbose,
            expectedIdentifier: deviceID
        ).write()
    }

    func enterBootloader(deviceID: UUID, verbose: Bool) throws {
        let payload = Data(#"{"op":"enter_bootloader","version":1,"confirm":true}"#.utf8)
        let outcome = try BLEQuotaWriter(
            payload: payload,
            verbose: verbose,
            expectedIdentifier: deviceID,
            purpose: .enterBootloader
        ).write()
        guard case .commandAcknowledgedAndDisconnected = outcome else {
            throw CompanionError.bluetooth("bootloader 命令返回了意外的写入结果")
        }
    }
}
