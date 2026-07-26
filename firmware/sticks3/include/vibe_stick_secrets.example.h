#pragma once

// Copy this file to vibe_stick_secrets.h and fill in local values.
// vibe_stick_secrets.h is intentionally ignored by git.

#define VIBE_STICK_WIFI_SSID "your-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"
#define VIBE_STICK_BRIDGE_HOST "192.168.1.10"
#define VIBE_STICK_BRIDGE_PORT 8765
#define VIBE_STICK_BRIDGE_TOKEN "paste-generated-token-here"

// Optional: define this list to try several known Wi-Fi networks automatically.
// When present, it replaces VIBE_STICK_WIFI_SSID and VIBE_STICK_WIFI_PASSWORD.
//
// #define VIBE_STICK_WIFI_NETWORKS \
//     { \
//         {"home-wifi", "home-password"}, \
//         {"office-wifi", "office-password"}, \
//         {"phone-hotspot", "hotspot-password"}, \
//     }
