import AppKit
import Darwin
import Foundation
import VibeWatchCompanionCore

setbuf(stdout, nil)
setbuf(stderr, nil)
_ = NSApplication.shared
NSApplication.shared.setActivationPolicy(.prohibited)

do {
    let options = try CompanionOptions.parse(CommandLine.arguments)
    let result = try Runner(writer: LegacyBLEWriter()).run(options: options) { output in
        print(output)
    }
    exit(Int32(result.exitCode))
} catch {
    fputs("错误：\(error.localizedDescription)\n", stderr)
    if case .usage = error as? CompanionError {
        exit(2)
    }
    exit(1)
}
