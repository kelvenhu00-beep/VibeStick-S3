# Protocol

VibeStick v0.1.4 prefers a framed protocol over the ESP32-S3 USB Serial/JTAG
channel and falls back to HTTP over Wi-Fi.

## Transport Selection

The Mac bridge opens `/dev/cu.usbmodem*` and sends:

```text
@VBS1 HELLO <bridge-version>
```

The StickS3 enables USB runtime transport only after replying:

```text
@VBS1 READY <firmware-version> USB
```

While that handshake is active, state and POST requests use `REQUEST` and
`RESPONSE` frames. Paths, JSON bodies, and JSON responses are Base64 encoded so
normal ESP-IDF serial logs can share the same USB channel:

```text
@VBS1 REQUEST <id> <method> <path-base64> <body-base64>
@VBS1 RESPONSE <id> <status> <json-base64>
```

PCM uses one begin frame, Base64 chunks, and one end frame:

```text
@VBS1 AUDIO_BEGIN <id> <session-id-base64> <byte-count>
@VBS1 AUDIO_READY <id>
@VBS1 AUDIO_CHUNK <id> <pcm-base64>
@VBS1 AUDIO_END <id>
@VBS1 RESPONSE <id> <status> <json-base64>
```

The bridge rejects transfers that exceed the configured recording limit or do
not match the declared byte count. A USB request failure clears the handshake;
later requests use Wi-Fi until the bridge completes another USB handshake.

## HTTP Fallback

Default bridge URL:

```text
http://<mac-ip>:8765
```

The Mac bridge advertises a DNS-SD service named
`VibeStick Bridge._vibestick._tcp.local`. TXT records identify the service with
`name=vibestick-bridge`, publish the bridge version, and declare `/health` as the
health path. Firmware should prefer the discovered IPv4 address and port over a
stored address, then rediscover after Wi-Fi changes or connection failures.

## Firmware Headers

Firmware requests include:

```text
X-Vibe-Stick-Firmware-Name: vibestick
X-Vibe-Stick-Firmware-Version: 0.1.4
X-Vibe-Stick-Firmware-Transport: HTTP
X-Vibe-Stick-Firmware-Build-Date: <compile date>
```

Audio upload requests additionally include:

```text
X-Vibe-Stick-Sample-Rate: 16000
X-Vibe-Stick-Channels: 1
X-Vibe-Stick-Bits-Per-Sample: 16
```

When `VIBE_STICK_BRIDGE_TOKEN` is configured on the bridge and firmware, protected POST requests also include:

```text
X-Vibe-Stick-Token: <shared-token>
```

Protected endpoints are `/event`, `/quota/refresh`, `/recording/start`, `/recording/audio`, and `/recording/stop`. If the bridge binds outside loopback, such as `0.0.0.0`, `VIBE_STICK_BRIDGE_TOKEN` is required and placeholder tokens are rejected. If the bridge binds to loopback only, missing tokens are allowed for local development.

## GET /state

Returns the current bridge state:

```json
{
  "time": "13:01",
  "wifi": true,
  "ble": false,
  "battery": null,
  "active_provider": "claude",
  "provider": {
    "id": "claude",
    "display_name": "Claude",
    "implemented": true,
    "status": "RUNNING",
    "project": "vibestick",
    "quota_5h_remaining": 66,
    "quota_7d_remaining": 96,
    "quota_updated_at": "13:01",
    "quota_stale": false
  },
  "codex": {
    "status": "RUNNING",
    "project": "vibestick",
    "quota_5h_remaining": 53,
    "quota_7d_remaining": 93,
    "quota_updated_at": "13:01",
    "quota_stale": false
  },
  "alert": {
    "event_id": "",
    "type": "NONE",
    "message": ""
  },
  "bridge_name": "vibestick-bridge",
  "bridge_version": "0.1.4"
}
```

`battery` is intentionally `null` from the bridge. The StickS3 displays its local PMIC battery reading.

`active_provider` selects which normalized `provider` block the firmware should render. `provider.quota_5h_remaining` and `provider.quota_7d_remaining` are remaining percentages from `0` to `100`; `null` means unknown and the firmware renders `--%`. The legacy `codex` block remains present for backward compatibility.

## GET /health

Returns bridge health metadata:

```json
{
  "ok": true,
  "bridge_name": "vibestick-bridge",
  "bridge_version": "0.1.4"
}
```

## POST /event

Receives generic firmware or debug events.

Examples:

```json
{"event":"button_short","source":"sticks3"}
```

```json
{"event":"test_agent_status","source":"manual_test","status":"DONE","message":"test done"}
```

Manual `DONE`, `ERROR`, and `APPROVAL` statuses produce alert fields for local testing.

## POST /quota/refresh

Requests a quota refresh for the active provider. Codex refreshes from local session events. Claude refreshes the cached usage snapshot only when `VIBE_STICK_CLAUDE_USAGE` is enabled; failures keep the provider quota fields `null` so the firmware shows `--%`.

```json
{
  "refreshed": true,
  "state": {
    "time": "13:01",
    "wifi": true,
    "battery": null
  }
}
```

## POST /recording/start

Starts a recording session:

```json
{
  "event": "button_long_start",
  "source": "sticks3",
  "audio_source": "sticks3_pcm",
  "session_id": "<firmware-generated-id>"
}
```

## POST /recording/audio

Uploads raw little-endian signed PCM for the active session:

```text
POST /recording/audio?session_id=<id>
Content-Type: application/octet-stream
```

The bridge writes a local WAV file under:

```text
~/Library/Application Support/VibeStick/Recordings/
```

The bridge rejects audio uploads larger than `VIBE_STICK_MAX_RECORDING_AUDIO_BYTES`. The default is `2000000` bytes.

## POST /recording/stop

Stops the session and runs transcription:

```json
{"event":"button_long_stop","source":"sticks3","paste":true}
```

When transcription succeeds, the bridge pastes the text into the focused macOS app, then the device waits for an explicit action:

- `POST /recording/confirm` presses Enter in the focused macOS app to submit the pasted text.
- `POST /recording/cancel` selects and deletes all text in the focused macOS text field.

On StickS3, single-click the front button to submit or double-click it to clear the focused text field without submitting. Recording status does not trigger agent alert sounds.

StickS3 appends `?compact=1` to the stop, confirm, and cancel endpoints. Compact responses omit the transcript and audio path so long recognized text cannot overflow the firmware response buffer.
