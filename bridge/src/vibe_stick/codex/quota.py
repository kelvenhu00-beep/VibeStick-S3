from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from vibe_stick.config.atomic_file import atomic_write_text_if_changed


@dataclass
class QuotaSnapshot:
    quota_5h_remaining: int | None = None
    quota_7d_remaining: int | None = None
    quota_updated_at: str = ""
    quota_stale: bool = False

    def to_jsonable(self) -> dict[str, Any]:
        return asdict(self)


def load_quota(path: Path) -> QuotaSnapshot:
    try:
        data = json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return QuotaSnapshot()
    return QuotaSnapshot(
        quota_5h_remaining=_percent_or_none(data.get("quota_5h_remaining")),
        quota_7d_remaining=_percent_or_none(data.get("quota_7d_remaining")),
        quota_updated_at=str(data.get("quota_updated_at") or ""),
        quota_stale=bool(data.get("quota_stale", False)),
    )


def save_quota(path: Path, snapshot: QuotaSnapshot) -> None:
    atomic_write_text_if_changed(
        path,
        json.dumps(snapshot.to_jsonable(), indent=2) + "\n",
    )


def _percent_or_none(value: object) -> int | None:
    if value is None:
        return None
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return max(0, min(100, number))
