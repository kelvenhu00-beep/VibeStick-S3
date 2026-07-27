from __future__ import annotations

import unittest
from unittest import mock

from vibe_stick.paste.input_injector import MacPasteInjector


class InputInjectorTests(unittest.TestCase):
    @mock.patch("vibe_stick.paste.input_injector.platform.system", return_value="Darwin")
    def test_confirm_is_rejected_after_focus_changes(self, _system) -> None:
        injector = MacPasteInjector()
        injector._paste_target = "10\tcom.example.Editor\tDraft"
        injector._frontmost_target = lambda: "11\tcom.example.Browser\tPage"

        result = injector.press_enter()

        self.assertFalse(result.success)
        self.assertIn("changed", result.message)

    @mock.patch("vibe_stick.paste.input_injector.subprocess.run")
    @mock.patch("vibe_stick.paste.input_injector.platform.system", return_value="Darwin")
    def test_confirm_runs_for_same_focus_target(self, _system, run) -> None:
        run.return_value = mock.Mock(returncode=0, stdout="", stderr="")
        injector = MacPasteInjector()
        injector._paste_target = "10\tcom.example.Editor\tDraft"
        injector._frontmost_target = lambda: "10\tcom.example.Editor\tDraft"

        result = injector.press_enter()

        self.assertTrue(result.success)
        self.assertEqual(injector._paste_target, "")


if __name__ == "__main__":
    unittest.main()
