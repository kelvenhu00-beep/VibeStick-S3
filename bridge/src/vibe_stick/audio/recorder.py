from __future__ import annotations

import json
import math
import os
import signal
import struct
import subprocess
import threading
import time
import wave
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime
from functools import wraps
from pathlib import Path
from typing import Any, Callable, TypeVar

from vibe_stick.audio.transcriber import TranscriptionAdapter
from vibe_stick.config.atomic_file import atomic_write_text_if_changed
from vibe_stick.config.paths import RECORDINGS_DIR
from vibe_stick.desktop.hud import hide_hud, show_hud
from vibe_stick.paste.input_injector import MacPasteInjector

MIN_AUDIO_DURATION_SECONDS = 0.7
MIN_AUDIO_RMS = 120.0
MIN_SPEECH_SECONDS = 0.28
MIN_SPEECH_WINDOWS = 3
SPEECH_WINDOW_SECONDS = 0.10
SPEECH_RMS_THRESHOLD = 900.0
SPEECH_EDGE_IGNORE_SECONDS = 0.18
RECORDING_STALE_SECONDS = 180
KNOWN_ASR_HALLUCINATIONS = (
    "\u8bf7\u4e0d\u541d\u70b9\u8d5e\u8ba2\u9605\u8f6c\u53d1\u6253\u8d4f\u652f\u6301\u660e\u955c\u4e0e\u70b9\u70b9\u680f\u76ee",
    "\u8bf7\u4f7f\u7528\u7b80\u4f53\u4e2d\u6587\u8f93\u51fa\u3002",
)
_RecordingMethod = TypeVar("_RecordingMethod", bound=Callable[..., Any])


def _session_locked(method: _RecordingMethod) -> _RecordingMethod:
    @wraps(method)
    def wrapper(self: "RecordingController", *args: Any, **kwargs: Any) -> Any:
        with self._session_lock:
            return method(self, *args, **kwargs)

    return wrapper  # type: ignore[return-value]


