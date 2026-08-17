import json
import pytest
from pathlib import Path
import sys

# Add scripts directory to path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

from sync_watch import QUOTA_SERVICE_UUID, QUOTA_WRITE_UUID


def test_uuid_constants():
    assert QUOTA_SERVICE_UUID == "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01"
    assert QUOTA_WRITE_UUID == "7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02"


def test_quota_payload_serialization():
    data = {
        "remaining_percent": 86.0,
        "reset_in_seconds": 236000,
        "card": "codex"
    }
    payload = json.dumps(data).encode("utf-8")
    parsed = json.loads(payload.decode("utf-8"))
    assert parsed["remaining_percent"] == 86.0
    assert parsed["reset_in_seconds"] == 236000
    assert parsed["card"] == "codex"


def test_approval_payload_serialization():
    data = {
        "method": "v.oai.approval_req",
        "params": {
            "active": True,
            "type": "EXEC",
            "summary": "pio run --target upload",
            "card": "workbuddy"
        }
    }
    payload = json.dumps(data).encode("utf-8")
    parsed = json.loads(payload.decode("utf-8"))
    assert parsed["method"] == "v.oai.approval_req"
    assert parsed["params"]["active"] is True
    assert parsed["params"]["type"] == "EXEC"
    assert parsed["params"]["card"] == "workbuddy"


def test_tri_agent_card_names():
    valid_cards = ["codex", "workbuddy", "antigravity"]
    for c in valid_cards:
        payload = json.dumps({"card": c, "remaining_percent": 50.0, "reset_in_seconds": 3600})
        parsed = json.loads(payload)
        assert parsed["card"] in valid_cards


def test_workbuddy_credits_payload():
    data = {
        "card": "workbuddy",
        "credits": 1250.0,
        "total_credits": 1500.0,
        "remaining_percent": 83.3,
        "reset_in_seconds": 0
    }
    payload = json.dumps(data).encode("utf-8")
    parsed = json.loads(payload.decode("utf-8"))
    assert parsed["card"] == "workbuddy"
    assert parsed["credits"] == 1250.0
    assert parsed["total_credits"] == 1500.0
    assert parsed["remaining_percent"] == 83.3

