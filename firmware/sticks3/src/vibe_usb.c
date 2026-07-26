#include "vibe_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "vibe_stick_config.h"

#define USB_PROTOCOL_PREFIX "@VBS1"
#define USB_RX_BUFFER_SIZE 8192
#define USB_TX_BUFFER_SIZE 16384
#define USB_LINE_BUFFER_SIZE 4096
#define USB_AUDIO_CHUNK_BYTES 720

typedef enum {
    USB_AWAIT_NONE = 0,
    USB_AWAIT_AUDIO_READY,
    USB_AWAIT_RESPONSE,
} usb_await_t;

static const char *TAG = "vibe_usb";
static SemaphoreHandle_t s_request_lock;
static SemaphoreHandle_t s_response_ready;
static volatile bool s_ready;
static uint32_t s_next_request_id;
static uint32_t s_waiting_request_id;
static volatile usb_await_t s_awaiting;
static int s_response_status;
static char *s_response;
static size_t s_response_len;

static esp_err_t write_line(const char *line)
{
    size_t length = strlen(line);
    if (usb_serial_jtag_write_bytes(line, length, pdMS_TO_TICKS(3000)) != (int)length ||
        usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(1000)) != 1) {
        s_ready = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static char *base64_encode(const uint8_t *data, size_t data_len)
{
    size_t required = 0;
    int result = mbedtls_base64_encode(NULL, 0, &required, data, data_len);
    if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && !(result == 0 && required == 0)) {
        return NULL;
    }
    char *encoded = malloc(required + 1);
    if (!encoded) {
        return NULL;
    }
    size_t written = 0;
    result = mbedtls_base64_encode((unsigned char *)encoded, required + 1, &written, data, data_len);
    if (result != 0) {
        free(encoded);
        return NULL;
    }
    encoded[written] = '\0';
    return encoded;
}

