# VibeWatch

**English** | [简体中文](README.zh-CN.md)

[![Firmware build](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange.svg)](https://platformio.org/)
[![macOS Companion](https://img.shields.io/badge/macOS-Swift%205.10-blue.svg)](https://swift.org/)
[![Tests](https://img.shields.io/badge/PyTest-Passing-brightgreen.svg)](tests/)
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
      Mint Cyan Theme · Weekly Quota Gauge · Live Reset Countdown
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
- **Strict Task Isolation**: 6-agent status rings, push-to-talk mic, and approval actions are strictly tagged with the active system context (e.g. `"c": "WORKBUDDY"`), preventing cross-agent crosstalk.

---

### 2. Human-in-the-Loop Approval & Anti-Flapping Protection

When an AI agent performs sensitive operations (reading files, executing scripts, running terminal commands), VibeWatch provides immediate tactile oversight:

<p align="center">
  <img src="docs/images/approval-modal.jpg" alt="Human-in-the-Loop Approval Modal" width="400"><br>
  <em>Prominent security card displayed when an agent requests confirmation</em>
</p>

- **Auto-Wake & Targeted Switch**: Automatically wakes the display, routes to the corresponding agent card, and triggers a dual-pulse vibration alarm and 1250Hz chime.
- **Operation Metadata**: Displays action type tags (`[ EXEC ]`, `[ SCRIPT ]`, `[ WRITE ]`) with safe UTF-8 multilingual text.
- **Physical Eyes-Free Decisions**:
  - 👉 **Right Button (BtnB) / Screen Right**: One-touch **OK (Approve / Execute)**
  - 👈 **Left Button (BtnA) / Screen Left**: One-touch **NG (Reject / Cancel)**
- **4.0s Anti-Flapping Protection Window**: Prevents subsequent background notifications from abruptly interrupting an ongoing user approval decision.

---

### 3. 6-Agent Subtask Ring & Push-to-Talk Voice Input
- **6 Outer Ring Nodes**: Track up to 6 concurrent parallel agent sessions or subtasks with real-time breathing/pulsing status indicators.
- **Center Push-to-Talk Mic**: Press and hold the center dial area (or the right physical button) to speak prompts directly to the active coding agent.

---

### 4. Dynamic CPU Frequency Scaling (DVFS) & Power Optimization
- **Active & Interaction Mode**: Locked at **`240MHz`** for smooth 60FPS animations and sub-millisecond BLE responsiveness.
- **60s Dimmed Mode**: Gracefully steps down to **`160MHz`** to reduce heat dissipation.
- **180s Screen Sleep**: Enters ultra-low-power **`80MHz`** standby, **reducing idle battery consumption by ~65%** while instantly restoring 240MHz on touch or button press.

---

## 🛠️ Hardware & Architecture

| Component | Specification / Role |
| :--- | :--- |
| **Controller** | [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3, 16MB Flash, 8MB PSRAM) |
| **Display** | 1.43-inch 466 × 466 Round AMOLED Capacitive Touchscreen |
| **Controls & Haptics** | 2 × Physical Buttons (BtnA / BtnB), Haptic Motor, Audio Buzzer Speaker |
| **Wireless Protocol** | Bluetooth Low Energy 5.0 (BLE HID Keyboard + Dedicated GATT Telemetry Channel) |
| **Host Environment** | macOS / Linux / Windows AI Workstation |

---

## 💻 Getting Started

### 1. Build and Flash Firmware

This project uses [PlatformIO](https://platformio.org/). Connect your M5Stack StopWatch via a USB-C cable:

```bash
# 1. Clone the repository
git clone https://github.com/neilshare/vibewatch.git
cd vibewatch

# 2. Build and upload firmware to the watch
platformio run --target upload
```

---

### 2. Bluetooth Pairing

1. Tap the **Settings** icon on the bottom-left of the watch screen.
2. Select a device slot (e.g. Slot 1) and tap **PAIR**.
3. Open Bluetooth settings on your computer and connect to `Vibe Watch #1`.

---

### 3. Host Companion & Telemetry CLI

#### Native macOS Swift Companion (Recommended)
A standalone Swift CLI utilizing macOS CoreBluetooth to ensure instant GATT synchronization even when connected as a HID keyboard:

```bash
cd companion && swift build

# Sync live rate limits to active card
./.build/debug/CodexWatchCompanion --card codex

# Sync Workbuddy credits balance
./.build/debug/CodexWatchCompanion --card workbuddy --credits 1250 --total-credits 1500

# Trigger a test approval dialog
./.build/debug/CodexWatchCompanion --approval --card workbuddy --type "SCRIPT" --summary "Run automated test suite"
```

#### Python CLI & Unit Tests
```bash
# Install dependencies
pip install pytest bleak

# Run automated unit test suite
pytest tests/

# Automatically sync local Codex quota
python scripts/sync_watch.py --auto
```

---

## 📜 License

This project is licensed under the [MIT License](LICENSE).
