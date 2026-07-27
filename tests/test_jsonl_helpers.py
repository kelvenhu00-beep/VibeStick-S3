import json
import os
import tempfile
import unittest
from pathlib import Path

from vibe_stick.providers._jsonl import (
    cached_tail_json_events,
    clear_jsonl_caches,
    session_files,
    tail_json_events,
)


class JsonlHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        clear_jsonl_caches()

    def test_tail_json_events_ignores_partial_and_invalid_lines(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            path.write_text(
                "\n".join(
                    [
                        "not-json",
                        json.dumps({"type": "user", "message": "hello"}),
                        "[1, 2, 3]",
                        "{bad json",
                        json.dumps({"type": "assistant", "message": "done"}),
                    ]
                )
            )

            events = list(tail_json_events(path, tail_bytes=2048))

        self.assertEqual([event["type"] for event in events], ["user", "assistant"])

    def test_session_files_returns_newest_jsonl_files_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            older = root / "older.jsonl"
            newer = root / "nested" / "newer.jsonl"
            ignored = root / "ignored.txt"
            newer.parent.mkdir()
            older.write_text("{}\n")
            newer.write_text("{}\n")
            ignored.write_text("{}\n")
            os.utime(older, (1000, 1000))
            os.utime(newer, (2000, 2000))

            files = session_files(root, max_files=1)

        self.assertEqual(files, [newer])

    def test_cached_tail_reads_appended_events(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            path.write_text(json.dumps({"type": "first"}) + "\n")

            first = list(cached_tail_json_events(path, tail_bytes=2048))
            with path.open("a") as handle:
                handle.write(json.dumps({"type": "second"}) + "\n")
            second = list(cached_tail_json_events(path, tail_bytes=2048))

        self.assertEqual([event["type"] for event in first], ["first"])
        self.assertEqual([event["type"] for event in second], ["first", "second"])


if __name__ == "__main__":
    unittest.main()
