from __future__ import annotations

import os
import socket
import shutil
import subprocess
import threading
from dataclasses import dataclass, field


SERVICE_INSTANCE = "VibeStick Bridge"
SERVICE_TYPE = "_vibestick._tcp"
SERVICE_DOMAIN = "local"


@dataclass
class BonjourRegistration:
    port: int
    bridge_version: str
    process: subprocess.Popen[bytes] | None = None
    _lock: threading.RLock = field(default_factory=threading.RLock, init=False, repr=False)
    _stop: threading.Event = field(default_factory=threading.Event, init=False, repr=False)
    _watchdog: threading.Thread | None = field(default=None, init=False, repr=False)
    _network: tuple[str, ...] = field(default_factory=tuple, init=False, repr=False)

    def start(self) -> bool:
        if _env_disabled():
            return False
        with self._lock:
            if self.process is not None and self.process.poll() is None:
                return False
            started = self._spawn_locked()
            if not started:
                return False
            self._network = _network_signature()
            self._stop.clear()
            if self._watchdog is None or not self._watchdog.is_alive():
                self._watchdog = threading.Thread(
                    target=self._watch,
                    name="vibestick-bonjour",
                    daemon=True,
                )
                self._watchdog.start()
            return True

    def close(self) -> None:
        self._stop.set()
        watchdog = self._watchdog
        if watchdog is not None and watchdog.is_alive():
            watchdog.join(timeout=2)
        with self._lock:
            self._watchdog = None
            self._terminate_locked()

    def check(self) -> bool:
        """Restart a dead registration or re-register after the host network changes."""
        if _env_disabled():
            return False
        network = _network_signature()
        with self._lock:
            network_changed = network != self._network
            process_dead = self.process is None or self.process.poll() is not None
            if not network_changed and not process_dead:
                return True
            if network_changed:
                print(
                    "VibeStick Bridge network changed; re-registering Bonjour service.",
                    flush=True,
                )
            elif process_dead:
                print(
                    "VibeStick Bridge Bonjour process stopped; restarting registration.",
                    flush=True,
                )
            self._terminate_locked()
            started = self._spawn_locked()
            if started:
                self._network = network
            return started

    def _watch(self) -> None:
        while not self._stop.wait(_watchdog_interval_seconds()):
            self.check()

    def _spawn_locked(self) -> bool:
        executable = shutil.which("dns-sd")
        if not executable:
            self.process = None
            return False
        try:
            self.process = subprocess.Popen(
                [
                    executable,
                    "-R",
                    SERVICE_INSTANCE,
                    SERVICE_TYPE,
                    SERVICE_DOMAIN,
                    str(self.port),
                    "name=vibestick-bridge",
                    f"version={self.bridge_version}",
                    "health=/health",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            self.process = None
            return False
        return True

    def _terminate_locked(self) -> None:
        process = self.process
        self.process = None
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def _env_disabled() -> bool:
    return os.environ.get("VIBE_STICK_DISABLE_BONJOUR", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def _watchdog_interval_seconds() -> float:
    try:
        value = float(os.environ.get("VIBE_STICK_BONJOUR_WATCHDOG_SECONDS", "5"))
    except ValueError:
        value = 5.0
    return max(1.0, min(60.0, value))


def _network_signature() -> tuple[str, ...]:
    addresses: set[str] = set()
    ipconfig = shutil.which("ipconfig")
    if ipconfig:
        for _, interface in socket.if_nameindex():
            try:
                result = subprocess.run(
                    [ipconfig, "getifaddr", interface],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=1,
                )
            except (OSError, subprocess.TimeoutExpired):
                continue
            address = result.stdout.strip()
            if result.returncode == 0 and address and not address.startswith("127."):
                addresses.add(address)
    if not addresses:
        try:
            for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                address = item[4][0]
                if address and not address.startswith("127."):
                    addresses.add(address)
        except OSError:
            pass
    return tuple(sorted(addresses))
