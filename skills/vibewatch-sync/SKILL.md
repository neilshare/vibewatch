---
name: vibewatch-sync
description: "Sync real-time agent quotas, credits, and telemetry from Workbuddy, AntiGravity, and OpenAI Codex to VibeWatch hardware via BLE. Use to trigger one-time sync, schedule background cron (every 10 minutes), or install the persistent macOS LaunchAgent daemon."
risk: low
source: community
date_added: "2026-08-18"
---

# VibeWatch Sync Skill

Automated telemetry, quota, and credit balance synchronization between AI Agent hosts (Workbuddy, AntiGravity, OpenAI Codex) and the VibeWatch hardware device (M5Stack StopWatch) over Bluetooth Low Energy (BLE).

## Quick Workflows

### 1. One-Time Telemetry Sync

Run a single immediate push across all 3 agent cards (Workbuddy credits, AntiGravity quota, Codex rate limits):

```bash
python3 /Users/zhangneil/github/vibewatch/scripts/vibewatch_daemon.py --once
```

If you know your watch's CoreBluetooth UUID:
```bash
python3 /Users/zhangneil/github/vibewatch/scripts/vibewatch_daemon.py --once --device-id <WATCH_UUID>
```

### 2. Manual Custom Quota or Credits Push

```bash
# Push Workbuddy Credit Balance (e.g. 1250 credits / 1500 total)
/Users/zhangneil/github/vibewatch/companion/.build/release/codex-watch-companion \
  --card workbuddy \
  --credits 1250 --total-credits 1500 \
  --remaining 83.3 --reset 0 \
  --device-id <WATCH_UUID>

# Push AntiGravity Monthly Quota (e.g. 100% remaining, resets in 3 days)
/Users/zhangneil/github/vibewatch/companion/.build/release/codex-watch-companion \
  --card antigravity \
  --remaining 100.0 --reset 259200 \
  --device-id <WATCH_UUID>
```

### 3. Install macOS Background Daemon (Auto-Runs Every 10 Minutes)

To automatically sync every 10 minutes in the background upon macOS login:

```bash
python3 /Users/zhangneil/github/vibewatch/scripts/vibewatch_daemon.py --install-launchd --interval 600
```

To monitor daemon logs:
```bash
tail -f /tmp/vibewatch_sync.log
```

To stop or uninstall the LaunchAgent:
```bash
launchctl unload ~/Library/LaunchAgents/com.vibewatch.sync.plist
rm ~/Library/LaunchAgents/com.vibewatch.sync.plist
```

### 4. Periodic Cron Schedule via Agent (/schedule)

In AntiGravity or Workbuddy, you can schedule a recurring 10-minute cron check:

- **Cron Expression**: `*/10 * * * *`
- **Command**: `python3 /Users/zhangneil/github/vibewatch/scripts/vibewatch_daemon.py --once`
