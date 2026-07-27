from __future__ import annotations

import base64
import binascii
import glob
import json
import os
import select
import termios
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from vibe_stick import __version__ as BRIDGE_VERSION
from vibe_stick.config.wifi_profiles import bootstrap_wifi_profiles, usb_wifi_payload

PROTOCOL_PREFIX = "@VBS1"
USB_PORT_GLOB = "/dev/cu.usbmodem*"
USB_SCAN_INTERVAL_SECONDS = 1.0
USB_HANDSHAKE_INTERVAL_SECONDS = 1.0
USB_MAX_LINE_BYTES = 8192


def _b64_encode(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _b64_decode(text: str) -> bytes:
    return base64.b64decode(text, validate=True)


@dataclass
class _AudioTransfer:
    request_id: int
    session_id: str
    expected_length: int
    data: bytearray


class USBProtocol:
    """Line-framed VibeStick protocol independent from the serial-port loop."""

    def __init__(self, store: Any) -> None:
        self._store = store
        self._audio: _AudioTransfer | None = None

    def handle_line(self, line: str) -> list[str]:
        if not line.startswith(f"{PROTOCOL_PREFIX} "):
            return []
        parts = line.split(" ")
        command = parts[1] if len(parts) > 1 else ""
        try:
            if command == "READY" and len(parts) == 4:
                return []
            if command == "REQUEST" and len(parts) == 6:
                return [self._handle_request(parts)]
            if command == "AUDIO_BEGIN" and len(parts) == 5:
                return [self._handle_audio_begin(parts)]
            if command == "AUDIO_CHUNK" and len(parts) == 4:
                self._handle_audio_chunk(parts)
                return []
            if command == "AUDIO_END" and len(parts) == 3:
                return [self._handle_audio_end(parts)]
        except (ValueError, UnicodeDecodeError, binascii.Error, json.JSONDecodeError) as exc:
            request_id = _request_id(parts)
            self._audio = None
            return [self._response(request_id, 400, {"error": f"Invalid USB frame: {exc}"})]
        return []

    def _handle_request(self, parts: list[str]) -> str:
        request_id = int(parts[2])
        method = parts[3]
        path = _b64_decode(parts[4]).decode("utf-8")
        body_raw = _b64_decode(parts[5])
        body = json.loads(body_raw.decode("utf-8")) if body_raw else {}
        if not isinstance(body, dict):
            raise ValueError("request body must be an object")
        status, payload = self._dispatch(method, path, body)
        return self._response(request_id, status, payload)

    def _handle_audio_begin(self, parts: list[str]) -> str:
        request_id = int(parts[2])
        session_id = _b64_decode(parts[3]).decode("utf-8")
        expected_length = int(parts[4])
        max_length = _max_recording_audio_bytes()
        if expected_length <= 0 or expected_length > max_length:
            self._audio = None
            return self._response(
                request_id,
                413,
                {"error": f"Recording audio exceeds {max_length} bytes"},
            )
        self._audio = _AudioTransfer(
            request_id=request_id,
            session_id=session_id,
            expected_length=expected_length,
            data=bytearray(),
        )
        return f"{PROTOCOL_PREFIX} AUDIO_READY {request_id}"

    def _handle_audio_chunk(self, parts: list[str]) -> None:
        request_id = int(parts[2])
        transfer = self._audio
        if transfer is None or transfer.request_id != request_id:
            raise ValueError("audio transfer is not active")
        chunk = _b64_decode(parts[3])
        if len(transfer.data) + len(chunk) > transfer.expected_length:
            raise ValueError("audio transfer exceeds declared length")
        transfer.data.extend(chunk)

    def _handle_audio_end(self, parts: list[str]) -> str:
        request_id = int(parts[2])
        transfer = self._audio
        self._audio = None
        if transfer is None or transfer.request_id != request_id:
            return self._response(request_id, 409, {"error": "Audio transfer is not active"})
        if len(transfer.data) != transfer.expected_length:
            return self._response(
                request_id,
                400,
                {
                    "error": (
                        f"Audio length mismatch: expected {transfer.expected_length}, "
                        f"received {len(transfer.data)}"
                    )
                },
            )
        payload = self._store.upload_recording_audio(
            bytes(transfer.data),
            session_id=transfer.session_id,
            sample_rate=16000,
            channels=1,
            bits_per_sample=16,
        )
        return self._response(request_id, 200, _maybe_compact(payload, True))

    def _dispatch(self, method: str, raw_path: str, body: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        parsed = urlparse(raw_path)
        path = parsed.path
        compact = _first(parse_qs(parsed.query), "compact") == "1"
        if method == "GET" and path == "/state":
            payload = self._store.get_state().to_jsonable()
            payload["bridge_name"] = "vibestick-bridge"
            payload["bridge_version"] = BRIDGE_VERSION
            return 200, payload
        if method == "GET" and path == "/device/wifi":
            return 200, usb_wifi_payload()
        if method == "POST" and path == "/device/wifi/bootstrap":
            created = bootstrap_wifi_profiles(body)
            return 200, {"created": created}
        if method != "POST":
            return 404, {"error": "Unknown endpoint"}
        if path == "/event":
            return 200, self._store.update_from_event(body).to_jsonable()
        if path == "/quota/refresh":
            state = self._store.refresh_quota()
            return 200, {"refreshed": True, "state": state.to_jsonable()}
        if path == "/recording/start":
            return 200, self._store.start_recording(body)
        if path == "/recording/stop":
            return 200, _maybe_compact(self._store.stop_recording(body), compact)
        if path == "/recording/confirm":
            return 200, _maybe_compact(self._store.confirm_recording(), compact)
        if path == "/recording/cancel":
            return 200, _maybe_compact(self._store.cancel_recording(), compact)
        return 404, {"error": "Unknown endpoint"}

    @staticmethod
    def _response(request_id: int, status: int, payload: dict[str, Any]) -> str:
        encoded = _b64_encode(json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode())
        return f"{PROTOCOL_PREFIX} RESPONSE {request_id} {status} {encoded}"


class USBTransport:
    def __init__(self, store: Any) -> None:
        self._protocol = USBProtocol(store)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._fd: int | None = None
        self._port = ""
        self._state_request_logged = False
        self._quiet_response_ids: set[str] = set()

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="vibestick-usb", daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._close_port()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2)

    def _run(self) -> None:
        while not self._stop.is_set():
            if self._fd is None and not self._open_port():
                self._stop.wait(USB_SCAN_INTERVAL_SECONDS)
                continue
            try:
                self._serve_port()
            except OSError as exc:
                print(f"VibeStick USB disconnected port={self._port}: {exc}", flush=True)
            finally:
                self._close_port()

    def _open_port(self) -> bool:
        for raw_path in sorted(glob.glob(USB_PORT_GLOB)):
            path = Path(raw_path)
            try:
                fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                _configure_raw(fd)
            except OSError:
                continue
            self._fd = fd
            self._port = str(path)
            print(f"VibeStick USB port opened: {path}", flush=True)
            return True
        return False

    def _serve_port(self) -> None:
        assert self._fd is not None
        buffer = bytearray()
        last_handshake = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now - last_handshake >= USB_HANDSHAKE_INTERVAL_SECONDS:
                self._write_line(f"{PROTOCOL_PREFIX} HELLO {BRIDGE_VERSION}")
                last_handshake = now
            readable, _, _ = select.select([self._fd], [], [], 0.2)
            if not readable:
                continue
            chunk = os.read(self._fd, 4096)
            if not chunk:
                raise OSError("serial port returned EOF")
            buffer.extend(chunk)
            if len(buffer) > USB_MAX_LINE_BYTES * 2:
                del buffer[:-USB_MAX_LINE_BYTES]
            while b"\n" in buffer:
                raw_line, _, remainder = buffer.partition(b"\n")
                buffer = bytearray(remainder)
                line = raw_line.rstrip(b"\r").decode("utf-8", errors="replace")
                if line.startswith(f"{PROTOCOL_PREFIX} READY "):
                    print(f"VibeStick USB handshake ready port={self._port}", flush=True)
                    last_handshake = float("inf")
                elif line.startswith(f"{PROTOCOL_PREFIX} REQUEST "):
                    fields = line.split(" ", 4)
                    request_id = fields[2] if len(fields) > 2 else "-"
                    method = fields[3] if len(fields) > 3 else "-"
                    if method == "GET":
                        self._quiet_response_ids.add(request_id)
                        if not self._state_request_logged:
                            print(
                                f"VibeStick USB state request id={request_id}",
                                flush=True,
                            )
                    else:
                        print(
                            f"VibeStick USB request id={request_id} method={method}",
                            flush=True,
                        )
                elif line.startswith(f"{PROTOCOL_PREFIX} AUDIO_BEGIN "):
                    fields = line.split(" ", 4)
                    request_id = fields[2] if len(fields) > 2 else "-"
                    length = fields[4] if len(fields) > 4 else "-"
                    print(
                        f"VibeStick USB audio begin id={request_id} bytes={length}",
                        flush=True,
                    )
                elif "bridge GET /state transport=USB result=ESP_OK" in line:
                    if not self._state_request_logged:
                        print(f"VibeStick device: {line}", flush=True)
                        self._state_request_logged = True
                elif "vibe_usb:" in line or line.startswith(("W (", "E (")):
                    print(f"VibeStick device: {line}", flush=True)
                for response in self._protocol.handle_line(line):
                    fields = response.split(" ", 4)
                    command = fields[1] if len(fields) > 1 else "-"
                    request_id = fields[2] if len(fields) > 2 else "-"
                    quiet = request_id in self._quiet_response_ids
                    self._quiet_response_ids.discard(request_id)
                    if not quiet or not self._state_request_logged:
                        print(
                            f"VibeStick USB response id={request_id} command={command} "
                            f"bytes={len(response)}",
                            flush=True,
                        )
                    self._write_line(response)

    def _write_line(self, line: str) -> None:
        if self._fd is None:
            return
        data = f"{line}\n".encode("ascii")
        offset = 0
        while offset < len(data):
            _, writable, _ = select.select([], [self._fd], [], 1.0)
            if not writable:
                raise OSError("serial write timed out")
            offset += os.write(self._fd, data[offset:])

    def _close_port(self) -> None:
        fd, self._fd = self._fd, None
        self._port = ""
        self._state_request_logged = False
        self._quiet_response_ids.clear()
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass


def _configure_raw(fd: int) -> None:
    attributes = termios.tcgetattr(fd)
    attributes[0] = 0
    attributes[1] = 0
    attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attributes[3] = 0
    attributes[4] = termios.B115200
    attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)


def _request_id(parts: list[str]) -> int:
    try:
        return int(parts[2])
    except (IndexError, ValueError):
        return 0


def _first(query: dict[str, list[str]], key: str) -> str:
    values = query.get(key) or []
    return values[0] if values else ""


def _max_recording_audio_bytes() -> int:
    raw = os.environ.get("VIBE_STICK_MAX_RECORDING_AUDIO_BYTES", "").strip()
    if not raw:
        return 2_000_000
    try:
        value = int(raw)
    except ValueError:
        return 2_000_000
    return max(256_000, min(8_000_000, value))


def _maybe_compact(response: dict[str, Any], compact: bool) -> dict[str, Any]:
    if not compact:
        return response
    recording = response.get("recording")
    recording = recording if isinstance(recording, dict) else {}
    return {
        "recording": {
            "session_id": recording.get("session_id", ""),
            "status": recording.get("status", ""),
            "pasted": bool(recording.get("pasted", False)),
        },
        "state": response.get("state", {}),
    }
