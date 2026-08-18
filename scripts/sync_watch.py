#!/usr/bin/env python3
"""Validated compatibility CLI for the VibeWatch Swift and Bleak transports."""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from enum import Enum
import json
import math
from pathlib import Path
import subprocess
import sys
from typing import Any, Union
from uuid import UUID, uuid4


QUOTA_SERVICE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
QUOTA_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"
APPROVAL_WRITE_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03"
APPROVAL_RESULT_UUID = "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04"

PROJECT_ROOT = Path(__file__).resolve().parent.parent
COMPANION_DIR = PROJECT_ROOT / "companion"
VALID_CARDS = frozenset({"codex", "workbuddy", "antigravity"})


class Backend(str, Enum):
    NATIVE = "native"
    BLEAK = "bleak"


@dataclass(frozen=True)
class AutomaticQuotaRequest:
    device_id: str
    card: str | None = None
    credits: str | None = None
    total_credits: str | None = None
    codex_path: str | None = None
    verbose: bool = False


@dataclass(frozen=True)
class ManualQuotaRequest:
    remaining: str
    reset: int
    device_id: str
    card: str | None = None
    credits: str | None = None
    total_credits: str | None = None
    verbose: bool = False


@dataclass(frozen=True)
class ApprovalRequest:
    device_id: str
    card: str
    request_id: str | None
    agent_id: int
    operation_type: str
    summary: str
    ttl_ms: int
    legacy: bool
    verbose: bool = False


@dataclass(frozen=True)
class DemoDiscoveryRequest:
    card: str | None = None
    credits: str | None = None
    total_credits: str | None = None
    verbose: bool = False


NormalizedRequest = Union[
    AutomaticQuotaRequest, ManualQuotaRequest, ApprovalRequest, DemoDiscoveryRequest
]


class UsageFailure(ValueError):
    pass


