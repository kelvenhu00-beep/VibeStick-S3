from __future__ import annotations

import os
import threading
from pathlib import Path


_lock = threading.RLock()
_known_content: dict[Path, str] = {}


def atomic_write_text_if_changed(
    path: Path,
    data: str,
    *,
    encoding: str = "utf-8",
    mode: int | None = None,
) -> bool:
    """Atomically replace path only when its complete text content changed."""
    resolved = path.resolve()
    with _lock:
        previous = _known_content.get(resolved)
        if previous is None:
            try:
                previous = path.read_text(encoding=encoding)
            except (FileNotFoundError, OSError, UnicodeError):
                previous = ""
        if previous == data and path.exists():
            if mode is not None:
                os.chmod(path, mode)
            _known_content[resolved] = data
            return False

        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        try:
            temporary.write_text(data, encoding=encoding)
            if mode is not None:
                os.chmod(temporary, mode)
            os.replace(temporary, path)
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        _known_content[resolved] = data
        return True


def clear_atomic_write_cache() -> None:
    with _lock:
        _known_content.clear()
