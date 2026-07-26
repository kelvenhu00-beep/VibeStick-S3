import json
import os
import unittest
import urllib.error
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

from vibe_stick.claude.usage import (
    CLAUDE_USAGE_HEADERS,
    USAGE_ENDPOINT,
    fetch_usage,
    parse_usage,
    resolve_token,
    to_quota_snapshot,
    token_from_credentials_json,
    usage_enabled,
)

FIXTURE_PATH = Path(__file__).resolve().parents[1] / "bridge" / "tests" / "fixtures" / "claude_usage_sample.json"


class _FakeResponse:
    def __init__(self, status: int, body: bytes) -> None:
        self.status = status
        self._body = body

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *exc: object) -> bool:
        return False

    def read(self, amount: int = -1) -> bytes:
        return self._body


def _opener_returning(status: int, body: bytes):
    def _opener(request, timeout=None):  # noqa: ANN001
        return _FakeResponse(status, body)

    return _opener


def _opener_raising(exc: BaseException):
    def _opener(request, timeout=None):  # noqa: ANN001
        raise exc

    return _opener


def _usage_on():
    return mock.patch.dict(os.environ, {"VIBE_STICK_CLAUDE_USAGE": "on"}, clear=True)


class ClaudeUsageTests(unittest.TestCase):
    def test_appendix_a_fixture_parses_remaining_percentages(self) -> None:
        data = json.loads(FIXTURE_PATH.read_text())
        usage = parse_usage(data)
        self.assertIsNotNone(usage)

        snapshot = to_quota_snapshot(
            usage,
            now=datetime(2026, 6, 28, 14, 0, tzinfo=timezone.utc),
            fetched_at=datetime(2026, 6, 28, 13, 59, tzinfo=timezone.utc),
        )

        self.assertEqual(snapshot.quota_5h_remaining, 66)
        self.assertEqual(snapshot.quota_7d_remaining, 96)
        self.assertFalse(snapshot.quota_stale)

    def test_parse_usage_falls_back_to_top_level_windows(self) -> None:
        usage = parse_usage(
            {
                "five_hour": {"utilization": 12.5, "resets_at": "2026-06-28T14:50:00+00:00"},
                "seven_day": {"utilization": 99.6, "resets_at": "2026-06-29T10:00:00+00:00"},
            }
        )
        self.assertIsNotNone(usage)

        snapshot = to_quota_snapshot(
            usage,
            now=datetime(2026, 6, 28, 14, 0, tzinfo=timezone.utc),
            fetched_at=datetime(2026, 6, 28, 13, 59, tzinfo=timezone.utc),
        )

        self.assertEqual(snapshot.quota_5h_remaining, 88)
        self.assertEqual(snapshot.quota_7d_remaining, 0)

    def test_parse_usage_unknown_fields_return_none(self) -> None:
        self.assertIsNone(parse_usage({"limits": [{"kind": "unknown", "percent": 10}]}))

    def test_token_from_credentials_json_rejects_expired_token(self) -> None:
        payload = json.dumps({"claudeAiOauth": {"accessToken": "secret-token", "expiresAt": 1000}})
        self.assertIsNone(token_from_credentials_json(payload, now_ms=2000))
        self.assertEqual(token_from_credentials_json(payload, now_ms=500), "secret-token")

    def test_resolve_token_prefers_environment(self) -> None:
        with mock.patch.dict(os.environ, {"CLAUDE_CODE_OAUTH_TOKEN": "env-token"}):
            self.assertEqual(resolve_token(), "env-token")

    def test_usage_enabled_defaults_off(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(usage_enabled())

    def test_fetch_usage_is_noop_when_disabled(self) -> None:
        opener = mock.Mock()
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(fetch_usage(token="secret-token", opener=opener))
        opener.assert_not_called()

    def test_required_usage_headers_are_present(self) -> None:
        for header in (
            "anthropic-version",
            "anthropic-beta",
            "x-app",
            "User-Agent",
            "anthropic-client-platform",
            "Content-Type",
        ):
            self.assertIn(header, CLAUDE_USAGE_HEADERS)

    def test_fetch_usage_success_parses_real_response(self) -> None:
        body = FIXTURE_PATH.read_bytes()
        with _usage_on():
            usage = fetch_usage(token="secret-token", opener=_opener_returning(200, body))
        self.assertIsNotNone(usage)
        self.assertEqual(round(usage.five_hour_percent), 34)
        self.assertEqual(round(usage.seven_day_percent), 4)

    def test_fetch_usage_returns_none_on_http_403(self) -> None:
        exc = urllib.error.HTTPError(USAGE_ENDPOINT, 403, "Forbidden", {}, None)
        with _usage_on():
            self.assertIsNone(fetch_usage(token="secret-token", opener=_opener_raising(exc)))

    def test_fetch_usage_returns_none_on_http_401(self) -> None:
        exc = urllib.error.HTTPError(USAGE_ENDPOINT, 401, "Unauthorized", {}, None)
        with _usage_on():
            self.assertIsNone(fetch_usage(token="secret-token", opener=_opener_raising(exc)))

    def test_fetch_usage_returns_none_on_timeout(self) -> None:
        with _usage_on():
            self.assertIsNone(fetch_usage(token="secret-token", opener=_opener_raising(TimeoutError())))

    def test_fetch_usage_returns_none_on_non_200(self) -> None:
        with _usage_on():
            self.assertIsNone(fetch_usage(token="secret-token", opener=_opener_returning(204, b"")))

    def test_fetch_usage_returns_none_on_invalid_json(self) -> None:
        with _usage_on():
            self.assertIsNone(fetch_usage(token="secret-token", opener=_opener_returning(200, b"not-json")))

    def test_fetch_usage_does_not_log_token_or_body(self) -> None:
        token = "super-secret-token-value-1234567890"
        body = FIXTURE_PATH.read_bytes()
        exc = urllib.error.HTTPError(USAGE_ENDPOINT, 403, "Forbidden", {}, None)
        with _usage_on():
            with self.assertLogs("vibe_stick.claude.usage", level="DEBUG") as captured:
                # 403 path emits a log line; success path emits none.
                self.assertIsNone(fetch_usage(token=token, opener=_opener_raising(exc)))
                fetch_usage(token=token, opener=_opener_returning(200, body))
        joined = "\n".join(captured.output)
        self.assertNotIn(token, joined)
        self.assertNotIn("Authorization", joined)
        self.assertNotIn("Bearer", joined)


if __name__ == "__main__":
    unittest.main()
