from __future__ import annotations

import unittest

from vibe_stick.codex.local_observer import _is_codex_process_command


class CodexProcessDetectionTests(unittest.TestCase):
    def test_detects_chatgpt_bundled_codex_app_server(self) -> None:
        command = (
            "/Applications/ChatGPT.app/Contents/Resources/codex "
            "-c features.code_mode_host=true app-server --analytics-default-enabled"
        )
        self.assertTrue(_is_codex_process_command(command))

    def test_rejects_unrelated_chatgpt_process(self) -> None:
        command = "/Applications/ChatGPT.app/Contents/MacOS/ChatGPT"
        self.assertFalse(_is_codex_process_command(command))


if __name__ == "__main__":
    unittest.main()
