from __future__ import annotations

import argparse
import getpass
import json
import sys
from pathlib import Path
from typing import Any

from vibe_stick.config.atomic_file import atomic_write_text_if_changed
from vibe_stick.config.paths import WIFI_PROFILES_PATH, ensure_app_support


MAX_PROFILES = 8
MAX_SSID_BYTES = 32
MAX_PASSWORD_BYTES = 64


def load_wifi_profiles(path: Path = WIFI_PROFILES_PATH) -> list[dict[str, str]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return []
    except (OSError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ValueError(f"Cannot read Wi-Fi profile file: {exc}") from exc
    if not isinstance(payload, dict) or not isinstance(payload.get("profiles"), list):
        raise ValueError("Wi-Fi profile file must contain a profiles array")
    profiles = [_validated_profile(item) for item in payload["profiles"]]
    if len(profiles) > MAX_PROFILES:
        raise ValueError(f"At most {MAX_PROFILES} Wi-Fi profiles are supported")
    return profiles


def save_wifi_profiles(
    profiles: list[dict[str, str]],
    path: Path = WIFI_PROFILES_PATH,
) -> None:
    if not profiles:
        raise ValueError("At least one Wi-Fi profile must remain")
    if len(profiles) > MAX_PROFILES:
        raise ValueError(f"At most {MAX_PROFILES} Wi-Fi profiles are supported")
    validated = [_validated_profile(profile) for profile in profiles]
    data = json.dumps({"profiles": validated}, ensure_ascii=False, indent=2) + "\n"
    atomic_write_text_if_changed(path, data, mode=0o600)


def usb_wifi_payload(path: Path = WIFI_PROFILES_PATH) -> dict[str, Any]:
    profiles = load_wifi_profiles(path)
    return {
        "configured": bool(profiles),
        "profiles": profiles,
    }


def bootstrap_wifi_profiles(
    payload: object,
    path: Path = WIFI_PROFILES_PATH,
) -> bool:
    if path.exists() and load_wifi_profiles(path):
        return False
    if not isinstance(payload, dict) or not isinstance(payload.get("profiles"), list):
        raise ValueError("Wi-Fi bootstrap payload must contain a profiles array")
    save_wifi_profiles(payload["profiles"], path)
    return True


def add_wifi_profile(
    ssid: str,
    password: str,
    path: Path = WIFI_PROFILES_PATH,
) -> bool:
    incoming = _validated_profile({"ssid": ssid, "password": password})
    profiles = load_wifi_profiles(path)
    replaced = False
    for index, profile in enumerate(profiles):
        if profile["ssid"] == incoming["ssid"]:
            profiles[index] = incoming
            replaced = True
            break
    if not replaced:
        if len(profiles) >= MAX_PROFILES:
            raise ValueError(f"At most {MAX_PROFILES} Wi-Fi profiles are supported")
        profiles.append(incoming)
    save_wifi_profiles(profiles, path)
    return replaced


def remove_wifi_profile(ssid: str, path: Path = WIFI_PROFILES_PATH) -> None:
    profiles = load_wifi_profiles(path)
    remaining = [profile for profile in profiles if profile["ssid"] != ssid]
    if len(remaining) == len(profiles):
        raise ValueError(f"Wi-Fi profile not found: {ssid}")
    save_wifi_profiles(remaining, path)


def _validated_profile(value: object) -> dict[str, str]:
    if not isinstance(value, dict):
        raise ValueError("Each Wi-Fi profile must be an object")
    ssid = value.get("ssid")
    password = value.get("password")
    if not isinstance(ssid, str) or not ssid:
        raise ValueError("Wi-Fi name cannot be empty")
    if not isinstance(password, str):
        raise ValueError("Wi-Fi password must be text")
    if len(ssid.encode("utf-8")) > MAX_SSID_BYTES:
        raise ValueError(f"Wi-Fi name cannot exceed {MAX_SSID_BYTES} UTF-8 bytes")
    if len(password.encode("utf-8")) > MAX_PASSWORD_BYTES:
        raise ValueError(f"Wi-Fi password cannot exceed {MAX_PASSWORD_BYTES} UTF-8 bytes")
    if password and len(password.encode("utf-8")) < 8:
        raise ValueError("Protected Wi-Fi passwords must contain at least 8 UTF-8 bytes")
    return {"ssid": ssid, "password": password}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Manage StickS3 Wi-Fi profiles synced securely over USB."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    add = subparsers.add_parser("add", help="add or update a Wi-Fi profile")
    add.add_argument("ssid", help="Wi-Fi network name")
    add.add_argument(
        "--open",
        action="store_true",
        dest="open_network",
        help="save an open network without a password",
    )
    subparsers.add_parser("list", help="list saved Wi-Fi names")
    remove = subparsers.add_parser("remove", help="remove a Wi-Fi profile")
    remove.add_argument("ssid", help="Wi-Fi network name")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    ensure_app_support()
    try:
        if args.command == "list":
            profiles = load_wifi_profiles()
            if not profiles:
                print("No USB-managed Wi-Fi profiles are saved.")
            for index, profile in enumerate(profiles, start=1):
                security = "open" if not profile["password"] else "protected"
                print(f"{index}. {profile['ssid']} ({security})")
            return 0
        if args.command == "add":
            password = "" if args.open_network else getpass.getpass("Wi-Fi password: ")
            replaced = add_wifi_profile(args.ssid, password)
            action = "Updated" if replaced else "Added"
            print(f"{action} Wi-Fi profile: {args.ssid}")
            print("Keep StickS3 connected by USB; it will sync automatically within 5 seconds.")
            return 0
        if args.command == "remove":
            remove_wifi_profile(args.ssid)
            print(f"Removed Wi-Fi profile: {args.ssid}")
            print("Keep StickS3 connected by USB; it will sync automatically within 5 seconds.")
            return 0
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
