from __future__ import annotations

import base64
import json
import unittest

from vibe_stick.server.usb_transport import PROTOCOL_PREFIX, USBProtocol


def _b64(value: bytes) -> str:
    return base64.b64encode(value).decode()


class _Jsonable:
    def __init__(self, value: dict) -> None:
        self.value = value

    def to_jsonable(self) -> dict:
        return self.value


class _Store:
    def __init__(self) -> None:
        self.audio = b""
        self.session_id = ""

    def get_state(self) -> _Jsonable:
        return _Jsonable({"time": "12:34", "wifi": True})

    def start_recording(self, body: dict) -> dict:
        return {"recording": {"session_id": body["session_id"], "status": "recording"}, "state": {}}

    def upload_recording_audio(self, pcm: bytes, **kwargs) -> dict:
        self.audio = pcm
        self.session_id = kwargs["session_id"]
        return {"recording": {"session_id": self.session_id, "status": "audio_ready"}, "state": {}}


def _decode_response(line: str) -> tuple[int, int, dict]:
    parts = line.split(" ")
    return int(parts[2]), int(parts[3]), json.loads(base64.b64decode(parts[4]))


class USBProtocolTests(unittest.TestCase):
    def test_state_request_returns_bridge_state(self) -> None:
        protocol = USBProtocol(_Store())
        line = (
            f"{PROTOCOL_PREFIX} REQUEST 7 GET "
            f"{_b64(b'/state')} {_b64(b'')}"
        )

        request_id, status, payload = _decode_response(protocol.handle_line(line)[0])

        self.assertEqual((request_id, status), (7, 200))
        self.assertEqual(payload["time"], "12:34")
        self.assertEqual(payload["bridge_name"], "vibestick-bridge")

    def test_audio_chunks_are_reassembled_exactly(self) -> None:
        store = _Store()
        protocol = USBProtocol(store)
        audio = bytes(range(256)) * 4
        session_id = "abc123"

        begin = protocol.handle_line(
            f"{PROTOCOL_PREFIX} AUDIO_BEGIN 9 {_b64(session_id.encode())} {len(audio)}"
        )
        self.assertEqual(begin, [f"{PROTOCOL_PREFIX} AUDIO_READY 9"])
        for offset in range(0, len(audio), 127):
            self.assertEqual(
                protocol.handle_line(
                    f"{PROTOCOL_PREFIX} AUDIO_CHUNK 9 {_b64(audio[offset:offset + 127])}"
                ),
                [],
            )
        request_id, status, payload = _decode_response(
            protocol.handle_line(f"{PROTOCOL_PREFIX} AUDIO_END 9")[0]
        )

        self.assertEqual((request_id, status), (9, 200))
        self.assertEqual(payload["recording"]["status"], "audio_ready")
        self.assertEqual(store.audio, audio)
        self.assertEqual(store.session_id, session_id)

    def test_audio_length_mismatch_is_rejected(self) -> None:
        protocol = USBProtocol(_Store())
        protocol.handle_line(f"{PROTOCOL_PREFIX} AUDIO_BEGIN 4 {_b64(b'session')} 10")
        protocol.handle_line(f"{PROTOCOL_PREFIX} AUDIO_CHUNK 4 {_b64(b'short')}")

        _, status, payload = _decode_response(
            protocol.handle_line(f"{PROTOCOL_PREFIX} AUDIO_END 4")[0]
        )

        self.assertEqual(status, 400)
        self.assertIn("Audio length mismatch", payload["error"])


if __name__ == "__main__":
    unittest.main()