class TransportFailure(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class RaisingArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise UsageFailure(message)


def parser() -> RaisingArgumentParser:
    result = RaisingArgumentParser(
        description="Sync quota data or send a transactional approval to VibeWatch",
        add_help=False,
    )
    result.add_argument("-h", "--help", action="store_true", help="show this help and exit")
    result.add_argument(
        "--backend", choices=[item.value for item in Backend], default=Backend.NATIVE.value,
        help="explicit transport backend (default: native; automatic quota is native-only)",
    )
    result.add_argument("--remaining", help="manual remaining percentage, requires --reset")
    result.add_argument("--reset", help="manual reset interval in seconds, requires --remaining")
    result.add_argument("--auto", action="store_true", help="read App Server quota through the native backend")
    result.add_argument(
        "--demo", action="store_true",
        help="send synthetic quota using broad discovery and report the selected UUID",
    )
    result.add_argument("--approval", action="store_true", help="send an approval request")
    result.add_argument("--card", "--agent", help="target card: codex, workbuddy, or antigravity")
    result.add_argument("--credits", "--credit", "--balance", help="current credit balance")
    result.add_argument("--total-credits", "--total", help="positive total credit balance")
    result.add_argument("--request-id", help="canonical lowercase approval request UUID")
    result.add_argument("--agent-id", default="0", help="approval agent slot, 0 through 5")
    result.add_argument("--type", dest="operation_type", default="EXEC", help="approval operation type")
    result.add_argument("--summary", default="Run Command", help="approval summary")
    result.add_argument("--ttl-ms", default="30000", help="approval TTL, 5000 through 120000 ms")
    result.add_argument(
        "--legacy-approval", action="store_true",
        help="use the explicit one-release approval compatibility payload",
    )
    result.add_argument(
        "--device-id", help="pinned CoreBluetooth UUID required by every real operation",
    )
    result.add_argument("--codex-path", help="native automatic-quota Codex executable")
    result.add_argument("-v", "--verbose", action="store_true", help="show additional progress on stderr")
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    return parser().parse_args(argv)


def _decimal(raw: str | None, option: str, minimum: Decimal, maximum: Decimal | None = None) -> str | None:
    if raw is None:
        return None
    try:
        value = Decimal(raw)
    except InvalidOperation as error:
        raise UsageFailure(f"{option} requires a number") from error
    if not value.is_finite() or value < minimum or (maximum is not None and value > maximum):
        if maximum is None:
            raise UsageFailure(f"{option} must be at least {minimum}")
        raise UsageFailure(f"{option} must be between {minimum} and {maximum}")
    try:
        double_value = float(value)
    except (OverflowError, ValueError) as error:
        raise UsageFailure(f"{option} must be representable as a finite Double") from error
    if not math.isfinite(double_value):
        raise UsageFailure(f"{option} must be representable as a finite Double")
    return str(value)


def _integer(raw: str, option: str, minimum: int, maximum: int | None = None) -> int:
    try:
        value = int(raw)
    except ValueError as error:
        raise UsageFailure(f"{option} requires an integer") from error
    if value < minimum or (maximum is not None and value > maximum):
        if maximum is None:
            raise UsageFailure(f"{option} must be at least {minimum}")
        raise UsageFailure(f"{option} must be between {minimum} and {maximum}")
    return value


def _device_id(raw: str | None, mode: str) -> str:
    if raw is None:
        raise UsageFailure(f"{mode} requires --device-id")
    try:
        return str(UUID(raw))
    except ValueError as error:
        raise UsageFailure("--device-id requires a valid UUID") from error


def _card(raw: str | None) -> str | None:
    if raw is None:
        return None
    value = raw.lower()
    if value not in VALID_CARDS:
        raise UsageFailure(f"unknown card: {value}")
    return value


def build_request(args: argparse.Namespace) -> NormalizedRequest:
    manual = args.remaining is not None or args.reset is not None
    explicit_modes = int(args.auto) + int(args.demo) + int(args.approval) + int(manual)
    if explicit_modes > 1:
        raise UsageFailure("mode flags are mutually exclusive")
    if manual and (args.remaining is None or args.reset is None):
        raise UsageFailure("manual quota requires both --remaining and --reset")

    card = _card(args.card)
    credits = _decimal(args.credits, "--credits", Decimal(0))
    total_credits = _decimal(args.total_credits, "--total-credits", Decimal(0))
    if total_credits is not None and Decimal(total_credits) == 0:
        raise UsageFailure("--total-credits must be positive")

    approval_options_used = (
        args.request_id is not None or args.agent_id != "0" or args.operation_type != "EXEC"
        or args.summary != "Run Command" or args.ttl_ms != "30000" or args.legacy_approval
    )
    if not args.approval and approval_options_used:
        raise UsageFailure("approval options require --approval")
    if args.approval and (credits is not None or total_credits is not None or args.codex_path is not None):
        raise UsageFailure("quota and App Server options cannot be used with --approval")
    if args.codex_path is not None and (manual or args.demo):
        raise UsageFailure("--codex-path is only valid with automatic quota")

    if args.approval:
        request_id = None
        if args.request_id is not None:
            try:
                request_id = str(UUID(args.request_id))
            except ValueError as error:
                raise UsageFailure("--request-id requires a canonical lowercase UUID") from error
            if args.request_id != request_id:
                raise UsageFailure("--request-id requires a canonical lowercase UUID")
        agent_id = _integer(args.agent_id, "--agent-id", 0, 5)
        ttl_ms = _integer(args.ttl_ms, "--ttl-ms", 5_000, 120_000)
        if not 1 <= len(args.operation_type.encode("utf-8")) <= 23:
            raise UsageFailure("--type must contain 1...23 UTF-8 bytes")
        if not 1 <= len(args.summary.encode("utf-8")) <= 95:
            raise UsageFailure("--summary must contain 1...95 UTF-8 bytes")
        return ApprovalRequest(
            device_id=_device_id(args.device_id, "approval"), card=card or "codex",
            request_id=request_id, agent_id=agent_id, operation_type=args.operation_type,
            summary=args.summary, ttl_ms=ttl_ms, legacy=args.legacy_approval,
            verbose=args.verbose,
        )

    if args.demo:
        if args.device_id is not None:
            raise UsageFailure("demo discovery does not accept --device-id")
        return DemoDiscoveryRequest(card, credits, total_credits, args.verbose)

    if manual:
        remaining = _decimal(args.remaining, "--remaining", Decimal(0), Decimal(100))
        reset = _integer(args.reset, "--reset", 0)
        return ManualQuotaRequest(
            remaining=remaining, reset=reset,
            device_id=_device_id(args.device_id, "manual quota"), card=card,
            credits=credits, total_credits=total_credits, verbose=args.verbose,
        )

    return AutomaticQuotaRequest(
        device_id=_device_id(args.device_id, "automatic quota"), card=card,
        credits=credits, total_credits=total_credits, codex_path=args.codex_path,
        verbose=args.verbose,
    )


def native_command(request: NormalizedRequest, executable: str | Path) -> list[str]:
    command = [str(executable)]
    if isinstance(request, AutomaticQuotaRequest):
        command.append("--auto")
        if request.codex_path is not None:
            command.extend(["--codex-path", request.codex_path])
    elif isinstance(request, ManualQuotaRequest):
        command.extend(["--remaining", request.remaining, "--reset", str(request.reset)])
    elif isinstance(request, ApprovalRequest):
        command.append("--approval")
    else:
        command.append("--demo")

    if request.card is not None:
        command.extend(["--card", request.card])
    if isinstance(request, (AutomaticQuotaRequest, ManualQuotaRequest, DemoDiscoveryRequest)):
        if request.credits is not None:
            command.extend(["--credits", request.credits])
        if request.total_credits is not None:
            command.extend(["--total-credits", request.total_credits])
    if isinstance(request, ApprovalRequest):
        if request.request_id is not None:
            command.extend(["--request-id", request.request_id])
        command.extend([
            "--agent-id", str(request.agent_id), "--type", request.operation_type,
            "--summary", request.summary, "--ttl-ms", str(request.ttl_ms),
        ])
        if request.legacy:
            command.append("--legacy-approval")
    if not isinstance(request, DemoDiscoveryRequest):
        command.extend(["--device-id", request.device_id])
    if request.verbose:
        command.append("--verbose")
    return command


def _find_companion_binary() -> Path | None:
    direct = [
        COMPANION_DIR / ".build" / "debug" / "codex-watch-companion",
        COMPANION_DIR / ".build" / "debug" / "CodexWatchCompanion",
        COMPANION_DIR / ".build" / "release" / "codex-watch-companion",
        COMPANION_DIR / ".build" / "release" / "CodexWatchCompanion",
    ]
    for candidate in direct:
        if candidate.is_file():
            return candidate
    for configuration in ("debug", "release"):
        candidates = sorted((COMPANION_DIR / ".build").glob(f"*/{configuration}/codex-watch-companion"))
        candidates += sorted((COMPANION_DIR / ".build").glob(f"*/{configuration}/CodexWatchCompanion"))
        if candidates:
            return candidates[0]
    return None


def run_native(request: NormalizedRequest, executable: str | Path | None = None) -> int:
    binary = Path(executable) if executable is not None else _find_companion_binary()
    if binary is None or not binary.is_file():
        try:
            build = subprocess.run(["swift", "build"], cwd=COMPANION_DIR)
        except OSError as error:
            print(f"native build failed: {error}", file=sys.stderr)
            return 1
        if build.returncode != 0:
            return build.returncode
        binary = _find_companion_binary()
        if binary is None:
            print("native build succeeded but companion executable was not found", file=sys.stderr)
            return 1
    try:
        return subprocess.run(native_command(request, binary)).returncode
    except OSError as error:
        print(f"native transport failed: {error}", file=sys.stderr)
        return 1


class BleakAdapter:
    """Small injectable boundary around Bleak scanning and client creation."""

    async def resolve_pinned(self, device_id: str) -> Any:
        from bleak import BleakScanner
        return await BleakScanner.find_device_by_address(device_id, timeout=5.0)

    async def discover_demo(self) -> Any:
        from bleak import BleakScanner
        discovered = await BleakScanner.discover(timeout=5.0, return_adv=True)
        for device, advertisement in discovered.values():
            name = device.name or advertisement.local_name or ""
            services = {str(value).lower() for value in (advertisement.service_uuids or [])}
            if "Vibe Watch" in name or "StopWatch" in name or QUOTA_SERVICE_UUID in services:
                return device
        return None

    def client(self, device: Any) -> Any:
        from bleak import BleakClient
        return BleakClient(device, timeout=10.0)


def _json_bytes(value: dict[str, Any]) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")


def _quota_payload(request: ManualQuotaRequest | DemoDiscoveryRequest) -> dict[str, Any]:
    if isinstance(request, ManualQuotaRequest):
        remaining = float(Decimal(request.remaining))
        reset = request.reset
    else:
        remaining = 59.0
        reset = 3_600
    payload: dict[str, Any] = {
        "remaining_percent": remaining,
        "reset_in_seconds": reset,
    }
    if request.card is not None:
        payload["card"] = request.card
    if request.credits is not None:
        payload["credits"] = float(Decimal(request.credits))
    if request.total_credits is not None:
        payload["total_credits"] = float(Decimal(request.total_credits))
    return payload


async def _run_bleak(
    request: NormalizedRequest,
    adapter: Any,
    timeout_override: float | None,
) -> dict[str, Any] | None:
    if isinstance(request, AutomaticQuotaRequest):
        raise TransportFailure(
            "transport_error",
            "Automatic App Server quota is supported by the native backend; select --backend native.",
        )
    if isinstance(request, DemoDiscoveryRequest):
        device = await adapter.discover_demo()
    else:
        device = await adapter.resolve_pinned(request.device_id)
    if device is None:
        raise TransportFailure("device_not_found", "Pinned Bluetooth device was not found.")
    if isinstance(request, DemoDiscoveryRequest):
        raw_identifier = getattr(device, "address", None)
        try:
            selected_id = str(UUID(str(raw_identifier)))
        except (ValueError, TypeError) as error:
            raise TransportFailure(
                "transport_error",
                "Synthetic/demo discovery selected a device without a bindable CoreBluetooth UUID.",
            ) from error
        print(
            f"Synthetic/demo discovery selected device UUID {selected_id}; "
            f"use --device-id {selected_id} for a real operation.",
            file=sys.stderr,
        )

    delivered_decision: dict[str, Any] | None = None
    client_context = adapter.client(device)
    client = await client_context.__aenter__()
    primary_error: BaseException | None = None
    try:
        if not client.is_connected:
            raise TransportFailure("transport_error", "Bluetooth connection failed.")
        if isinstance(request, (ManualQuotaRequest, DemoDiscoveryRequest)):
            payload = _quota_payload(request)
            await client.write_gatt_char(QUOTA_WRITE_UUID, _json_bytes(payload), response=True)
            return payload

        request_id = request.request_id or str(uuid4())
        if request.legacy:
            payload = {
                "method": "v.oai.approval_req",
                "params": {
                    "active": True, "type": request.operation_type,
                    "summary": request.summary, "card": request.card,
                },
            }
            await client.write_gatt_char(APPROVAL_WRITE_UUID, _json_bytes(payload), response=True)
            return None

        payload = {
            "version": 2, "kind": "approval_request", "request_id": request_id,
            "card": request.card, "agent_id": request.agent_id,
            "operation_type": request.operation_type, "summary": request.summary,
            "ttl_ms": request.ttl_ms,
        }
        loop = asyncio.get_running_loop()
        result: asyncio.Future[dict[str, Any]] = loop.create_future()

        def receive(_: Any, raw: bytearray | bytes) -> None:
            try:
                message = json.loads(bytes(raw))
            except (TypeError, ValueError, UnicodeDecodeError):
                return
            if not isinstance(message, dict):
                return
            if message.get("version") != 2:
                return
            if message.get("kind") == "approval_decision":
                decided_at = message.get("decided_at_ms")
                if (
                    message.get("request_id") != request_id
                    or message.get("decision") not in {"approve", "reject", "expired", "cancelled"}
                    or isinstance(decided_at, bool)
                    or not isinstance(decided_at, int)
                    or not 0 <= decided_at <= 0xFFFFFFFF
                ):
                    return
                loop.call_soon_threadsafe(lambda: not result.done() and result.set_result(message))
            elif message.get("kind") == "error":
                raw_id = message.get("request_id")
                if "request_id" not in message or not isinstance(raw_id, str):
                    error = TransportFailure("transport_error", "Malformed approval error indication.")
                elif raw_id not in ("", request_id):
                    return
                elif not isinstance(message.get("code"), str) or not isinstance(message.get("message"), str):
                    error = TransportFailure("transport_error", "Malformed approval error indication.")
                else:
                    error = TransportFailure(message["code"], message["message"])
                loop.call_soon_threadsafe(
                    lambda: not result.done() and result.set_exception(error)
                )

        subscribed = False
        approval_error: BaseException | None = None
        try:
            await client.start_notify(APPROVAL_RESULT_UUID, receive)
            subscribed = True
            await client.write_gatt_char(APPROVAL_WRITE_UUID, _json_bytes(payload), response=True)
            timeout = timeout_override if timeout_override is not None else request.ttl_ms / 1_000 + 5
            try:
                delivered_decision = await asyncio.wait_for(result, timeout=timeout)
                return delivered_decision
            except asyncio.TimeoutError as error:
                raise TransportFailure(
                    "transport_error", "Timed out waiting for a matching approval decision."
                ) from error
        except BaseException as error:
            approval_error = error
            raise
        finally:
            if subscribed:
                try:
                    await client.stop_notify(APPROVAL_RESULT_UUID)
                except Exception:
                    if approval_error is None and delivered_decision is None:
                        raise
    except BaseException as error:
        primary_error = error
        raise
    finally:
        try:
            await client_context.__aexit__(
                type(primary_error) if primary_error is not None else None,
                primary_error,
                primary_error.__traceback__ if primary_error is not None else None,
            )
        except Exception:
            if primary_error is None and delivered_decision is None:
                raise


def run_bleak(
    request: NormalizedRequest,
    adapter: Any | None = None,
    *,
    timeout_override: float | None = None,
) -> int:
    try:
        output = asyncio.run(_run_bleak(request, adapter or BleakAdapter(), timeout_override))
    except TransportFailure as error:
        print(json.dumps({
            "version": 2, "kind": "error", "code": error.code, "message": str(error),
        }, sort_keys=True), file=sys.stderr)
        return 1
    except Exception as error:
        print(json.dumps({
            "version": 2, "kind": "error", "code": "transport_error", "message": str(error),
        }, sort_keys=True), file=sys.stderr)
        return 1
    if output is not None:
        print(json.dumps(output, ensure_ascii=False, separators=(",", ":"), sort_keys=True))
    return 0


def main(argv: list[str] | None = None) -> int:
    cli_parser = parser()
    try:
        args = cli_parser.parse_args(argv)
        if args.help:
            cli_parser.print_help()
            return 0
        request = build_request(args)
    except UsageFailure as error:
        print(f"usage error: {error}", file=sys.stderr)
        return 2

    if Backend(args.backend) is Backend.BLEAK:
        return run_bleak(request)
    return run_native(request)


if __name__ == "__main__":
    raise SystemExit(main())
