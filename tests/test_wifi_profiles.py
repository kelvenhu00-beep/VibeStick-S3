from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from vibe_stick.config.wifi_profiles import (
    MAX_PROFILES,
    add_wifi_profile,
    bootstrap_wifi_profiles,
    load_wifi_profiles,
    remove_wifi_profile,
    save_wifi_profiles,
    usb_wifi_payload,
)


class WifiProfilesTests(unittest.TestCase):
    def make_path(self) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return Path(directory.name) / "wifi-networks.json"

    def test_add_update_list_and_remove_profiles(self) -> None:
        path = self.make_path()

        self.assertFalse(add_wifi_profile("Office", "password-one", path))
        self.assertFalse(add_wifi_profile("Home", "password-two", path))
        self.assertTrue(add_wifi_profile("Office", "password-new", path))

        self.assertEqual(
            load_wifi_profiles(path),
            [
                {"ssid": "Office", "password": "password-new"},
                {"ssid": "Home", "password": "password-two"},
            ],
        )
        remove_wifi_profile("Office", path)
        self.assertEqual(
            load_wifi_profiles(path),
            [{"ssid": "Home", "password": "password-two"}],
        )

    def test_usb_payload_is_unconfigured_when_file_is_missing(self) -> None:
        path = self.make_path()

        self.assertEqual(
            usb_wifi_payload(path),
            {"configured": False, "profiles": []},
        )

    def test_usb_payload_contains_saved_profiles(self) -> None:
        path = self.make_path()
        save_wifi_profiles([{"ssid": "Phone", "password": "password-three"}], path)

        self.assertEqual(
            usb_wifi_payload(path),
            {
                "configured": True,
                "profiles": [{"ssid": "Phone", "password": "password-three"}],
            },
        )

    def test_bootstrap_initializes_once_without_overwriting(self) -> None:
        path = self.make_path()
        first = {"profiles": [{"ssid": "Initial", "password": "password-one"}]}
        second = {"profiles": [{"ssid": "Other", "password": "password-two"}]}

        self.assertTrue(bootstrap_wifi_profiles(first, path))
        self.assertFalse(bootstrap_wifi_profiles(second, path))
        self.assertEqual(
            load_wifi_profiles(path),
            [{"ssid": "Initial", "password": "password-one"}],
        )

    def test_rejects_more_than_device_capacity(self) -> None:
        path = self.make_path()
        profiles = [
            {"ssid": f"network-{index}", "password": "password"}
            for index in range(MAX_PROFILES + 1)
        ]

        with self.assertRaisesRegex(ValueError, "At most"):
            save_wifi_profiles(profiles, path)

    def test_rejects_removing_last_profile(self) -> None:
        path = self.make_path()
        save_wifi_profiles([{"ssid": "Only", "password": "password"}], path)

        with self.assertRaisesRegex(ValueError, "At least one"):
            remove_wifi_profile("Only", path)


if __name__ == "__main__":
    unittest.main()