@dataclass
class RecordingSession:
    session_id: str = ""
    active: bool = False
    started_at: str = ""
    stopped_at: str = ""
    status: str = "idle"
    message: str = ""
    transcript: str = ""
    transcript_source: str = "none"
    pasted: bool = False
    audio_file: str = ""
    audio_source: str = "none"

    def to_jsonable(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class AudioMetrics:
    duration_seconds: float
    audio_bytes: int
    rms: float
    ac_rms: float
    speech_seconds: float
    speech_windows: int


class RecordingController:
    """project-owned push-to-talk session boundary."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._session_lock = threading.RLock()
        self.transcriber = TranscriptionAdapter()
        self.paste_injector = MacPasteInjector()
        self.audio_recorder = MacMicRecorder()
        self.session = self._load()

    @_session_locked
    def start(self, request: dict[str, Any] | None = None) -> RecordingSession:
        request = request or {}
        requested_source = str(request.get("audio_source") or request.get("source") or "")
        requested_session_id = _requested_session_id(request)
        if self.session.active:
            if requested_session_id and requested_session_id == self.session.session_id:
                return self.session
            if self.session.status != "recording" or self.session.audio_source != "sticks3_pcm":
                return self._rejected(
                    "start_conflict",
                    "Another recording session is already active",
                    requested_session_id,
                )
            print(
                "recording session superseded "
                f"old_session={self.session.session_id} new_session={requested_session_id}",
                flush=True,
            )
        self.session = RecordingSession(
            session_id=requested_session_id or uuid.uuid4().hex,
            active=True,
            started_at=datetime.now().isoformat(timespec="seconds"),
            stopped_at="",
            status="recording",
            message="Recording session started",
        )
        use_mac_mic = "sticks3" not in requested_source.lower()
        if not use_mac_mic:
            self.session.audio_source = "sticks3_pcm"
            self.session.message = "Waiting for StickS3 audio upload"
            show_hud("listening")

        mic_result = self.audio_recorder.start(self.session.session_id) if use_mac_mic else None
        if mic_result is not None:
            ok, audio_file, message = mic_result
            self.session.audio_file = str(audio_file) if audio_file else ""
            self.session.audio_source = "mac_mic"
            self.session.message = message
            if not ok:
                self.session.active = False
                self.session.stopped_at = datetime.now().isoformat(timespec="seconds")
                self.session.status = "start_failed"
                show_hud("failed", hold_seconds=1.8)
                self._save()
                return self.session
            show_hud("listening")

        hook = _run_command_hook("VIBE_STICK_RECORDING_START_CMD", self.session.to_jsonable(), timeout=15)
        if hook is not None and not hook[0]:
            self.audio_recorder.stop()
            self.session.active = False
            self.session.stopped_at = datetime.now().isoformat(timespec="seconds")
            self.session.status = "start_failed"
            self.session.message = hook[2] or "Recording start command failed"
            show_hud("failed", hold_seconds=1.8)
        self._save()
        return self.session

    @_session_locked
    def attach_pcm(
        self,
        pcm: bytes,
        *,
        session_id: str = "",
        sample_rate: int = 16000,
        channels: int = 1,
        bits_per_sample: int = 16,
    ) -> RecordingSession:
        session_id = _clean_session_id(session_id)
        if not self.session.active or self.session.status not in {"recording", "audio_ready"}:
            return self._rejected(
                "audio_rejected",
                "No recording session is waiting for audio",
                session_id,
            )
        if not session_id or session_id != self.session.session_id:
            return self._rejected(
                "audio_rejected",
                "Uploaded audio session did not match active recording",
                session_id,
            )
        if self.session.status == "audio_ready" and self.session.audio_file:
            return self.session
        if not pcm:
            rejected = self._rejected(
                "audio_failed",
                "Uploaded audio was empty",
                session_id,
            )
            show_hud("failed", hold_seconds=1.8)
            return rejected
        if bits_per_sample != 16:
            rejected = self._rejected(
                "audio_failed",
                "Only 16-bit PCM audio is supported",
                session_id,
            )
            show_hud("failed", hold_seconds=1.8)
            return rejected

        RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
        sid = self.session.session_id or session_id or uuid.uuid4().hex
        audio_file = RECORDINGS_DIR / f"{sid}.wav"
        with wave.open(str(audio_file), "wb") as wav:
            wav.setnchannels(max(1, channels))
            wav.setsampwidth(bits_per_sample // 8)
            wav.setframerate(sample_rate)
            wav.writeframes(pcm)

        self.session.audio_file = str(audio_file)
        self.session.audio_source = "sticks3_pcm"
        self.session.status = "audio_ready"
        self.session.message = "StickS3 audio uploaded"
        show_hud("sending")
        self._save()
        return self.session

    @_session_locked
    def stop(self, request: dict[str, Any] | None = None) -> RecordingSession:
        request = request or {}
        requested_session_id = _requested_session_id(request)
        if requested_session_id and requested_session_id != self.session.session_id:
            return self._rejected(
                "stop_rejected",
                "Stop request did not match active recording",
                requested_session_id,
            )
        if not self.session.active:
            return self.session
        if self.session.audio_source == "sticks3_pcm" and (
            self.session.status != "audio_ready" or not self.session.audio_file
        ):
            return self._rejected(
                "stop_rejected",
                "Audio upload must be confirmed before stopping",
                requested_session_id,
            )
        self.session.active = False
        self.session.stopped_at = datetime.now().isoformat(timespec="seconds")
        self.session.status = "processing"
        self.session.message = "Processing uploaded audio"
        self._save()
        explicit_text = str(request.get("text") or request.get("transcript") or "")
        mic_stop = self.audio_recorder.stop()
        if mic_stop is not None:
            ok, audio_file, message = mic_stop
            self.session.audio_file = str(audio_file) if audio_file else self.session.audio_file
            self.session.audio_source = "mac_mic"
            if not ok:
                self.session.status = "stop_failed"
                self.session.message = message
                show_hud("failed", hold_seconds=1.8)
                self._save_stop_result()
                return self.session
        stop_hook_source = False
        stop_hook = _run_command_hook("VIBE_STICK_RECORDING_STOP_CMD", self.session.to_jsonable(), timeout=120)
        if stop_hook is not None:
            hook_ok, hook_stdout, hook_stderr = stop_hook
            if hook_ok and hook_stdout.strip():
                explicit_text = hook_stdout.strip()
                stop_hook_source = True
            elif not hook_ok:
                self.session.status = "stop_failed"
                self.session.message = hook_stderr or "Recording stop command failed"
                show_hud("failed", hold_seconds=1.8)
                self._save_stop_result()
                return self.session
        should_paste = bool(request.get("paste", True))
        press_enter = _env_bool("VIBE_STICK_AUTO_ENTER", default=False)
        show_hud("transcribing")
        stop_started_at = time.monotonic()

        if not explicit_text:
            metrics = _wav_metrics(self.session.audio_file)
            if metrics is not None:
                print(
                    "recording audio metrics "
                    f"session={self.session.session_id} "
                    f"file={self.session.audio_file} "
                    f"bytes={metrics.audio_bytes} "
                    f"duration={metrics.duration_seconds:.3f}s "
                    f"rms={metrics.rms:.1f} "
                    f"ac_rms={metrics.ac_rms:.1f} "
                    f"speech_seconds={metrics.speech_seconds:.2f} "
                    f"speech_windows={metrics.speech_windows}",
                    flush=True,
                )
                if metrics.duration_seconds < MIN_AUDIO_DURATION_SECONDS:
                    self.session.pasted = False
                    self.session.status = "audio_skipped"
                    self.session.message = (
                        f"Audio too short for transcription: {metrics.duration_seconds:.2f}s"
                    )
                    show_hud("unclear", hold_seconds=1.8)
                    self._save_stop_result()
                    return self.session
                if metrics.rms < MIN_AUDIO_RMS:
                    self.session.pasted = False
                    self.session.status = "audio_skipped"
                    self.session.message = f"Audio appears silent: rms={metrics.rms:.1f}"
                    show_hud("unclear", hold_seconds=1.8)
                    self._save_stop_result()
                    return self.session
                if (
                    metrics.speech_seconds < MIN_SPEECH_SECONDS
                    or metrics.speech_windows < MIN_SPEECH_WINDOWS
                ):
                    self.session.pasted = False
                    self.session.status = "audio_skipped"
                    self.session.message = (
                        "No clear speech detected before transcription: "
                        f"speech_seconds={metrics.speech_seconds:.2f}"
                    )
                    show_hud("unclear", hold_seconds=1.8)
                    self._save_stop_result()
                    return self.session

        transcription_started_at = time.monotonic()
        transcript = self.transcriber.transcribe(
            self.session.to_jsonable(),
            explicit_text=explicit_text,
        )
        transcription_ms = round((time.monotonic() - transcription_started_at) * 1000)
        self.session.transcript_source = "recording_stop_cmd" if stop_hook_source else transcript.source
        if transcript.success and transcript.text:
            self.session.transcript = transcript.text
            transcript_message = (
                "Transcript supplied by recording stop command"
                if stop_hook_source else transcript.message
            )
            rejection_reason = _transcript_rejection_reason(transcript.text)
            if rejection_reason:
                self.session.pasted = False
                self.session.status = "transcript_rejected"
                self.session.message = rejection_reason
                show_hud("unclear", hold_seconds=1.8)
                print(
                    "recording transcript rejected "
                    f"session={self.session.session_id} "
                    f"source={self.session.transcript_source} "
                    f"reason={rejection_reason}",
                    flush=True,
                )
                self._save_stop_result()
                return self.session
            if should_paste:
                paste = self.paste_injector.paste(transcript.text, press_enter=press_enter)
                self.session.pasted = paste.success
                self.session.status = "pasted" if paste.success else "paste_failed"
                self.session.message = paste.message if paste.success else f"{transcript_message}; {paste.message}"
                if paste.success:
                    hide_hud(delay_seconds=0.5)
                else:
                    show_hud("failed", hold_seconds=1.8)
            else:
                self.session.pasted = False
                self.session.status = "transcribed"
                self.session.message = transcript_message
                hide_hud(delay_seconds=0.5)
        else:
            self.session.pasted = False
            self.session.status = "transcription_failed"
            self.session.message = transcript.message
            show_hud("failed", hold_seconds=1.8)
        stop_total_ms = round((time.monotonic() - stop_started_at) * 1000)
        print(
            "recording processing timing "
            f"session={self.session.session_id} "
            f"transcription_ms={transcription_ms} "
            f"stop_total_ms={stop_total_ms}",
            flush=True,
        )
        self._save_stop_result()
        return self.session

    @_session_locked
    def confirm(self) -> RecordingSession:
        if self.session.status != "pasted" or not self.session.transcript.strip():
            self.session.status = "confirm_failed"
            self.session.message = "No pasted transcript is waiting to be submitted"
            self._save_stop_result()
            return self.session

        submit = self.paste_injector.press_enter()
        self.session.status = "submitted" if submit.success else "submit_failed"
        self.session.message = submit.message
        if submit.success:
            hide_hud(delay_seconds=0.5)
        else:
            show_hud("failed", hold_seconds=1.8)
        self._save_stop_result()
        return self.session

    @_session_locked
    def cancel(self) -> RecordingSession:
        if self.session.status != "pasted":
            self.session.status = "cancel_failed"
            self.session.message = "No pasted transcript is waiting for a decision"
            self._save_stop_result()
            return self.session

        clear = self.paste_injector.clear_focused_text()
        self.session.pasted = not clear.success
        self.session.status = "cleared" if clear.success else "clear_failed"
        self.session.message = clear.message
        if clear.success:
            self.session.transcript = ""
            hide_hud(delay_seconds=0.2)
        else:
            show_hud("failed", hold_seconds=1.8)
        self._save_stop_result()
        return self.session

    @_session_locked
    def expire_if_stale(self, max_age_seconds: int = RECORDING_STALE_SECONDS) -> bool:
        if not self.session.active or not self.session.started_at:
            return False
        try:
            started_at = datetime.fromisoformat(self.session.started_at)
        except ValueError:
            return False
        age_seconds = (datetime.now() - started_at).total_seconds()
        if age_seconds < max_age_seconds:
            return False

        self.audio_recorder.stop()
        self.session.active = False
        self.session.stopped_at = datetime.now().isoformat(timespec="seconds")
        self.session.status = "recording_timeout"
        self.session.message = (
            f"Recording expired after {int(age_seconds)} seconds without a stop request"
        )
        hide_hud(delay_seconds=0.2)
        self._save_stop_result()
        return True

    @_session_locked
    def current_audio_file(self) -> str:
        return self.session.audio_file

    def _load(self) -> RecordingSession:
        try:
            data = json.loads(self.path.read_text())
        except (FileNotFoundError, json.JSONDecodeError, OSError):
            return RecordingSession()
        return RecordingSession(
            session_id=str(data.get("session_id") or ""),
            active=bool(data.get("active", False)),
            started_at=str(data.get("started_at") or ""),
            stopped_at=str(data.get("stopped_at") or ""),
            status=str(data.get("status") or "idle"),
            message=str(data.get("message") or ""),
            transcript=str(data.get("transcript") or ""),
            transcript_source=str(data.get("transcript_source") or "none"),
            pasted=bool(data.get("pasted", False)),
            audio_file=str(data.get("audio_file") or ""),
            audio_source=str(data.get("audio_source") or "none"),
        )

    def _save(self) -> None:
        atomic_write_text_if_changed(
            self.path,
            json.dumps(self.session.to_jsonable(), indent=2) + "\n",
        )

    def _save_stop_result(self) -> None:
        self._save()
        print(
            "recording stop result "
            f"session={self.session.session_id} "
            f"status={self.session.status} "
            f"source={self.session.transcript_source} "
            f"audio_source={self.session.audio_source} "
            f"transcript_chars={len(self.session.transcript)} "
            f"pasted={self.session.pasted} "
            f"message={self.session.message}",
            flush=True,
        )

    def _rejected(self, status: str, message: str, session_id: str = "") -> RecordingSession:
        return RecordingSession(
            session_id=session_id or self.session.session_id,
            active=False,
            started_at=self.session.started_at,
            stopped_at=self.session.stopped_at,
            status=status,
            message=message,
            audio_source=self.session.audio_source,
        )


def _env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _wav_metrics(audio_file: str) -> AudioMetrics | None:
    if not audio_file:
        return None
    path = Path(audio_file)
    if not path.is_file() or path.suffix.lower() != ".wav":
        return None
    try:
        with wave.open(str(path), "rb") as wav:
            frames = wav.getnframes()
            rate = wav.getframerate()
            sample_width = wav.getsampwidth()
            raw = wav.readframes(frames)
    except (OSError, wave.Error):
        return None

    duration_seconds = frames / rate if rate else 0.0
    if sample_width != 2 or not raw:
        return AudioMetrics(duration_seconds, len(raw), 0.0, 0.0, 0.0, 0)

    usable_len = len(raw) - (len(raw) % 2)
    samples = [sample for (sample,) in struct.iter_unpack("<h", raw[:usable_len])]
    count = len(samples)
    if count == 0:
        return AudioMetrics(duration_seconds, len(raw), 0.0, 0.0, 0.0, 0)
    total = sum(int(sample) * int(sample) for sample in samples)
    rms = math.sqrt(total / count) if count else 0.0
    mean = sum(samples) / count
    ac_rms = math.sqrt(sum((sample - mean) * (sample - mean) for sample in samples) / count)

    window_size = max(1, int(rate * SPEECH_WINDOW_SECONDS)) if rate else count
    edge_windows = int(math.ceil(SPEECH_EDGE_IGNORE_SECONDS / SPEECH_WINDOW_SECONDS))
    window_rms: list[float] = []
    for start in range(0, count, window_size):
        chunk = samples[start:start + window_size]
        if len(chunk) < max(1, window_size // 2):
            continue
        chunk_mean = sum(chunk) / len(chunk)
        chunk_rms = math.sqrt(
            sum((sample - chunk_mean) * (sample - chunk_mean) for sample in chunk) / len(chunk)
        )
        window_rms.append(chunk_rms)
    speech_windows = 0
    for index, value in enumerate(window_rms):
        if index < edge_windows or index >= len(window_rms) - edge_windows:
            continue
        if value >= SPEECH_RMS_THRESHOLD:
            speech_windows += 1
    speech_seconds = speech_windows * SPEECH_WINDOW_SECONDS
    return AudioMetrics(duration_seconds, len(raw), rms, ac_rms, speech_seconds, speech_windows)


def _transcript_rejection_reason(text: str) -> str:
    normalized = _normalized_transcript(text)
    for phrase in KNOWN_ASR_HALLUCINATIONS:
        if phrase in normalized:
            return "Rejected known ASR hallucination transcript"
    return ""


def _normalized_transcript(text: str) -> str:
    return "".join(str(text).split()).lower()


def _requested_session_id(request: dict[str, Any]) -> str:
    return _clean_session_id(str(request.get("session_id") or ""))


def _clean_session_id(raw: str) -> str:
    value = raw.strip()
    if not 8 <= len(value) <= 64:
        return ""
    if not all(ch.isalnum() or ch in {"-", "_"} for ch in value):
        return ""
    return value


def _run_command_hook(
    env_name: str,
    payload: dict[str, Any],
    timeout: int,
) -> tuple[bool, str, str] | None:
    command = os.environ.get(env_name, "").strip()
    if not command:
        return None
    try:
        result = subprocess.run(
            command,
            input=json.dumps(payload),
            shell=True,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return (False, "", str(exc))
    return (result.returncode == 0, result.stdout, result.stderr.strip())


class MacMicRecorder:
    """Small project-owned wrapper around a local AVFoundation recorder helper."""

    def __init__(self) -> None:
        self.process: subprocess.Popen[str] | None = None
        self.audio_file: Path | None = None

    def start(self, session_id: str) -> tuple[bool, Path | None, str] | None:
        if os.environ.get("VIBE_STICK_RECORDING_USE_MAC_MIC", "1").strip().lower() in {"0", "false", "no", "off"}:
            return None
        if self.process and self.process.poll() is None:
            return (False, self.audio_file, "A recording session is already active")

        RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
        self.audio_file = RECORDINGS_DIR / f"{session_id}.m4a"
        binary = self._ensure_helper_binary()
        if binary is None:
            return (False, self.audio_file, "Could not build VibeStick mic recorder helper")

        try:
            self.process = subprocess.Popen(
                [str(binary), str(self.audio_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError as exc:
            self.process = None
            return (False, self.audio_file, f"Could not start mic recorder: {exc}")

        time.sleep(1.0)
        if self.process.poll() is not None:
            _, stderr = self.process.communicate(timeout=1)
            message = stderr.strip() or "Mic recorder exited before recording started"
            self.process = None
            return (False, self.audio_file, message)
        return (True, self.audio_file, "Recording from Mac microphone")

    def stop(self) -> tuple[bool, Path | None, str] | None:
        if not self.process:
            return None
        process = self.process
        audio_file = self.audio_file
        self.process = None
        self.audio_file = None

        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)

        stdout, stderr = process.communicate(timeout=1)
        if process.returncode != 0:
            message = stderr.strip() or stdout.strip() or "Mic recorder stopped with an error"
            return (False, audio_file, message)
        if audio_file is None or not audio_file.exists() or audio_file.stat().st_size == 0:
            return (False, audio_file, "Mic recorder produced no audio")
        return (True, audio_file, "Recording stopped")

    def _ensure_helper_binary(self) -> Path | None:
        source = Path(__file__).resolve().parents[3] / "tools" / "vibe_stick_mic_recorder.swift"
        binary = RECORDINGS_DIR.parent / "vibe_stick_mic_recorder"
        if not source.exists():
            return None
        if binary.exists() and binary.stat().st_mtime >= source.stat().st_mtime:
            return binary
        try:
            result = subprocess.run(
                ["swiftc", str(source), "-o", str(binary), "-framework", "AVFoundation"],
                check=False,
                capture_output=True,
                text=True,
                timeout=45,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if result.returncode != 0:
            return None
        return binary