static bool parse_uint32(const char *text, uint32_t *value)
{
    if (!text || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static void handle_protocol_line(char *line)
{
    if (strncmp(line, USB_PROTOCOL_PREFIX " HELLO ", strlen(USB_PROTOCOL_PREFIX " HELLO ")) == 0) {
        char ready[96];
        snprintf(ready, sizeof(ready), USB_PROTOCOL_PREFIX " READY %s USB", FIRMWARE_VERSION);
        if (write_line(ready) == ESP_OK && !s_ready) {
            ESP_LOGI(TAG, "USB runtime connected");
        }
        s_ready = true;
        return;
    }

    char *save = NULL;
    char *prefix = strtok_r(line, " ", &save);
    char *command = strtok_r(NULL, " ", &save);
    char *request_text = strtok_r(NULL, " ", &save);
    if (!prefix || strcmp(prefix, USB_PROTOCOL_PREFIX) != 0 || !command || !request_text) {
        return;
    }
    uint32_t request_id = 0;
    if (!parse_uint32(request_text, &request_id) || request_id != s_waiting_request_id) {
        return;
    }

    if (strcmp(command, "AUDIO_READY") == 0 && s_awaiting == USB_AWAIT_AUDIO_READY) {
        s_awaiting = USB_AWAIT_NONE;
        xSemaphoreGive(s_response_ready);
        return;
    }
    if (strcmp(command, "RESPONSE") != 0 || s_awaiting != USB_AWAIT_RESPONSE) {
        return;
    }

    char *status_text = strtok_r(NULL, " ", &save);
    char *payload = strtok_r(NULL, " ", &save);
    if (!status_text || !payload) {
        return;
    }
    s_response_status = atoi(status_text);
    if (s_response && s_response_len > 0) {
        size_t decoded = 0;
        int result = mbedtls_base64_decode(
            (unsigned char *)s_response,
            s_response_len - 1,
            &decoded,
            (const unsigned char *)payload,
            strlen(payload)
        );
        if (result == 0) {
            s_response[decoded] = '\0';
        } else {
            s_response[0] = '\0';
            s_response_status = 500;
        }
    }
    s_awaiting = USB_AWAIT_NONE;
    xSemaphoreGive(s_response_ready);
}

static void usb_rx_task(void *arg)
{
    (void)arg;
    uint8_t incoming[256];
    char *line = malloc(USB_LINE_BUFFER_SIZE);
    if (!line) {
        ESP_LOGE(TAG, "USB receive buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }
    size_t used = 0;
    while (true) {
        int received = usb_serial_jtag_read_bytes(incoming, sizeof(incoming), pdMS_TO_TICKS(100));
        for (int i = 0; i < received; ++i) {
            char value = (char)incoming[i];
            if (value == '\n') {
                line[used] = '\0';
                if (used > 0 && line[used - 1] == '\r') {
                    line[used - 1] = '\0';
                }
                handle_protocol_line(line);
                used = 0;
            } else if (used < USB_LINE_BUFFER_SIZE - 1) {
                line[used++] = value;
            } else {
                used = 0;
            }
        }
    }
}

static esp_err_t wait_for_response(int timeout_ms)
{
    if (xSemaphoreTake(s_response_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_awaiting = USB_AWAIT_NONE;
        s_ready = false;
        return ESP_ERR_TIMEOUT;
    }
    return s_response_status >= 200 && s_response_status < 300 ? ESP_OK : ESP_FAIL;
}

esp_err_t vibe_usb_init(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = USB_TX_BUFFER_SIZE,
        .rx_buffer_size = USB_RX_BUFFER_SIZE,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG, "install USB Serial/JTAG driver");
    usb_serial_jtag_vfs_use_driver();
    s_request_lock = xSemaphoreCreateMutex();
    s_response_ready = xSemaphoreCreateBinary();
    if (!s_request_lock || !s_response_ready) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(usb_rx_task, "vibe_usb_rx", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool vibe_usb_ready(void)
{
    return s_ready;
}

esp_err_t vibe_usb_request(const char *method, const char *path, const char *body,
                           char *response, size_t response_len, int timeout_ms)
{
    if (!vibe_usb_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_request_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    char *path_encoded = base64_encode((const uint8_t *)path, strlen(path));
    const char *body_text = body ? body : "";
    char *body_encoded = base64_encode((const uint8_t *)body_text, strlen(body_text));
    if (!path_encoded || !body_encoded) {
        free(path_encoded);
        free(body_encoded);
        xSemaphoreGive(s_request_lock);
        return ESP_ERR_NO_MEM;
    }
    uint32_t request_id = ++s_next_request_id;
    size_t line_len = strlen(path_encoded) + strlen(body_encoded) + strlen(method) + 64;
    char *line = malloc(line_len);
    if (!line) {
        free(path_encoded);
        free(body_encoded);
        xSemaphoreGive(s_request_lock);
        return ESP_ERR_NO_MEM;
    }
    snprintf(line, line_len, USB_PROTOCOL_PREFIX " REQUEST %lu %s %s %s",
             (unsigned long)request_id, method, path_encoded, body_encoded);
    free(path_encoded);
    free(body_encoded);

    s_waiting_request_id = request_id;
    s_response = response;
    s_response_len = response_len;
    s_response_status = 0;
    s_awaiting = USB_AWAIT_RESPONSE;
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    esp_err_t result = write_line(line);
    free(line);
    if (result == ESP_OK) {
        result = wait_for_response(timeout_ms);
    } else {
        s_awaiting = USB_AWAIT_NONE;
    }
    xSemaphoreGive(s_request_lock);
    return result;
}

esp_err_t vibe_usb_post_audio(const char *session_id, const uint8_t *audio, size_t audio_len,
                              char *response, size_t response_len, int timeout_ms)
{
    if (!vibe_usb_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_request_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    char *session_encoded = base64_encode((const uint8_t *)session_id, strlen(session_id));
    if (!session_encoded) {
        xSemaphoreGive(s_request_lock);
        return ESP_ERR_NO_MEM;
    }
    uint32_t request_id = ++s_next_request_id;
    char begin[160];
    snprintf(begin, sizeof(begin), USB_PROTOCOL_PREFIX " AUDIO_BEGIN %lu %s %u",
             (unsigned long)request_id, session_encoded, (unsigned)audio_len);
    free(session_encoded);

    s_waiting_request_id = request_id;
    s_response_status = 200;
    s_awaiting = USB_AWAIT_AUDIO_READY;
    esp_err_t result = write_line(begin);
    if (result == ESP_OK) {
        result = wait_for_response(3000);
    }
    for (size_t offset = 0; result == ESP_OK && offset < audio_len; offset += USB_AUDIO_CHUNK_BYTES) {
        size_t chunk_len = audio_len - offset;
        if (chunk_len > USB_AUDIO_CHUNK_BYTES) {
            chunk_len = USB_AUDIO_CHUNK_BYTES;
        }
        char *encoded = base64_encode(audio + offset, chunk_len);
        if (!encoded) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        size_t line_len = strlen(encoded) + 64;
        char *line = malloc(line_len);
        if (!line) {
            free(encoded);
            result = ESP_ERR_NO_MEM;
            break;
        }
        snprintf(line, line_len, USB_PROTOCOL_PREFIX " AUDIO_CHUNK %lu %s",
                 (unsigned long)request_id, encoded);
        free(encoded);
        result = write_line(line);
        free(line);
    }

    if (result == ESP_OK) {
        char end[64];
        snprintf(end, sizeof(end), USB_PROTOCOL_PREFIX " AUDIO_END %lu", (unsigned long)request_id);
        s_response = response;
        s_response_len = response_len;
        s_response_status = 0;
        s_awaiting = USB_AWAIT_RESPONSE;
        if (response && response_len > 0) {
            response[0] = '\0';
        }
        result = write_line(end);
        if (result == ESP_OK) {
            result = wait_for_response(timeout_ms);
        }
    }
    if (result != ESP_OK) {
        s_awaiting = USB_AWAIT_NONE;
    }
    xSemaphoreGive(s_request_lock);
    return result;
}
