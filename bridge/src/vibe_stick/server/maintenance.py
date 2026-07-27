from __future__ import annotations

import os
import shutil
import threading
import time
from pathlib import Path
from typing import Callable

from vibe_stick.config.paths import APP_SUPPORT_DIR, RECORDINGS_DIR


DEFAULT_INTERVAL_SECONDS = 3600
DEFAULT_LOG_MAX_BYTES = 5 * 1024 * 1024
DEFAULT_LOG_BACKUPS = 3
DEFAULT_RECORDING_RETENTION_DAYS = 14
DEFAULT_RECORDING_MAX_FILES = 100


class MaintenanceManager:
    def __init__(self, active_audio_file: Callable[[], str]) -> None:
        self._active_audio_file = active_audio_file
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self.run_once()
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run,
            name="vibestick-maintenance",
            daemon=True,
        )
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=2)
        self._thread = None

    def run_once(self) -> None:
        prune_recordings(active_audio_file=self._active_audio_file())
        for name in ("bridge.log", "bridge.err.log", "hud.log", "hud.err.log"):
            rotate_log(APP_SUPPORT_DIR / name)

    def _run(self) -> None:
        while not self._stop.wait(_int_env(
            "VIBE_STICK_MAINTENANCE_INTERVAL_SECONDS",
            DEFAULT_INTERVAL_SECONDS,
            minimum=60,
            maximum=86400,
        )):
            self.run_once()


def prune_recordings(*, active_audio_file: str = "") -> int:
    try:
        files = [
            path
            for path in RECORDINGS_DIR.iterdir()
            if path.is_file() and path.suffix.lower() in {".wav", ".m4a"}
        ]
    except OSError:
        return 0
    files.sort(key=lambda path: _mtime(path), reverse=True)
    active = Path(active_audio_file).resolve() if active_audio_file else None
    max_files = _int_env(
        "VIBE_STICK_RECORDING_MAX_FILES",
        DEFAULT_RECORDING_MAX_FILES,
        minimum=10,
        maximum=2000,
    )
    retention_seconds = _int_env(
        "VIBE_STICK_RECORDING_RETENTION_DAYS",
        DEFAULT_RECORDING_RETENTION_DAYS,
        minimum=1,
        maximum=365,
    ) * 86400
    cutoff = time.time() - retention_seconds
    removed = 0
    for index, path in enumerate(files):
        if active is not None and path.resolve() == active:
            continue
        if index < max_files and _mtime(path) >= cutoff:
            continue
        try:
            path.unlink()
            removed += 1
        except OSError:
            continue
    return removed


def rotate_log(path: Path) -> bool:
    max_bytes = _int_env(
        "VIBE_STICK_LOG_MAX_BYTES",
        DEFAULT_LOG_MAX_BYTES,
        minimum=256 * 1024,
        maximum=100 * 1024 * 1024,
    )
    backups = _int_env(
        "VIBE_STICK_LOG_BACKUPS",
        DEFAULT_LOG_BACKUPS,
        minimum=1,
        maximum=10,
    )
    try:
        if path.stat().st_size <= max_bytes:
            return False
    except OSError:
        return False

    try:
        oldest = path.with_name(f"{path.name}.{backups}")
        oldest.unlink(missing_ok=True)
        for index in range(backups - 1, 0, -1):
            source = path.with_name(f"{path.name}.{index}")
            target = path.with_name(f"{path.name}.{index + 1}")
            if source.exists():
                os.replace(source, target)
        shutil.copy2(path, path.with_name(f"{path.name}.1"))
        with path.open("wb"):
            pass
    except OSError:
        return False
    return True


def _mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def _int_env(name: str, default: int, *, minimum: int, maximum: int) -> int:
    try:
        value = int(os.environ.get(name, ""))
    except ValueError:
        value = default
    if value <= 0:
        value = default
    return max(minimum, min(maximum, value))
