# VibeWatch

**English** | [简体中文](README.zh-CN.md)

[![Firmware build](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange.svg)](https://platformio.org/)
[![Firmware Version](https://img.shields.io/badge/Firmware-v1.01-green.svg)](docs/release-notes-v1.01.md)
[![Protocol](https://img.shields.io/badge/Companion%20Protocol-v2-blue.svg)](docs/COMPANION_PROTOCOL.md)
[![macOS Companion](https://img.shields.io/badge/macOS-Swift%205.10-blue.svg)](companion/README.md)
[![Tests](https://img.shields.io/badge/Tests-100%25%20Passing-brightgreen.svg)](tests/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**A tactile wearable cockpit and control surface for multi-agent Vibe Coding — built around the M5Stack StopWatch (ESP32-S3 AMOLED).**

---

## 🌟 Key Features

### 1. Tri-Agent System Dashboard
Seamlessly navigate between three premier desktop AI coding agents with horizontal swipe gestures. Each agent operates with an independent telemetry data model, brand color theme, and custom metrics:

<table>
  <tr>
    <td width="33%" align="center">
      <img src="docs/images/card-codex.jpg" alt="OpenAI Codex Card"><br>
      <strong>🟢 1. OpenAI Codex</strong><br>
      Mint Cyan Theme · 360° Weekly Quota Gauge · Live Reset Countdown
    </td>
    <td width="33%" align="center">
      <img src="docs/images/card-workbuddy.jpg" alt="Tencent Workbuddy Card"><br>
      <strong>🔵 2. Tencent Workbuddy</strong><br>
      Azure Blue Theme · Dedicated Credit Balance · Quota Allocation
    </td>
    <td width="33%" align="center">
      <img src="docs/images/card-antigravity.jpg" alt="Google Antigravity Card"><br>
      <strong>🟣 3. Google Antigravity</strong><br>
      Galaxy Violet Theme · Monthly Rate & Context Telemetry
    </td>
  </tr>
</table>

- **Horizontal Swipe Navigation**: Swipe left or right across the screen with tactile haptic pulses and a crisp **1150Hz page chime**.
- **NVS Memory Persistence**: Automatically preserves and restores your active agent card across device reboots and power cycles.
- **Strict Task Isolation**: 6-agent status rings, push-to-talk HID control events, and approval actions are strictly tagged with the active system context (e.g. `"c": "WORKBUDDY"`), preventing cross-agent crosstalk.

---

### 2. Transactional Human-in-the-Loop Approval & Anti-Flapping Protection

When an AI agent performs sensitive operations (reading files, executing scripts, running terminal commands), VibeWatch provides immediate tactile oversight:

<p align="center">
  <img src="docs/images/approval-modal.jpg" alt="Human-in-the-Loop Approval Modal" width="400"><br>
  <em>Prominent security card displayed when an agent requests confirmation</em>
</p>

- **Transactional Protocol v2**: Every approval request and its decision share a canonical 36-byte lowercase UUID `request_id`, preventing race conditions and duplicated executions.
- **Security Trust Boundary**: Firmware approval characteristic (`.03`) strictly requires BLE encryption and a bonded peer, ensuring only trusted workstations can issue approval prompts.
- **Auto-Wake & Targeted Switch**: Automatically wakes the display, routes to the corresponding agent card, and triggers a dual-pulse vibration alarm and 1250Hz chime.
- **Operation Metadata**: Displays action type tags (`[ EXEC ]`, `[ SCRIPT ]`, `[ WRITE ]`) with safe UTF-8 multilingual text.
- **Physical Eyes-Free Decisions**:
  - 👉 **Right Button (BtnB) / Screen Right**: One-touch **OK (Approve / Execute)** with ascending audio chime.
  - 👈 **Left Button (BtnA) / Screen Left**: One-touch **NG (Reject / Cancel)** with descending audio chime.
- **4.0s Anti-Flapping Protection Window**: Prevents subsequent background notifications from abruptly interrupting an ongoing user approval decision.

---

### 3. 6-Agent Subtask Ring & Push-to-Talk Voice Control
- **6 Outer Ring Nodes**: Track up to 6 concurrent parallel agent sessions or subtasks with real-time breathing/pulsing status indicators.
- **Center Push-to-Talk Control**: Press and hold the center dial area (or the right physical button) to send a push-to-talk HID control event to the active host agent (the firmware does not capture or stream microphone audio, protecting privacy).

---

### 4. Dynamic CPU Frequency Scaling (DVFS) & Power Optimization
- **Active & Interaction Mode**: Locked at **`240MHz`** for smooth 60FPS animations and sub-millisecond BLE responsiveness.
- **60s Dimmed Mode**: Gracefully steps down to **`160MHz`** to reduce heat dissipation.
- **180s Screen Sleep**: Enters ultra-low-power **`80MHz`** standby, **reducing idle battery consumption by ~65%** while instantly restoring 240MHz on touch or button press.

---

## 🔒 Companion Protocol v2 Specification

VibeWatch advertises a private GATT telemetry service alongside standard BLE HID keyboard services:

| Role | UUID | GATT Behavior & Security | Constraint |
| :--- | :--- | :--- | :--- |
| **Private Service** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01` | Service discovery | — |
| **Quota Write (.02)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02` | Write with response; Encrypted ATT permission | Max 512 bytes UTF-8 JSON |
| **Approval Request (.03)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03` | Write with response; Encrypted + Bonded Peer required | Max 512 bytes, canonical `request_id` & `ttl_ms` |
| **Approval Result (.04)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04` | Indicate; Encrypted read permission | Returns `approved` / `rejected` / `expired` |

> See [docs/COMPANION_PROTOCOL.md](docs/COMPANION_PROTOCOL.md) for the complete byte-level wire contract and error codes.

---

## 🛠️ Hardware & Architecture

| Component | Specification / Role |
| :--- | :--- |
| **Controller** | [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3 Dual-Core 240MHz, 16MB Flash, 8MB PSRAM) |
| **Display** | 1.43-inch 466 × 466 Round AMOLED Capacitive Touchscreen |
| **Controls & Haptics** | 2 × Physical Buttons (BtnA / BtnB), Haptic Motor, Audio Buzzer Speaker |
| **Wireless Protocol** | Bluetooth Low Energy 5.0 (BLE HID Keyboard + Dedicated GATT Transactional Channel) |
| **Power Management** | Integrated LiPo Battery + AXP PMIC + DVFS Dynamic Frequency Scaling |
| **Host Environment** | macOS / Linux / Windows AI Workstations |

---

## 💻 Getting Started

### 1. Build and Flash Firmware

This project uses [PlatformIO](https://platformio.org/). Connect your M5Stack StopWatch via a USB-C cable:

```bash
# 1. Clone the repository
git clone https://github.com/neilshare/vibewatch.git
cd vibewatch

# 2. Build and upload firmware to the watch
platformio run -e m5stack-stopwatch --target upload
```

---

### 2. Bluetooth Pairing & Device Binding

1. Tap the **Settings** icon on the bottom-left of the watch screen.
2. Select a device slot (e.g. Slot 1) and tap **PAIR**.
3. Open Bluetooth settings on your Mac/PC and pair with `Vibe Watch #1`.
4. Note your host's CoreBluetooth UUID (in production, `--device-id` binds writes strictly to this device).

---

### 3. Host Companion & Telemetry CLI

#### Native macOS Swift Companion (Recommended)
A standalone Swift CLI utilizing macOS CoreBluetooth to ensure instant GATT synchronization even when connected as a HID keyboard:

```bash
# Build Swift companion tool
swift build --package-path companion -c release

# Synthetic demo discovery: discovers nearby watch, writes demo quota and prints device UUID
./companion/.build/release/codex-watch-companion --demo --verbose

# Sync real Codex rate limits to one pinned watch
./companion/.build/release/codex-watch-companion --auto --card codex \
  --device-id YOUR_COREBLUETOOTH_UUID

# Sync a manual Workbuddy credit balance to the pinned watch
./companion/.build/release/codex-watch-companion \
  --remaining 83.3 --reset 0 --card workbuddy \
  --credits 1250 --total-credits 1500 \
  --device-id YOUR_COREBLUETOOTH_UUID

# Send a correlated protocol-v2 approval request
./companion/.build/release/codex-watch-companion --approval \
  --card workbuddy --type SCRIPT --summary "Run automated test suite" \
  --device-id YOUR_COREBLUETOOTH_UUID
```

#### Python CLI & Scripts
```bash
# Install dependencies
pip install bleak pytest

# Automatically sync real local Codex quota
python3 scripts/sync_watch.py --auto --device-id YOUR_COREBLUETOOTH_UUID

# Explicit Python/Bleak v2 approval transport
python3 scripts/sync_watch.py --backend bleak --approval \
  --summary "Run automated test suite" \
  --device-id YOUR_COREBLUETOOTH_UUID
```

---

## 🧪 Comprehensive Automated Testing Suite

The repository features comprehensive automated test suites covering firmware, host drivers, and protocol contracts:

```bash
# 1. PlatformIO Native C++ Core Tests (27/27 test cases passing)
platformio test -e native

# 2. Swift Companion Core Verification (37/37 behavior groups passing)
swift run --package-path companion VibeWatchCompanionCoreVerification

# 3. Python / Bleak Protocol & CLI Tests (42/42 test cases passing)
pytest tests/
```

---

## 📚 References & Documentation

- [Authoritative Protocol v2 Specification](docs/COMPANION_PROTOCOL.md)
- [Hardware Acceptance Verification Criteria](docs/hardware-acceptance-v2.md)
- [Swift Companion Guide](companion/README.md)
- [v1.01 Release Notes](docs/release-notes-v1.01.md)
- [AI Code Review & Quality Audit Report](docs/code_review_report.md)

---

## 📜 License

This project is licensed under the [MIT License](LICENSE).
