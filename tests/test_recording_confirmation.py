from __future__ import annotations

import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest.mock import patch

from vibe_stick.audio.recorder import RecordingController
from vibe_stick.paste.input_injector import PasteResult


class RecordingConfirmationTests(unittest.TestCase):
    def make_controller(self) -> RecordingController:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return RecordingController(Path(directory.name) / "recording.json")

    @patch("vibe_stick.audio.recorder.hide_hud")
    def test_confirm_submits_pasted_transcript(self, _hide_hud) -> None:
        controller = self.make_controller()
        controller.session.status = "pasted"
        controller.session.transcript = "你好"
        controller.session.pasted = True
        controller.paste_injector.press_enter = lambda: PasteResult(True, "submitted")

        session = controller.confirm()

        self.assertEqual(session.status, "submitted")
        self.assertTrue(session.pasted)

    @patch("vibe_stick.audio.recorder.hide_hud")
    def test_cancel_clears_focused_text(self, _hide_hud) -> None:
        controller = self.make_controller()
        controller.session.status = "pasted"
        controller.session.transcript = "不要发送"
        controller.session.pasted = True
        controller.paste_injector.clear_focused_text = lambda: PasteResult(True, "cleared")

        session = controller.cancel()

        self.assertEqual(session.status, "cleared")
        self.assertFalse(session.pasted)
        self.assertEqual(session.transcript, "")

    def test_confirm_without_pending_transcript_fails(self) -> None:
        controller = self.make_controller()

        session = controller.confirm()

        self.assertEqual(session.status, "confirm_failed")
        self.assertFalse(session.pasted)

    @patch("vibe_stick.audio.recorder.hide_hud")
    def test_stale_recording_is_expired(self, hide_hud) -> None:
        controller = self.make_controller()
        controller.session.active = True
        controller.session.started_at = datetime.now().isoformat(timespec="seconds")
        controller.session.status = "recording"
        controller.audio_recorder.stop = lambda: None

        expired = controller.expire_if_stale(max_age_seconds=0)

        self.assertTrue(expired)
        self.assertFalse(controller.session.active)
        self.assertEqual(controller.session.status, "recording_timeout")
        hide_hud.assert_called_once()


if __name__ == "__main__":
    unittest.main()
