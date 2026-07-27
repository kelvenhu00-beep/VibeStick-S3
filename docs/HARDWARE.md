# Hardware

## Supported Device

VibeStick v0.1.4 targets M5Stack StickS3.

The project does not currently claim support for other devices because the UI layout, front button behavior, microphone path, speaker path, PMIC battery reads, and screen size are all written around StickS3.

## Hardware Used

- Screen: LVGL UI on the StickS3 display.
- Blue front button: hold for push-to-talk recording; after `TEXT READY`,
  single-click submits and double-click clears the focused text box.
- Side button: provider switching.
- Reset/power button: single-click resets or powers on; double-click powers
  off; long-press enters download mode.
- Microphone: StickS3 microphone captured as 16 kHz / 16-bit / mono PCM.
- Speaker: ES8311 / I2S playback for generated agent status tones.
- Wi-Fi: automatic HTTP fallback to the Mac bridge on a 2.4 GHz Wi-Fi network. StickS3 / ESP32-S3 does not support 5 GHz Wi-Fi.
- USB-C: preferred runtime transport, flashing, and serial logs.
- Battery / USB power: local PMIC reads for the battery UI.

## Firmware Configuration

Install ESP-IDF v5.5.x once before building or flashing firmware. Follow Espressif's [ESP-IDF v5.5.1 ESP32-S3 guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html), or use:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Create a local secrets header:

```sh
cp firmware/sticks3/include/vibe_stick_secrets.example.h firmware/sticks3/include/vibe_stick_secrets.h
```

Edit:

```c
#define VIBE_STICK_WIFI_SSID "your-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"
#define VIBE_STICK_BRIDGE_HOST "192.168.1.10"
#define VIBE_STICK_BRIDGE_PORT 8765
#define VIBE_STICK_BRIDGE_TOKEN "paste-generated-token-here"
```

Do not commit `vibe_stick_secrets.h`.

`VIBE_STICK_BRIDGE_HOST` is a compatibility fallback. At runtime, the firmware
normally discovers the Mac bridge through the `_vibestick._tcp.local` Bonjour
service, so the Mac can receive a different DHCP address after leaving and
rejoining the network.

The compiled settings are the initial fallback used on a new or erased device.
After the first installation, manage saved Wi-Fi networks over USB:

```sh
./scripts/wifi.sh add "Wi-Fi name"
./scripts/wifi.sh list
./scripts/wifi.sh remove "Old Wi-Fi name"
```

`add` asks for the password without echoing it. Keep the StickS3 connected by
USB for at least five seconds; it stores the profiles in NVS, so adding a
network does not require rebuilding or flashing firmware. Wi-Fi passwords are
returned only by the USB-only `/device/wifi` runtime route and are not exposed
by the HTTP bridge.

For a factory firmware image, several initial profiles can still be compiled
with this optional definition:

```c
#define VIBE_STICK_WIFI_NETWORKS \
    { \
        {"home-wifi", "home-password"}, \
        {"office-wifi", "office-password"}, \
        {"phone-hotspot", "hotspot-password"}, \
    }
```

When the list is present it replaces the single `VIBE_STICK_WIFI_SSID` and
`VIBE_STICK_WIFI_PASSWORD` pair. USB-managed NVS profiles replace the compiled
list after the first successful sync. The firmware rotates through the saved profiles
when association fails. It also changes profiles after five consecutive bridge
state failures on an associated Wi-Fi network, except during recording or while
waiting for the user to submit or clear recognized text. The screen displays
the connected SSID or `TRY <name>` while attempting a profile.

The Wi-Fi network must be 2.4 GHz. If the SSID is a combined 2.4/5 GHz network and the StickS3 cannot connect, create or select a dedicated 2.4 GHz SSID.

## Flashing

Load ESP-IDF into every new terminal before running `idf.py`:

```sh
. $HOME/esp/esp-idf/export.sh
```

Adjust the path if ESP-IDF is installed elsewhere. If you see `command not found: idf.py`, this shell has not loaded ESP-IDF yet.

From the firmware directory:

```sh
cd firmware/sticks3
idf.py build flash monitor
```

If automatic flashing fails, put the StickS3 into download mode and retry:

1. Plug the StickS3 into the Mac with a USB-C data cable.
2. Long-press the side power button until the blue LED double-blinks and the screen turns off.
3. Run `ls /dev/cu.*` to find the serial port.
4. Retry `idf.py -p <port> build flash`.
5. After flashing, short-press the reset/power button to wake the screen. The
   blue LED should turn off and the VibeStick home screen should appear.

For normal use, double-click the reset/power button to turn the StickS3 off.
A single click while it is running performs a reset and therefore appears to
turn the screen off and immediately back on.

## Runtime Network

With a USB-C data cable connected, the StickS3 prefers the USB runtime protocol
and does not depend on the Mac and StickS3 having stable Wi-Fi addresses. The Mac
still needs whatever Internet connection the configured ASR provider requires.

After the cable or USB bridge is unavailable, the StickS3 falls back to HTTP.
The Mac bridge listens on `0.0.0.0:8765`; Bonjour/mDNS multicast must be allowed
between clients on the same Wi-Fi network. Networks with client isolation cannot
carry the fallback path.

USB transports captured microphone PCM directly. Wi-Fi transport applies IMA
ADPCM compression on the StickS3 and the bridge decodes it before producing the
WAV file. A single device recording is forcibly stopped after 55 seconds.
