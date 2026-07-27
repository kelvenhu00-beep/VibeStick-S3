from __future__ import annotations

import io
import json
import tempfile
import unittest
from pathlib import Path

from vibe_stick.server.app import (
    RequestBodyError,
    SingleInstanceLock,
    _compact_audio_ack_response,
    _compact_recording_response,
    make_handler,
)


class _MinimalStore:
    def start_recording(self, body):  # noqa: ANN001
        return {"recording": {"status": "recording"}, "state": {}}


class ServerRuntimeTests(unittest.TestCase):
    def test_single_instance_lock_rejects_second_owner(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bridge.lock"
            first = SingleInstanceLock(path)
            second = SingleInstanceLock(path)
            first.acquire()
            self.addCleanup(first.close)

            with self.assertRaises(SystemExit):
                second.acquire()

            first.close()
            second.acquire()
            second.close()

    def test_incomplete_request_body_is_rejected(self) -> None:
        handler_type = make_handler(_MinimalStore())
        handler = object.__new__(handler_type)
        handler.rfile = io.BytesIO(b"{}")

        with self.assertRaises(RequestBodyError):
            handler._read_raw_body(100)

    def test_compact_recording_response_fits_firmware_capture_buffer(self) -> None:
        response = {
            "recording": {
                "session_id": "a" * 32,
                "status": "audio_ready",
                "pasted": False,
                "message": "x" * 1000,
            },
            "state": {
                "bridge_name": "vibestick-bridge",
                "bridge_version": "0.1.4",
            },
        }

        compact = _compact_recording_response(response)
        encoded = json.dumps(
            compact,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode()

        self.assertEqual(compact["recording"]["session_id"], "a" * 32)
        self.assertEqual(compact["recording"]["status"], "audio_ready")
        self.assertLess(len(encoded), 768)

    def test_compact_audio_ack_omits_state_and_has_large_buffer_margin(self) -> None:
        response = {
            "recording": {
                "session_id": "a" * 32,
                "status": "audio_ready",
                "pasted": False,
            },
            "state": {"large_future_field": "x" * 5000},
        }

        compact = _compact_audio_ack_response(response)
        encoded = json.dumps(
            compact,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode()

        self.assertEqual(
            compact,
            {
                "recording": {
                    "session_id": "a" * 32,
                    "status": "audio_ready",
                }
            },
        )
        self.assertLess(len(encoded), 160)


if __name__ == "__main__":
    unittest.main()
