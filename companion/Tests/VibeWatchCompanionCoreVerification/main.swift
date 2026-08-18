import Darwin
import VibeWatchCompanionCoreTestSupport

do {
    let count = try Task4Scenarios.runAll()
    print("PASS: \(count) Task 4 behavior groups")
} catch {
    print("FAIL: \(error)")
    exit(1)
}
