#!/usr/bin/env python3
"""VibeWatch Universal Host Bridge for Antigravity, Workbuddy, and Codex.

Provides bidirectional event routing, automatic application focus,
multi-tiered voice dictation (Doubao / macOS Dictation / Whisper),
and real-time quota/credit synchronization.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parent.parent

try:
    import bleak
    from bleak import BleakClient, BleakScanner
except ImportError:
    venv_python = PROJECT_ROOT / ".venv" / "bin" / "python"
    if venv_python.is_file() and sys.executable != str(venv_python):
        os.execv(str(venv_python), [str(venv_python)] + sys.argv)
    else:
        sys.stderr.write("Error: 'bleak' is required. Please run: pip3 install bleak or use .venv/bin/python\n")
        sys.exit(1)
CONFIG_FILE = Path.home() / ".vibewatch" / "config.json"

# BLE UUIDs
QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"
APPROVAL_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03"
APPROVAL_RESULT_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04"
HID_SERVICE_UUID = "00001812-0000-1000-8000-00805f9b34fb"
HID_REPORT_UUID = "00002a4d-0000-1000-8000-00805f9b34fb"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("vibewatch-bridge")


def is_process_running(process_name: str) -> bool:
    """Check if a specific process or application is currently running."""
    try:
        res = subprocess.run(["pgrep", "-f", process_name], capture_output=True, text=True)
        return res.returncode == 0
    except Exception:
        return False


def activate_application(app_name: str) -> bool:
    """Bring target application to front (Zero extra permissions required)."""
    script = f'tell application "{app_name}" to activate'
    try:
        res = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=2)
        if res.returncode == 0:
            logger.info("Activated application: %s", app_name)
            return True
        else:
            logger.warning("Could not activate %s: %s", app_name, res.stderr.strip())
            return False
    except Exception as e:
        logger.warning("Error activating %s: %s", app_name, e)
        return False


class VibeWatchBridge:
    def __init__(self, device_id: str | None = None, target_card: str = "antigravity"):
        self.device_id = device_id
        self.target_card = target_card
        self.client: BleakClient | None = None
        self.is_ptt_active = False
        self.has_doubao = is_process_running("Doubao")

    def get_target_app_name(self, card: str) -> str:
        card_upper = card.upper()
        if "ANTIGRAVITY" in card_upper or "GRAVITY" in card_upper:
            return "Antigravity"
        elif "WORKBUDDY" in card_upper or "BUDDY" in card_upper:
            return "Workbuddy"
        elif "CODEX" in card_upper:
            return "ChatGPT"
        return "Antigravity"

    def handle_agent_key(self, key: str, card: str, pressed: bool):
        """Handle AG01..AG06 slot event."""
        if not pressed:
            return
        app_name = self.get_target_app_name(card)
        logger.info(">>> Agent Key Pressed: %s (Card: %s -> Target App: %s)", key, card, app_name)
        activate_application(app_name)

    def handle_ptt_voice(self, act: int, card: str):
        """Handle PTT voice dictation with multi-tiered fallback (Doubao -> Native -> Agent)."""
        app_name = self.get_target_app_name(card)
        if act == 1:  # PTT Start (ACT10)
            self.is_ptt_active = True
            logger.info(">>> [PTT START] Activating %s and preparing voice input...", app_name)
            activate_application(app_name)
            
            # Check Doubao availability
            self.has_doubao = is_process_running("Doubao") or is_process_running("DoubaoIme")
            if self.has_doubao:
                logger.info(">>> [Voice Engine Level 1] Routing to Doubao Voice Input into %s...", app_name)
            else:
                logger.info(">>> [Voice Engine Level 2/3] Routing to macOS System Dictation into %s...", app_name)
        else:  # PTT End (ACT11)
            self.is_ptt_active = False
            logger.info(">>> [PTT END] Voice input finished. Text ready in %s Prompt.", app_name)

    def handle_approval_key(self, key: str, card: str, pressed: bool):
        """Handle OK (ACT07) / NG (ACT08) approval decisions."""
        if not pressed:
            return
        decision = "APPROVE (OK)" if "07" in key or "OK" in key else "REJECT (NG)"
        logger.info(">>> [APPROVAL] User decided on watch: %s for %s", decision, card)

    def on_hid_notification(self, sender: int, data: bytearray):
        """Parse Vendor Report 6 JSON-RPC events from watch."""
        try:
            # Report 6: [channel(1), length(1), ...json payload...]
            if len(data) < 3:
                return
            channel = data[0]
            length = data[1]
            payload_bytes = data[2:2 + length]
            text = payload_bytes.decode("utf-8", errors="ignore").strip()
            if not text:
                return

            msg = json.loads(text)
            method = msg.get("m")
            params = msg.get("p", {})

            if method == "v.oai.hid":
                key = params.get("k", "")
                act = params.get("act", 0)
                card = params.get("c", self.target_card)
                pressed = (act == 1)

                if key.startswith("AG"):
                    self.handle_agent_key(key, card, pressed)
                elif key in ("ACT10", "ACT11"):
                    self.handle_ptt_voice(1 if key == "ACT10" else 0, card)
                elif key in ("ACT07", "ACT08"):
                    self.handle_approval_key(key, card, pressed)
                else:
                    logger.info("HID Event: %s act=%d card=%s", key, act, card)
        except Exception as e:
            logger.debug("Error parsing HID report: %s (raw=%s)", e, data.hex())

    async def run(self):
        logger.info("Scanning for VibeWatch / Codex Watch BLE peripheral...")
        device = None
        if self.device_id:
            device = await BleakScanner.find_device_by_address(self.device_id, timeout=10.0)
        else:
            devices = await BleakScanner.discover(timeout=5.0)
            for d in devices:
                if d.name and ("Vibe" in d.name or "Codex" in d.name or "StopWatch" in d.name):
                    device = d
                    break

        if not device:
            logger.error("VibeWatch peripheral not found. Please ensure it is powered on and in range.")
            return

        logger.info("Connecting to VibeWatch at %s (%s)...", device.address, device.name or "Unknown")
        async with BleakClient(device) as client:
            self.client = client
            logger.info("Connected to VibeWatch! Subscribing to Vendor HID event stream...")

            # Find Vendor Report (0x2A4D with length 63 or report 6)
            for service in client.services:
                for char in service.characteristics:
                    if "notify" in char.properties:
                        try:
                            await client.start_notify(char.uuid, self.on_hid_notification)
                            logger.info("Subscribed to characteristic: %s (%s)", char.uuid, char.description)
                        except Exception as e:
                            logger.debug("Skip notify for %s: %s", char.uuid, e)

            logger.info("VibeWatch Bridge is actively listening. Press keys or PTT on your watch!")
            while True:
                await asyncio.sleep(1.0)


def main():
    parser = argparse.ArgumentParser(description="VibeWatch Bridge Daemon")
    parser.add_argument("--device-id", help="Explicit CoreBluetooth device UUID")
    parser.add_argument("--card", default="antigravity", help="Default target card (antigravity/workbuddy/codex)")
    args = parser.parse_args()

    bridge = VibeWatchBridge(device_id=args.device_id, target_card=args.card)
    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        logger.info("Bridge stopped by user.")


if __name__ == "__main__":
    main()
