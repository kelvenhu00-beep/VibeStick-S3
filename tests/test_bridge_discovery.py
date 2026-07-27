import os
import unittest
from unittest import mock

from vibe_stick.server.discovery import BonjourRegistration


class BridgeDiscoveryTests(unittest.TestCase):
    @mock.patch("vibe_stick.server.discovery.subprocess.Popen")
    @mock.patch("vibe_stick.server.discovery.shutil.which", return_value="/usr/bin/dns-sd")
    @mock.patch("vibe_stick.server.discovery._network_signature", return_value=("192.0.2.10",))
    def test_registers_expected_bonjour_service(self, _network, _which, popen) -> None:
        process = popen.return_value
        process.poll.return_value = None
        registration = BonjourRegistration(port=8765, bridge_version="0.1.4")
        self.addCleanup(registration.close)

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

    @mock.patch("vibe_stick.server.discovery.subprocess.Popen")
    @mock.patch("vibe_stick.server.discovery.shutil.which", return_value="/usr/bin/dns-sd")
    @mock.patch("vibe_stick.server.discovery._network_signature", return_value=("192.0.2.10",))
    def test_watchdog_restarts_dead_registration(self, _network, _which, popen) -> None:
        first = mock.Mock()
        first.poll.return_value = 1
        second = mock.Mock()
        second.poll.return_value = None
        popen.side_effect = [first, second]
        registration = BonjourRegistration(port=8765, bridge_version="0.1.4")
        self.addCleanup(registration.close)

        self.assertTrue(registration.start())
        self.assertTrue(registration.check())

        self.assertEqual(popen.call_count, 2)
        self.assertIs(registration.process, second)

    @mock.patch("vibe_stick.server.discovery.subprocess.Popen")
    @mock.patch("vibe_stick.server.discovery.shutil.which", return_value="/usr/bin/dns-sd")
    @mock.patch(
        "vibe_stick.server.discovery._network_signature",
        side_effect=[("192.0.2.10",), ("198.51.100.20",)],
    )
    def test_network_change_reregisters_service(self, _network, _which, popen) -> None:
        first = mock.Mock()
        first.poll.return_value = None
        second = mock.Mock()
        second.poll.return_value = None
        popen.side_effect = [first, second]
        registration = BonjourRegistration(port=8765, bridge_version="0.1.4")
        self.addCleanup(registration.close)

        self.assertTrue(registration.start())
        self.assertTrue(registration.check())

        first.terminate.assert_called_once()
        self.assertEqual(popen.call_count, 2)
        self.assertIs(registration.process, second)


if __name__ == "__main__":
    unittest.main()
