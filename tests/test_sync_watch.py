import inspect
import json
from pathlib import Path
import subprocess
import sys

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import sync_watch


VALID_UUID = "550e8400-e29b-41d4-a716-446655440000"


class NativeRunner:
    def __init__(self, returncode=0):
        self.returncode = returncode
        self.requests = []

    def __call__(self, request, executable=None):
        self.requests.append(request)
        return self.returncode


def test_manual_quota_forwards_every_value(monkeypatch):
    commands = []

    def fake_run_native(request, executable=None):
        commands.append(sync_watch.native_command(request, "/tmp/companion"))
        return 0

    monkeypatch.setattr(sync_watch, "run_native", fake_run_native)
    code = sync_watch.main([
        "--remaining", "42.5", "--reset", "900", "--card", "workbuddy",
        "--credits", "425", "--total-credits", "1000",
        "--device-id", VALID_UUID,
    ])

    assert code == 0
    assert commands == [[
        "/tmp/companion", "--remaining", "42.5", "--reset", "900",
        "--card", "workbuddy", "--credits", "425", "--total-credits", "1000",
        "--device-id", VALID_UUID,
    ]]


def test_failed_native_transport_returns_nonzero_without_bleak_retry(monkeypatch):
    native = NativeRunner(returncode=1)
    monkeypatch.setattr(sync_watch, "run_native", native)
    monkeypatch.setattr(sync_watch, "run_bleak", lambda *_: pytest.fail("unexpected fallback"))
    assert sync_watch.main(["--auto", "--device-id", VALID_UUID]) == 1


def test_approval_forwards_all_fields_to_native(monkeypatch):
    commands = []
    monkeypatch.setattr(
        sync_watch,
        "run_native",
        lambda request, executable=None: commands.append(
            sync_watch.native_command(request, "/tmp/companion")
        ) or 0,
    )
    assert sync_watch.main([
        "--approval", "--card", "antigravity", "--request-id", VALID_UUID,
        "--agent-id", "5", "--type", "WRITE", "--summary", "Edit settings",
        "--ttl-ms", "120000", "--legacy-approval", "--device-id", VALID_UUID,
    ]) == 0
    assert commands == [[
        "/tmp/companion", "--approval", "--card", "antigravity",
        "--request-id", VALID_UUID, "--agent-id", "5", "--type", "WRITE",
        "--summary", "Edit settings", "--ttl-ms", "120000",
        "--legacy-approval", "--device-id", VALID_UUID,
    ]]


@pytest.mark.parametrize(
    "argv",
    [
        ["--auto"],
        ["--remaining", "10", "--reset", "20"],
        ["--approval", "--summary", "Run"],
        ["--auto", "--remaining", "10", "--reset", "20", "--device-id", VALID_UUID],
        ["--remaining", "10", "--device-id", VALID_UUID],
        ["--auto", "--card", "unknown", "--device-id", VALID_UUID],
        ["--approval", "--type", "", "--device-id", VALID_UUID],
        ["--approval", "--summary", "x" * 96, "--device-id", VALID_UUID],
        ["--approval", "--ttl-ms", "4999", "--device-id", VALID_UUID],
        ["--approval", "--agent-id", "6", "--device-id", VALID_UUID],
    ],
)
def test_invalid_request_is_usage_failure_before_transport(argv, monkeypatch):
    monkeypatch.setattr(sync_watch, "run_native", lambda *_: pytest.fail("transport called"))
    assert sync_watch.main(argv) == 2


@pytest.mark.parametrize("option", ["--credits", "--total-credits"])
@pytest.mark.parametrize("backend", ["native", "bleak"])
def test_non_double_numeric_value_is_rejected_before_backend(option, backend, monkeypatch, capsys):
    monkeypatch.setattr(sync_watch, "run_native", lambda *_: pytest.fail("native called"))
    monkeypatch.setattr(sync_watch, "run_bleak", lambda *_: pytest.fail("bleak called"))

    assert sync_watch.main([
        "--backend", backend, "--auto", option, "1e10000", "--device-id", VALID_UUID,
    ]) == 2
    assert "finite double" in capsys.readouterr().err.lower()


