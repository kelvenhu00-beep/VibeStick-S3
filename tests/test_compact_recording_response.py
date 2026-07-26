from __future__ import annotations

import unittest

from vibe_stick.server.app import _compact_recording_response


class CompactRecordingResponseTests(unittest.TestCase):
    def test_omits_large_transcript_and_audio_path(self) -> None:
        response = {
            "recording": {
                "session_id": "abc123",
                "status": "pasted",
                "pasted": True,
                "transcript": "很长的识别文字" * 500,
                "audio_file": "/tmp/large.wav",
            },
            "state": {"active_provider": "codex"},
        }

        compact = _compact_recording_response(response)

        self.assertEqual(
            compact["recording"],
            {"session_id": "abc123", "status": "pasted", "pasted": True},
        )
        self.assertNotIn("transcript", compact["recording"])
        self.assertEqual(compact["state"], {"active_provider": "codex"})


if __name__ == "__main__":
    unittest.main()
