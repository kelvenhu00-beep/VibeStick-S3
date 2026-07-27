from __future__ import annotations

import tempfile
import struct
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
        recordings_patch = patch(
            "vibe_stick.audio.recorder.RECORDINGS_DIR",
            Path(directory.name) / "Recordings",
        )
        recordings_patch.start()
        self.addCleanup(recordings_patch.stop)
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

    @patch("vibe_stick.audio.recorder.show_hud")
    def test_sticks3_stop_is_rejected_until_audio_upload_is_confirmed(self, _show_hud) -> None:
        controller = self.make_controller()
        controller.start(
            {
                "source": "sticks3",
                "audio_source": "sticks3_pcm",
                "session_id": "session-a",
            }
        )

        rejected = controller.stop({"session_id": "session-a", "text": "不应执行"})

        self.assertEqual(rejected.status, "stop_rejected")
        self.assertTrue(controller.session.active)
        self.assertEqual(controller.session.status, "recording")

    @patch("vibe_stick.audio.recorder.show_hud")
    def test_mismatched_audio_does_not_replace_active_session(self, _show_hud) -> None:
        controller = self.make_controller()
        controller.start(
            {
                "source": "sticks3",
                "audio_source": "sticks3_pcm",
                "session_id": "session-a",
            }
        )

        rejected = controller.attach_pcm(
            struct.pack("<8h", *range(8)),
            session_id="session-b",
        )

        self.assertEqual(rejected.status, "audio_rejected")
        self.assertEqual(controller.session.session_id, "session-a")
        self.assertEqual(controller.session.status, "recording")
        self.assertTrue(controller.session.active)

    @patch("vibe_stick.audio.recorder.hide_hud")
    @patch("vibe_stick.audio.recorder.show_hud")
    def test_confirmed_audio_can_transition_to_pasted(self, _show_hud, _hide_hud) -> None:
        controller = self.make_controller()
        controller.paste_injector.paste = lambda text, press_enter=False: PasteResult(True, "pasted")
        controller.start(
            {
                "source": "sticks3",
                "audio_source": "sticks3_pcm",
                "session_id": "session-a",
            }
        )

        uploaded = controller.attach_pcm(
            struct.pack("<160h", *([1200] * 160)),
            session_id="session-a",
        )
        uploaded_status = uploaded.status
        stopped = controller.stop(
            {
                "session_id": "session-a",
                "text": "测试文字",
                "paste": True,
            }
        )

        self.assertEqual(uploaded_status, "audio_ready")
        self.assertEqual(stopped.status, "pasted")
        self.assertFalse(stopped.active)

    @patch("vibe_stick.audio.recorder.show_hud")
    def test_new_start_cannot_replace_audio_ready_session(self, _show_hud) -> None:
        controller = self.make_controller()
        controller.start(
            {
                "source": "sticks3",
                "audio_source": "sticks3_pcm",
                "session_id": "session-a",
            }
        )
        controller.attach_pcm(
            struct.pack("<8h", *range(8)),
            session_id="session-a",
        )

        rejected = controller.start(
            {
                "source": "sticks3",
                "audio_source": "sticks3_pcm",
                "session_id": "session-b",
            }
        )

        self.assertEqual(rejected.status, "start_conflict")
        self.assertEqual(controller.session.session_id, "session-a")
        self.assertEqual(controller.session.status, "audio_ready")


if __name__ == "__main__":
    unittest.main()