def test_bleak_backend_is_selected_only_when_explicit(monkeypatch):
    calls = []
    monkeypatch.setattr(sync_watch, "run_native", lambda *_: pytest.fail("native called"))
    monkeypatch.setattr(sync_watch, "run_bleak", lambda request, adapter=None: calls.append(request) or 0)
    assert sync_watch.main([
        "--backend", "bleak", "--remaining", "25", "--reset", "60",
        "--device-id", VALID_UUID,
    ]) == 0
    assert calls[0].remaining == "25"


def test_native_build_failure_is_returned(monkeypatch, tmp_path):
    missing = tmp_path / "missing-companion"
    calls = []

    def fake_run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 1)

    monkeypatch.setattr(sync_watch.subprocess, "run", fake_run)
    request = sync_watch.build_request(sync_watch.parse_args(["--auto", "--device-id", VALID_UUID]))
    assert sync_watch.run_native(request, missing) == 1
    assert calls == [["swift", "build"]]


class FakeBleakClient:
    def __init__(
        self, approval_result=None, connected=True, write_error=None,
        stop_error=None, exit_error=None,
    ):
        self.approval_result = approval_result
        self.is_connected = connected
        self.write_error = write_error
        self.stop_error = stop_error
        self.exit_error = exit_error
        self.events = []
        self.callback = None

    async def __aenter__(self):
        self.events.append(("connect",))
        return self

    async def __aexit__(self, *_):
        self.events.append(("disconnect",))
        if self.exit_error is not None:
            raise self.exit_error

    async def start_notify(self, characteristic, callback):
        self.events.append(("subscribe", characteristic))
        self.callback = callback

    async def stop_notify(self, characteristic):
        self.events.append(("unsubscribe", characteristic))
        if self.stop_error is not None:
            raise self.stop_error

    async def write_gatt_char(self, characteristic, payload, response):
        self.events.append(("write", characteristic, json.loads(payload), response))
        if self.write_error is not None:
            raise self.write_error
        if self.approval_result is not None:
            self.callback(characteristic, json.dumps(self.approval_result).encode())


class FakeBleakAdapter:
    def __init__(self, client, device="pinned-device", discovery_error=None):
        self._client = client
        self.device = device
        self.discovery_error = discovery_error
        self.resolved = []
        self.demo_discoveries = 0

    async def resolve_pinned(self, device_id):
        self.resolved.append(device_id)
        return self.device

    async def discover_demo(self):
        self.demo_discoveries += 1
        if self.discovery_error is not None:
            raise self.discovery_error
        return self.device

    def client(self, device):
        assert device == self.device
        return self._client


class FakeDevice:
    def __init__(self, address):
        self.address = address


def test_bleak_manual_quota_uses_exact_device_schema_and_write_response(capsys):
    client = FakeBleakClient()
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--remaining", "42.5", "--reset", "900",
        "--card", "workbuddy", "--credits", "425", "--total-credits", "1000",
        "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, adapter) == 0
    assert adapter.resolved == [VALID_UUID]
    assert adapter.demo_discoveries == 0
    write = next(event for event in client.events if event[0] == "write")
    assert write == (
        "write", sync_watch.QUOTA_WRITE_UUID,
        {
            "card": "workbuddy", "credits": 425.0, "remaining_percent": 42.5,
            "reset_in_seconds": 900, "total_credits": 1000.0,
        },
        True,
    )
    assert json.loads(capsys.readouterr().out) == write[2]


def test_bleak_v2_approval_subscribes_before_response_write_and_correlates(capsys):
    result = {
        "version": 2, "kind": "approval_decision", "request_id": VALID_UUID,
        "decision": "approve", "decided_at_ms": 123,
    }
    client = FakeBleakClient(approval_result=result)
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--card", "workbuddy",
        "--request-id", VALID_UUID, "--agent-id", "2", "--type", "EXEC",
        "--summary", "Run tests", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, adapter) == 0
    subscribe_index = next(i for i, event in enumerate(client.events) if event[0] == "subscribe")
    write_index = next(i for i, event in enumerate(client.events) if event[0] == "write")
    assert subscribe_index < write_index
    assert client.events[subscribe_index][1] == sync_watch.APPROVAL_RESULT_UUID
    write = client.events[write_index]
    assert write[1] == sync_watch.APPROVAL_WRITE_UUID
    assert write[3] is True
    assert write[2] == {
        "version": 2, "kind": "approval_request", "request_id": VALID_UUID,
        "card": "workbuddy", "agent_id": 2, "operation_type": "EXEC",
        "summary": "Run tests", "ttl_ms": 5000,
    }
    assert json.loads(capsys.readouterr().out) == result


