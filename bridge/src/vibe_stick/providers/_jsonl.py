from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Iterable


def session_files(root: Path, *, max_files: int, pattern: str = "*.jsonl") -> list[Path]:
    if not root.exists():
        return []
    files = [path for path in root.rglob(pattern) if path.is_file()]
    files.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    return files[:max_files]


def tail_json_events(path: Path, *, tail_bytes: int) -> Iterable[dict[str, Any]]:
    try:
        with path.open("rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - tail_bytes))
            data = handle.read().decode("utf-8", errors="ignore")
    except OSError:
        return []

    events: list[dict[str, Any]] = []
    for line in data.splitlines():
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events
