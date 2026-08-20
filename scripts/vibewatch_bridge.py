#!/usr/bin/env python3
"""VibeWatch Universal Host Bridge for Antigravity, Workbuddy, and Codex.

Delegates to the native macOS CoreBluetooth Swift binary for 100% reliable,
zero-latency background event routing, application focus, and PTT voice input.
"""

from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path
import subprocess
import sys

PROJECT_ROOT = Path(__file__).resolve().parent.parent
COMPANION_BIN = PROJECT_ROOT / "companion" / ".build" / "release" / "codex-watch-companion"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("vibewatch-bridge")


def ensure_binary():
    if not COMPANION_BIN.is_file():
        logger.info("Building native Swift companion binary...")
        subprocess.run(["swift", "build", "-c", "release", "--package-path", str(PROJECT_ROOT / "companion")], check=True)


def main():
    parser = argparse.ArgumentParser(description="VibeWatch Bridge Daemon")
    parser.add_argument("--device-id", help="Explicit CoreBluetooth device UUID")
    parser.add_argument("--card", default="antigravity", help="Default target card (antigravity/workbuddy/codex)")
    args = parser.parse_args()

    ensure_binary()
    cmd = [str(COMPANION_BIN), "--bridge", "--card", args.card]
    if args.device_id:
        cmd.extend(["--device-id", args.device_id])

    logger.info("Starting VibeWatch Native Bridge Daemon...")
    try:
        proc = subprocess.run(cmd)
        sys.exit(proc.returncode)
    except KeyboardInterrupt:
        logger.info("Bridge stopped by user.")


if __name__ == "__main__":
    main()


if __name__ == "__main__":
    main()
