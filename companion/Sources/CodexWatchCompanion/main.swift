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
    let reportProgress: (String) -> Void = { message in
        fputs("\(message)\n", stderr)
    }
    let transport = BLETransport(verbose: options.verbose, progress: reportProgress)
    let result = try Runner(transport: transport, progress: reportProgress).run(options: options) { output in
        print(output)
    }
    if !result.stdout.isEmpty { print(result.stdout) }
    exit(Int32(result.exitCode))
} catch {
    let isUsage: Bool
    if case .usage = error as? CompanionError { isUsage = true } else { isUsage = false }
    let result = MachineOutput.errorResult(for: error, exitCode: isUsage ? 2 : 1)
    print(result.stdout)
    exit(Int32(result.exitCode))
}
