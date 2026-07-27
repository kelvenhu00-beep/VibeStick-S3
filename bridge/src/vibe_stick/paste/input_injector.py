from __future__ import annotations

import platform
import subprocess
import time
from dataclasses import dataclass


@dataclass
class PasteResult:
    success: bool
    message: str


class MacPasteInjector:
    def __init__(self) -> None:
        self._paste_target = ""

    def paste(self, text: str, press_enter: bool = False) -> PasteResult:
        text = text.strip()
        if not text:
            return PasteResult(False, "No text to paste")
        if platform.system() != "Darwin":
            return PasteResult(False, "Automatic paste is only available on macOS")
        paste_target = self._frontmost_target()
        if not paste_target:
            return PasteResult(False, "Could not identify the focused macOS window")

        previous_text = self._read_clipboard()
        set_result = self._set_clipboard(text)
        if not set_result.success:
            return set_result

        script = [
            'tell application "System Events" to keystroke "v" using command down',
        ]
        if press_enter:
            script.extend([
                "delay 0.12",
                'tell application "System Events" to key code 36',
            ])

        args = ["osascript"]
        for line in script:
            args.extend(["-e", line])
        try:
            result = subprocess.run(args, check=False, capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return PasteResult(False, f"macOS paste failed: {exc}")
        time.sleep(0.2)
        if previous_text is not None:
            self._set_clipboard(previous_text)

        if result.returncode != 0:
            message = (result.stderr or result.stdout or "macOS paste failed").strip()
            return PasteResult(False, message)
        self._paste_target = paste_target
        return PasteResult(True, "Pasted into the focused app")

    def press_enter(self) -> PasteResult:
        if platform.system() != "Darwin":
            return PasteResult(False, "Automatic key input is only available on macOS")
        target_check = self._validate_paste_target()
        if target_check is not None:
            return target_check
        try:
            result = subprocess.run(
                ["osascript", "-e", 'tell application "System Events" to key code 36'],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return PasteResult(False, f"macOS key input failed: {exc}")
        if result.returncode != 0:
            message = (result.stderr or result.stdout or "macOS key input failed").strip()
            return PasteResult(False, message)
        self._paste_target = ""
        return PasteResult(True, "Pressed Enter in the focused app")

    def clear_focused_text(self) -> PasteResult:
        if platform.system() != "Darwin":
            return PasteResult(False, "Automatic key input is only available on macOS")
        target_check = self._validate_paste_target()
        if target_check is not None:
            return target_check
        try:
            result = subprocess.run(
                [
                    "osascript",
                    "-e",
                    'tell application "System Events" to keystroke "a" using command down',
                    "-e",
                    "delay 0.08",
                    "-e",
                    'tell application "System Events" to key code 51',
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return PasteResult(False, f"macOS key input failed: {exc}")
        if result.returncode != 0:
            message = (result.stderr or result.stdout or "macOS key input failed").strip()
            return PasteResult(False, message)
        self._paste_target = ""
        return PasteResult(True, "Cleared the focused text field")

    def _validate_paste_target(self) -> PasteResult | None:
        current = self._frontmost_target()
        if not self._paste_target or not current:
            return PasteResult(False, "Focused macOS window could not be verified")
        if current != self._paste_target:
            return PasteResult(False, "Focused window changed after transcription; action cancelled")
        return None

    def _frontmost_target(self) -> str:
        script = "\n".join(
            [
                'tell application "System Events"',
                "set frontProcess to first application process whose frontmost is true",
                'set windowName to ""',
                "try",
                "set windowName to name of front window of frontProcess",
                "end try",
                "return (unix id of frontProcess as text) & tab & "
                "(bundle identifier of frontProcess as text) & tab & windowName",
                "end tell",
            ]
        )
        try:
            result = subprocess.run(
                ["osascript", "-e", script],
                check=False,
                capture_output=True,
                text=True,
                timeout=3,
            )
        except (OSError, subprocess.TimeoutExpired):
            return ""
        return result.stdout.strip() if result.returncode == 0 else ""

    def _read_clipboard(self) -> str | None:
        try:
            result = subprocess.run(
                ["pbpaste"],
                check=False,
                capture_output=True,
                text=True,
                timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if result.returncode != 0:
            return None
        return result.stdout

    def _set_clipboard(self, text: str) -> PasteResult:
        try:
            result = subprocess.run(
                ["pbcopy"],
                input=text,
                check=False,
                capture_output=True,
                text=True,
                timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return PasteResult(False, f"Clipboard write failed: {exc}")
        if result.returncode != 0:
            message = (result.stderr or "Clipboard write failed").strip()
            return PasteResult(False, message)
        return PasteResult(True, "Clipboard updated")
