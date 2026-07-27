#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define VIBE_WIFI_MAX_PROFILES 8
#define VIBE_WIFI_SSID_BYTES 33
#define VIBE_WIFI_PASSWORD_BYTES 65

typedef struct {
    char ssid[VIBE_WIFI_SSID_BYTES];
    char password[VIBE_WIFI_PASSWORD_BYTES];
} vibe_wifi_profile_t;

esp_err_t vibe_wifi_profiles_init(void);
size_t vibe_wifi_profile_count(void);
bool vibe_wifi_profile_copy(size_t index, vibe_wifi_profile_t *profile);
esp_err_t vibe_wifi_profiles_apply_json(const char *json, bool *changed);
