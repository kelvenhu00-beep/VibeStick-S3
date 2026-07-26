# Changes from upstream

## Attribution and baseline

VibeStick-S3 is a modified distribution of
[GaryGaryyy/VibeStick](https://github.com/GaryGaryyy/VibeStick), created and
open-sourced by Gary Zhang under the MIT License.

This distribution uses the following upstream snapshot as its comparison
baseline:

- Repository: <https://github.com/GaryGaryyy/VibeStick>
- Branch at comparison time: `main`
- Commit:
  [`d7dc0364423bf738a0ce5c697544b386678a1e62`](https://github.com/GaryGaryyy/VibeStick/commit/d7dc0364423bf738a0ce5c697544b386678a1e62)
- Original copyright: Copyright (c) 2026 Gary Zhang
- Distribution maintainer: `kelvenhu00-beep`

Thank you to Gary Zhang for the original architecture, firmware, macOS bridge
and HUD, provider integrations, assets, documentation, tests, and the decision
to release the project under the MIT License. This repository retains
substantial original and original-derived code. It is not an official upstream
release.

## Main modifications in this distribution

### Runtime transport and mobility

- Added an ESP32-S3 USB Serial/JTAG application transport.
- Added a macOS USB bridge transport for state, button, and audio traffic.
- Made USB the preferred runtime path while keeping automatic Wi-Fi fallback.
- Added Bonjour bridge discovery so changing DHCP addresses does not require a
  firmware configuration change.
- Added multiple-known-Wi-Fi support in the firmware secrets example.
- Added the on-device `USB`, `WIFI`, and `OFF` connection indicator.

### Voice input and button workflow

- Changed the StickS3 microphone path to upload captured PCM audio to the Mac
  bridge for transcription.
- Added upload size limits, compact responses, retry/timeout handling, stale
  recording expiry, and recording diagnostics.
- Retained an optional Mac microphone fallback.
- Added a confirmation stage after recognized text is pasted:
  single-click submits the text and double-click clears it.
- Added English on-device recording states to avoid missing Chinese glyphs.

### Device interface

- Removed the obsolete 5H card from the StickS3 screen and retained the 7D
  usage view.
- Reworked the recording overlay and connection-status presentation.
- Updated firmware configuration, component dependencies, generated font
  subset, and UI layout for the new workflow.

### Local bridge, deployment, and tests

- Added paste confirmation and clear operations to the bridge.
- Added USB and Bonjour diagnostics to `scripts/doctor.sh`.
- Added `scripts/preflight.sh` and a shorter public deployment path.
- Expanded automated coverage for USB transport, discovery, recording
  confirmation, compact responses, transcription handling, and Codex process
  detection.
- Updated architecture, hardware, and protocol documentation.

## Original and unchanged portions

Files and lines not changed by this distribution remain the upstream work.
Modified files can also contain substantial upstream code alongside the changes
listed above. The original MIT copyright and permission notice therefore remain
in `LICENSE` and apply to copies or substantial portions of the original
software.

## Inspecting the exact diff

The summary above is explanatory; Git is the authoritative record. To compare
this distribution with the stated upstream baseline:

```sh
git clone https://github.com/kelvenhu00-beep/VibeStick-S3.git
cd VibeStick-S3
git remote add upstream https://github.com/GaryGaryyy/VibeStick.git
git fetch upstream
git diff d7dc0364423bf738a0ce5c697544b386678a1e62..main
```

Bug reports for behavior introduced by this modified distribution should be
reported in the `kelvenhu00-beep/VibeStick-S3` repository rather than attributed
to the upstream author.
