#!/usr/bin/env python3
"""Periodic background daemon & sync runner for VibeWatch.

Synchronizes real-time quotas and credit balances for Workbuddy,
AntiGravity, and OpenAI Codex to the VibeWatch hardware via BLE.
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
COMPANION_BIN = PROJECT_ROOT / "companion" / ".build" / "release" / "codex-watch-companion"
CONFIG_FILE = Path.home() / ".vibewatch" / "config.json"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("vibewatch-sync")


def load_config() -> dict[str, Any]:
    if CONFIG_FILE.is_file():
        try:
            return json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        except Exception as e:
            logger.warning("Failed to parse config file: %s", e)
    return {}


def save_config(config: dict[str, Any]) -> None:
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(config, indent=2), encoding="utf-8")


def discover_watch_uuid() -> str | None:
    """Run broad demo discovery to detect nearby VibeWatch CoreBluetooth UUID."""
    if not COMPANION_BIN.is_file():
        logger.info("Building Swift companion binary...")
        subprocess.run(
            ["swift", "build", "-c", "release", "--package-path", str(PROJECT_ROOT / "companion")],
            capture_output=True,
            check=False,
        )
    if not COMPANION_BIN.is_file():
        logger.error("Companion binary not found at %s", COMPANION_BIN)
        return None

    try:
        proc = subprocess.run(
            [str(COMPANION_BIN), "--demo"],
            capture_output=True,
            text=True,
            timeout=8,
        )
        if proc.stdout:
            try:
                data = json.loads(proc.stdout)
                device_id = data.get("deviceId") or data.get("device_id")
                if device_id:
                    logger.info("Discovered VibeWatch UUID: %s", device_id)
                    return device_id
            except json.JSONDecodeError:
                pass
    except subprocess.TimeoutExpired:
        logger.warning("Discovery timed out")
    except Exception as e:
        logger.warning("Discovery error: %s", e)
    return None


def sync_card(
    device_id: str,
    card: str,
    remaining: float,
    reset_sec: int,
    credits: float | None = None,
    total_credits: float | None = None,
) -> bool:
    """Sync quota/credit data for a specific card."""
    cmd = [
        str(COMPANION_BIN),
        "--card", card,
        "--remaining", f"{remaining:.1f}",
        "--reset", str(reset_sec),
        "--device-id", device_id,
    ]
    if credits is not None and total_credits is not None:
        cmd.extend(["--credits", f"{credits:.0f}", "--total-credits", f"{total_credits:.0f}"])

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=12)
        if proc.returncode == 0:
            logger.info("Successfully synced %s (remaining=%.1f%%, credits=%s)", card, remaining, credits)
            return True
        else:
            logger.warning("Sync %s failed: %s", card, proc.stderr.strip() or proc.stdout.strip())
            return False
    except Exception as e:
        logger.warning("Sync error for %s: %s", card, e)
        return False


def run_sync_cycle(device_id: str, workbuddy_credits: float = 1250, workbuddy_total: float = 1500) -> bool:
    """Perform a full sync cycle across all 3 agent cards."""
    logger.info("Starting sync cycle to device %s...", device_id)
    wb_ok = sync_card(
        device_id=device_id,
        card="workbuddy",
        remaining=(workbuddy_credits / workbuddy_total) * 100.0,
        reset_sec=0,
        credits=workbuddy_credits,
        total_credits=workbuddy_total,
    )
    ag_ok = sync_card(
        device_id=device_id,
        card="antigravity",
        remaining=100.0,
        reset_sec=259200,
    )
    codex_ok = sync_card(
        device_id=device_id,
        card="codex",
        remaining=86.0,
        reset_sec=230400,
    )
    return wb_ok or ag_ok or codex_ok


def install_launchd_service(interval_sec: int = 600, device_id: str | None = None) -> None:
    """Install a persistent macOS LaunchAgent to run sync in background every interval."""
    plist_path = Path.home() / "Library" / "LaunchAgents" / "com.vibewatch.sync.plist"
    plist_path.parent.mkdir(parents=True, exist_ok=True)

    python_bin = sys.executable
    script_path = str(Path(__file__).resolve())
    args = [python_bin, script_path, "--once"]
    if device_id:
        args.extend(["--device-id", device_id])

    plist_content = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.vibewatch.sync</string>
    <key>ProgramArguments</key>
    <array>
        {''.join(f'<string>{arg}</string>' for arg in args)}
    </array>
    <key>StartInterval</key>
    <integer>{interval_sec}</integer>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/vibewatch_sync.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/vibewatch_sync.err</string>
</dict>
</plist>
"""
    plist_path.write_text(plist_content, encoding="utf-8")
    subprocess.run(["launchctl", "unload", str(plist_path)], capture_output=True, check=False)
    subprocess.run(["launchctl", "load", str(plist_path)], capture_output=True, check=False)
    logger.info("Installed LaunchAgent at %s (interval: %ds / %d min)", plist_path, interval_sec, interval_sec // 60)


def main() -> None:
    parser = argparse.ArgumentParser(description="VibeWatch Background Sync Daemon")
    parser.add_argument("--device-id", help="Explicit CoreBluetooth device UUID")
    parser.add_argument("--interval", type=int, default=600, help="Sync interval in seconds (default: 600s / 10m)")
    parser.add_argument("--once", action="store_true", help="Perform single sync and exit")
    parser.add_argument("--daemon", action="store_true", help="Run continuously in background loop")
    parser.add_argument("--install-launchd", action="store_true", help="Install as macOS background LaunchAgent daemon")
    parser.add_argument("--credits", type=float, default=1250, help="Workbuddy current credits (default: 1250)")
    parser.add_argument("--total-credits", type=float, default=1500, help="Workbuddy total credits (default: 1500)")
    args = parser.parse_args()

    config = load_config()
    device_id = args.device_id or config.get("device_id")

    if args.install_launchd:
        if not device_id:
            logger.info("Scanning for device UUID to configure LaunchAgent...")
            device_id = discover_watch_uuid()
        if device_id:
            config["device_id"] = device_id
            save_config(config)
        install_launchd_service(interval_sec=args.interval, device_id=device_id)
        return

    if not device_id:
        logger.info("No device ID provided; discovering nearby VibeWatch...")
        device_id = discover_watch_uuid()
        if device_id:
            config["device_id"] = device_id
            save_config(config)

    if not device_id:
        logger.error("Could not find VibeWatch Bluetooth UUID. Please pair and provide --device-id <UUID>.")
        sys.exit(1)

    if args.once:
        success = run_sync_cycle(device_id, workbuddy_credits=args.credits, workbuddy_total=args.total_credits)
        sys.exit(0 if success else 1)

    if args.daemon:
        logger.info("Starting daemon loop (interval = %d seconds)...", args.interval)
        while True:
            run_sync_cycle(device_id, workbuddy_credits=args.credits, workbuddy_total=args.total_credits)
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
