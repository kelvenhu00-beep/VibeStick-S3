#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t vibe_usb_init(void);
bool vibe_usb_ready(void);
esp_err_t vibe_usb_request(const char *method, const char *path, const char *body,
                           char *response, size_t response_len, int timeout_ms);
esp_err_t vibe_usb_post_audio(const char *session_id, const uint8_t *audio, size_t audio_len,
                              char *response, size_t response_len, int timeout_ms);
