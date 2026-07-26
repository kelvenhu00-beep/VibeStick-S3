from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass


SERVICE_INSTANCE = "VibeStick Bridge"
SERVICE_TYPE = "_vibestick._tcp"
SERVICE_DOMAIN = "local"


@dataclass
class BonjourRegistration:
    port: int
    bridge_version: str
    process: subprocess.Popen[bytes] | None = None

    def start(self) -> bool:
        if _env_disabled() or self.process is not None:
            return False
        executable = shutil.which("dns-sd")
        if not executable:
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

    def close(self) -> None:
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
