from __future__ import annotations

import json
import time
from typing import Any

from vibe_stick.config.atomic_file import atomic_write_text_if_changed
from vibe_stick.config.paths import HUD_STATE_PATH, ensure_app_support

HUD_TEXT = {
    "listening": "正在聆听",
    "sending": "正在发送",
    "transcribing": "正在识别",
    "unclear": "未听清",
    "failed": "识别失败",
}


def show_hud(status: str, *, hold_seconds: float | None = None) -> None:
    text = HUD_TEXT.get(status, status)
    now = time.time()
    _write_hud_state(
        {
            "active": True,
            "status": status,
            "text": text,
            "updated_at_epoch": now,
            "expires_at_epoch": now + hold_seconds if hold_seconds else None,
        }
    )


def hide_hud(*, delay_seconds: float = 0.0) -> None:
    if delay_seconds > 0:
        show_hud("transcribing", hold_seconds=delay_seconds)
        return
    _write_hud_state(
        {
            "active": False,
            "status": "idle",
            "text": "",
            "updated_at_epoch": time.time(),
            "expires_at_epoch": None,
        }
    )


def _write_hud_state(payload: dict[str, Any]) -> None:
    ensure_app_support()
    data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"
    try:
        atomic_write_text_if_changed(HUD_STATE_PATH, data)
    except OSError as exc:
        print(f"hud state write failed path={HUD_STATE_PATH} error={exc}", flush=True)
