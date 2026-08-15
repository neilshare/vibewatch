#!/usr/bin/env python3
"""
VibeWatch / Codex StopWatch 额度与状态直接同步工具
可直接由 Codex / Claude Code / AI 智能体通过 Function Call 或 CLI 执行，无需依赖独立 Companion 常驻后台。
"""

import argparse
import asyncio
import json
import os
import sys
import subprocess
from pathlib import Path
from typing import Optional, Tuple

QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"

PROJECT_ROOT = Path(__file__).resolve().parent.parent
COMPANION_BIN = PROJECT_ROOT / "companion" / ".build" / "debug" / "CodexWatchCompanion"


def get_codex_rate_limits(codex_bin: str = "codex") -> Tuple[Optional[float], Optional[int]]:
    """尝试从本地 codex app-server 或环境获取当前账户额度快照"""
    try:
        proc = subprocess.Popen(
            [codex_bin, "app-server", "--listen", "stdio://"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        init_req = json.dumps({"method": "initialize", "id": 0, "params": {}}) + "\n"
        proc.stdin.write(init_req)
        proc.stdin.flush()
        
        # Read init response
        for _ in range(5):
            line = proc.stdout.readline()
            if not line:
                break
            msg = json.loads(line)
            if msg.get("id") == 0:
                break
        
        # Request rate limits
        rate_req = json.dumps({"method": "account/rateLimits", "id": 1, "params": {}}) + "\n"
        proc.stdin.write(rate_req)
        proc.stdin.flush()
        
        for _ in range(10):
            line = proc.stdout.readline()
            if not line:
                break
            msg = json.loads(line)
            if msg.get("id") == 1:
                result = msg.get("result", {})
                proc.terminate()
                rate_limits = result.get("rateLimits", {})
                weekly = rate_limits.get("weekly", rate_limits.get("primary", {}))
                if weekly:
                    rem = weekly.get("remainingPercent")
                    reset = weekly.get("resetInSeconds", 0)
                    return float(rem) if rem is not None else None, int(reset)
                break
        proc.terminate()
    except Exception:
        pass
    return None, None


def sync_via_native_companion(demo: bool = False, verbose: bool = False) -> bool:
    """使用已编译的原生 macOS CoreBluetooth 驱动写入（支持已配对/已连接状态）"""
    bin_path = COMPANION_BIN
    if not bin_path.exists():
        # Build if needed
        subprocess.run(["swift", "build"], cwd=str(PROJECT_ROOT / "companion"), check=True)

    cmd = [str(bin_path)]
    if demo:
        cmd.append("--demo")
    if verbose:
        cmd.append("-v")

    result = subprocess.run(cmd, text=True)
    return result.returncode == 0


async def sync_via_bleak(remaining_percent: float, reset_in_seconds: int, device_name_prefix: str = "Vibe Watch"):
    from bleak import BleakClient, BleakScanner

    print(f"📡 正在扫描周边 {device_name_prefix} 设备...")
    discovered = await BleakScanner.discover(timeout=4.0, return_adv=True)
    device = None
    for d, adv in discovered.values():
        name = d.name or adv.local_name or ""
        uuids = [str(u).lower() for u in (adv.service_uuids or [])]
        if device_name_prefix in name or "StopWatch" in name or QUOTA_SERVICE_UUID.lower() in uuids:
            device = d
            break

    if not device:
        print(f"❌ 未在广播列表中找到未连接的 {device_name_prefix}")
        return False

    print(f"🔗 正在连接 {device.name or 'StopWatch'} ({device.address})...")
    payload = json.dumps({
        "remaining_percent": float(remaining_percent),
        "reset_in_seconds": int(reset_in_seconds)
    }).encode("utf-8")

    async with BleakClient(device.address, timeout=10.0) as client:
        if not client.is_connected:
            return False
        await client.write_gatt_char(QUOTA_WRITE_UUID, payload, response=False)
        print(f"✅ 成功同步用量至手表！额度: {remaining_percent:.1f}%, 倒计时: {reset_in_seconds}秒")
        return True


def main():
    parser = argparse.ArgumentParser(description="直接向 VibeWatch / StopWatch 同步额度与用量数据")
    parser.add_argument("--remaining", type=float, default=None, help="剩余额度百分比 (0-100)")
    parser.add_argument("--reset", type=int, default=None, help="重置倒计时（秒）")
    parser.add_argument("--auto", action="store_true", help="自动从本地 Codex 会话读取额度")
    parser.add_argument("--demo", action="store_true", help="发送模拟测试额度数据")
    parser.add_argument("-v", "--verbose", action="store_true", help="输出详细日志")
    args = parser.parse_args()

    # Priority 1: Use native CoreBluetooth tool (handles connected HID keyboard flawlessly)
    try:
        if sync_via_native_companion(demo=args.demo, verbose=args.verbose):
            return
    except Exception as e:
        if args.verbose:
            print(f"Native companion fallback: {e}")

    # Priority 2: Bleak fallback
    rem = args.remaining
    reset = args.reset
    if rem is None or reset is None:
        auto_rem, auto_reset = get_codex_rate_limits()
        if auto_rem is not None:
            rem = auto_rem
            reset = auto_reset or 0
        else:
            rem = rem if rem is not None else 85.0
            reset = reset if reset is not None else 360000

    if args.demo:
        rem, reset = 59.0, 3600

    asyncio.run(sync_via_bleak(rem, reset))


if __name__ == "__main__":
    main()