@pytest.mark.parametrize("cleanup", ["stop", "disconnect"])
def test_bleak_valid_decision_survives_cleanup_failure(cleanup, capsys):
    result = {
        "version": 2, "kind": "approval_decision", "request_id": VALID_UUID,
        "decision": "approve", "decided_at_ms": 123,
    }
    cleanup_error = Exception(f"{cleanup} failed")
    client = FakeBleakClient(
        approval_result=result,
        stop_error=cleanup_error if cleanup == "stop" else None,
        exit_error=cleanup_error if cleanup == "disconnect" else None,
    )
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter) == 0
    assert json.loads(capsys.readouterr().out) == result


def test_bleak_primary_transport_error_survives_cleanup_failure(capsys):
    client = FakeBleakClient(
        approval_result=None, write_error=Exception("write denied"),
        stop_error=Exception("stop failed"), exit_error=Exception("disconnect failed"),
    )
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter) == 1
    assert json.loads(capsys.readouterr().err)["message"] == "write denied"


def test_bleak_approval_ignores_mismatched_request_id(capsys):
    client = FakeBleakClient(approval_result={
        "version": 2, "kind": "approval_decision",
        "request_id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "decision": "approve", "decided_at_ms": 123,
    })
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, adapter, timeout_override=0.01) == 1
    assert "matching approval" in capsys.readouterr().err.lower()


def test_bleak_approval_rejects_malformed_matching_decision(capsys):
    client = FakeBleakClient(approval_result={
        "version": 2, "kind": "approval_decision", "request_id": VALID_UUID,
        "decision": "yes", "decided_at_ms": -1,
    })
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter, timeout_override=0.01) == 1
    assert "matching approval" in capsys.readouterr().err.lower()


@pytest.mark.parametrize("request_id", ["", VALID_UUID])
def test_bleak_approval_accepts_well_formed_empty_or_matching_protocol_error(request_id, capsys):
    client = FakeBleakClient(approval_result={
        "version": 2, "kind": "error", "request_id": request_id,
        "code": "busy", "message": "Another approval is pending.",
    })
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter) == 1
    error = json.loads(capsys.readouterr().err)
    assert error["code"] == "busy"
    assert error["message"] == "Another approval is pending."


@pytest.mark.parametrize(
    "malformed",
    [
        {"version": 2, "kind": "error", "code": "busy", "message": "Busy"},
        {"version": 2, "kind": "error", "request_id": None, "code": "busy", "message": "Busy"},
        {"version": 2, "kind": "error", "request_id": VALID_UUID, "code": 7, "message": "Busy"},
        {"version": 2, "kind": "error", "request_id": VALID_UUID, "code": "busy"},
    ],
)
def test_bleak_approval_rejects_malformed_protocol_error(malformed, capsys):
    client = FakeBleakClient(approval_result=malformed)
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter) == 1
    error = json.loads(capsys.readouterr().err)
    assert error["code"] == "transport_error"
    assert "malformed" in error["message"].lower()


def test_bleak_approval_ignores_well_formed_mismatched_protocol_error(capsys):
    client = FakeBleakClient(approval_result={
        "version": 2, "kind": "error",
        "request_id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "code": "busy", "message": "Unrelated request",
    })
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--request-id", VALID_UUID,
        "--summary", "Run", "--ttl-ms", "5000", "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter, timeout_override=0.01) == 1
    error = json.loads(capsys.readouterr().err)
    assert error["code"] == "transport_error"
    assert "matching approval" in error["message"].lower()


def test_bleak_legacy_approval_is_ack_only_on_new_characteristic(capsys):
    client = FakeBleakClient()
    adapter = FakeBleakAdapter(client)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--approval", "--legacy-approval",
        "--request-id", VALID_UUID, "--card", "codex", "--type", "WRITE",
        "--summary", "Edit file", "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, adapter) == 0
    assert not any(event[0] == "subscribe" for event in client.events)
    write = next(event for event in client.events if event[0] == "write")
    assert write[1] == sync_watch.APPROVAL_WRITE_UUID
    assert write[2] == {
        "method": "v.oai.approval_req",
        "params": {"active": True, "card": "codex", "summary": "Edit file", "type": "WRITE"},
    }
    assert write[3] is True
    assert capsys.readouterr().out == ""


