from __future__ import annotations

import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from vibe_stick.server import maintenance


class MaintenanceTests(unittest.TestCase):
    def test_prunes_old_recordings_but_keeps_active_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            recordings = Path(tmp)
            active = recordings / "active.wav"
            old = recordings / "old.wav"
            active.write_bytes(b"active")
            old.write_bytes(b"old")
            old_time = time.time() - 3 * 86400
            os.utime(old, (old_time, old_time))

            with mock.patch.object(maintenance, "RECORDINGS_DIR", recordings):
                with mock.patch.dict(
                    os.environ,
                    {"VIBE_STICK_RECORDING_RETENTION_DAYS": "1"},
                ):
                    removed = maintenance.prune_recordings(active_audio_file=str(active))

            self.assertEqual(removed, 1)
            self.assertTrue(active.exists())
            self.assertFalse(old.exists())

    def test_rotates_large_log_with_copy_truncate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bridge.log"
            path.write_bytes(b"x" * (300 * 1024))

            with mock.patch.dict(
                os.environ,
                {"VIBE_STICK_LOG_MAX_BYTES": str(256 * 1024)},
            ):
                rotated = maintenance.rotate_log(path)

            self.assertTrue(rotated)
            self.assertEqual(path.stat().st_size, 0)
            self.assertEqual(path.with_name("bridge.log.1").stat().st_size, 300 * 1024)


if __name__ == "__main__":
    unittest.main()
