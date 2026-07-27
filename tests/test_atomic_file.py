from __future__ import annotations

import tempfile
import time
import unittest
from pathlib import Path

from vibe_stick.config.atomic_file import atomic_write_text_if_changed, clear_atomic_write_cache


class AtomicFileTests(unittest.TestCase):
    def setUp(self) -> None:
        clear_atomic_write_cache()

    def test_unchanged_content_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            self.assertTrue(atomic_write_text_if_changed(path, '{"ok":true}\n'))
            first_mtime = path.stat().st_mtime_ns
            time.sleep(0.001)

            changed = atomic_write_text_if_changed(path, '{"ok":true}\n')

            self.assertFalse(changed)
            self.assertEqual(path.stat().st_mtime_ns, first_mtime)

    def test_changed_content_replaces_complete_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            atomic_write_text_if_changed(path, "old\n")

            self.assertTrue(atomic_write_text_if_changed(path, "new\n"))

            self.assertEqual(path.read_text(), "new\n")
            self.assertEqual(list(path.parent.glob("*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