def test_demo_is_the_only_bleak_mode_using_broad_discovery():
    client = FakeBleakClient()
    adapter = FakeBleakAdapter(client, device=FakeDevice(VALID_UUID))
    request = sync_watch.build_request(sync_watch.parse_args(["--backend", "bleak", "--demo"]))
    assert sync_watch.run_bleak(request, adapter) == 0
    assert adapter.resolved == []
    assert adapter.demo_discoveries == 1


def test_bleak_demo_reports_bindable_uuid_on_stderr_and_keeps_stdout_machine_json(capsys):
    client = FakeBleakClient()
    device = FakeDevice(VALID_UUID.upper())
    adapter = FakeBleakAdapter(client, device=device)
    request = sync_watch.build_request(sync_watch.parse_args(["--backend", "bleak", "--demo"]))

    assert sync_watch.run_bleak(request, adapter) == 0
    captured = capsys.readouterr()
    assert "synthetic/demo" in captured.err.lower()
    assert VALID_UUID in captured.err
    assert json.loads(captured.out) == {
        "remaining_percent": 59.0,
        "reset_in_seconds": 3600,
    }
    assert adapter.demo_discoveries == 1
    assert client.events.count(("connect",)) == 1
    assert sum(event[0] == "write" for event in client.events) == 1


def test_bleak_demo_no_device_is_labeled_before_demo_specific_failure(capsys):
    adapter = FakeBleakAdapter(FakeBleakClient(), device=None)
    request = sync_watch.build_request(sync_watch.parse_args(["--backend", "bleak", "--demo"]))

    assert sync_watch.run_bleak(request, adapter) == 1
    captured = capsys.readouterr()
    assert captured.out == ""
    assert "synthetic/demo" in captured.err.lower()
    assert "demo discovery did not find" in captured.err.lower()
    assert "pinned bluetooth device" not in captured.err.lower()


def test_bleak_demo_discovery_exception_is_labeled_and_stdout_stays_empty(capsys):
    adapter = FakeBleakAdapter(
        FakeBleakClient(), discovery_error=Exception("bluetooth unavailable"),
    )
    request = sync_watch.build_request(sync_watch.parse_args(["--backend", "bleak", "--demo"]))

    assert sync_watch.run_bleak(request, adapter) == 1
    captured = capsys.readouterr()
    assert captured.out == ""
    assert "synthetic/demo" in captured.err.lower()
    assert "bluetooth unavailable" in captured.err.lower()


def test_help_lists_supported_modes_without_bootloader(capsys):
    assert sync_watch.main(["--help"]) == 0
    help_text = capsys.readouterr().out
    for flag in (
        "--backend {native,bleak}", "--auto", "--remaining", "--reset",
        "--demo", "--approval", "--device-id", "--legacy-approval",
    ):
        assert flag in help_text
    assert "default: native" in help_text.lower()
    assert "synthetic" in help_text.lower()
    assert "broad discovery" in help_text.lower()
    assert "real operation" in " ".join(help_text.lower().split())
    assert "bootloader" not in help_text.lower()


def test_missing_pinned_bleak_device_is_nonzero(capsys):
    client = FakeBleakClient()
    adapter = FakeBleakAdapter(client, device=None)
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--remaining", "10", "--reset", "60",
        "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, adapter) == 1
    assert "device_not_found" in capsys.readouterr().err


def test_bleak_write_failure_is_reported_as_nonzero(capsys):
    adapter = FakeBleakAdapter(FakeBleakClient(write_error=Exception("write denied")))
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--remaining", "10", "--reset", "60",
        "--device-id", VALID_UUID,
    ]))

    assert sync_watch.run_bleak(request, adapter) == 1
    error = json.loads(capsys.readouterr().err)
    assert error == {
        "version": 2, "kind": "error", "code": "transport_error",
        "message": "write denied",
    }


def test_automatic_bleak_is_not_implemented_with_fake_quota(capsys):
    request = sync_watch.build_request(sync_watch.parse_args([
        "--backend", "bleak", "--auto", "--device-id", VALID_UUID,
    ]))
    assert sync_watch.run_bleak(request, FakeBleakAdapter(FakeBleakClient())) == 1
    assert "native backend" in capsys.readouterr().err.lower()
    source = inspect.getsource(sync_watch)
    assert "85.0" not in source
    assert "360000" not in source
