from __future__ import annotations

import json
import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


@dataclass
class _TailCacheEntry:
    inode: int
    size: int
    mtime_ns: int
    tail: bytes
    events: tuple[dict[str, Any], ...]
    accessed_at: float


_cache_lock = threading.RLock()
_tail_cache: dict[tuple[Path, int], _TailCacheEntry] = {}
_file_list_cache: dict[tuple[Path, int, str], tuple[float, tuple[Path, ...]]] = {}
_MAX_TAIL_CACHE_ENTRIES = 128
_FILE_LIST_CACHE_SECONDS = 1.0


def session_files(root: Path, *, max_files: int, pattern: str = "*.jsonl") -> list[Path]:
    if not root.exists():
        return []
    files = [path for path in root.rglob(pattern) if path.is_file()]
    files.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    return files[:max_files]


def cached_session_files(root: Path, *, max_files: int, pattern: str = "*.jsonl") -> list[Path]:
    key = (root.resolve(), max_files, pattern)
    now = time.monotonic()
    with _cache_lock:
        cached = _file_list_cache.get(key)
        if cached is not None and now - cached[0] < _FILE_LIST_CACHE_SECONDS:
            return list(cached[1])
    files = session_files(root, max_files=max_files, pattern=pattern)
    with _cache_lock:
        _file_list_cache[key] = (now, tuple(files))
    return files


def tail_json_events(path: Path, *, tail_bytes: int) -> Iterable[dict[str, Any]]:
    return _parse_events(_read_tail(path, tail_bytes))


def cached_tail_json_events(path: Path, *, tail_bytes: int) -> Iterable[dict[str, Any]]:
    try:
        stat = path.stat()
    except OSError:
        return []
    key = (path.resolve(), tail_bytes)
    now = time.monotonic()
    with _cache_lock:
        cached = _tail_cache.get(key)
        if (
            cached is not None
            and cached.inode == stat.st_ino
            and cached.size == stat.st_size
            and cached.mtime_ns == stat.st_mtime_ns
        ):
            cached.accessed_at = now
            return list(cached.events)

    if cached is not None and cached.inode == stat.st_ino and stat.st_size >= cached.size:
        try:
            with path.open("rb") as handle:
                handle.seek(cached.size)
                appended = handle.read()
            tail = (cached.tail + appended)[-tail_bytes:]
        except OSError:
            return []
    else:
        tail = _read_tail(path, tail_bytes)
    events = tuple(_parse_events(tail))
    with _cache_lock:
        _tail_cache[key] = _TailCacheEntry(
            inode=stat.st_ino,
            size=stat.st_size,
            mtime_ns=stat.st_mtime_ns,
            tail=tail,
            events=events,
            accessed_at=now,
        )
        if len(_tail_cache) > _MAX_TAIL_CACHE_ENTRIES:
            oldest = min(_tail_cache, key=lambda item: _tail_cache[item].accessed_at)
            _tail_cache.pop(oldest, None)
    return list(events)


def clear_jsonl_caches() -> None:
    with _cache_lock:
        _tail_cache.clear()
        _file_list_cache.clear()


def _read_tail(path: Path, tail_bytes: int) -> bytes:
    try:
        with path.open("rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - tail_bytes))
            return handle.read()
    except OSError:
        return b""


def _parse_events(data: bytes) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for raw_line in data.splitlines():
        line = raw_line.decode("utf-8", errors="ignore")
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events
