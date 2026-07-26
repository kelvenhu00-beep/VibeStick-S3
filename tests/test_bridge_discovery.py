import os
import unittest
from unittest import mock

from vibe_stick.server.discovery import BonjourRegistration


class BridgeDiscoveryTests(unittest.TestCase):
    @mock.patch("vibe_stick.server.discovery.subprocess.Popen")
    @mock.patch("vibe_stick.server.discovery.shutil.which", return_value="/usr/bin/dns-sd")
    def test_registers_expected_bonjour_service(self, _which, popen) -> None:
        registration = BonjourRegistration(port=8765, bridge_version="0.1.4")

        self.assertTrue(registration.start())

        command = popen.call_args.args[0]
        self.assertEqual(command[:5], ["/usr/bin/dns-sd", "-R", "VibeStick Bridge", "_vibestick._tcp", "local"])
        self.assertIn("8765", command)
        self.assertIn("name=vibestick-bridge", command)
        self.assertIn("version=0.1.4", command)

    @mock.patch("vibe_stick.server.discovery.subprocess.Popen")
    @mock.patch("vibe_stick.server.discovery.shutil.which", return_value="/usr/bin/dns-sd")
    def test_can_disable_bonjour(self, _which, popen) -> None:
        with mock.patch.dict(os.environ, {"VIBE_STICK_DISABLE_BONJOUR": "true"}):
            registration = BonjourRegistration(port=8765, bridge_version="0.1.4")
            self.assertFalse(registration.start())
        popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
