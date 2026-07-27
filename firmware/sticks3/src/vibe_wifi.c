#include "vibe_wifi.h"

#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "vibe_stick_config.h"

#define WIFI_NVS_NAMESPACE "vibestick"
#define WIFI_NVS_KEY "wifi_profiles"
#define WIFI_BLOB_MAGIC 0x56425746u
#define WIFI_BLOB_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    vibe_wifi_profile_t profiles[VIBE_WIFI_MAX_PROFILES];
} wifi_profiles_blob_t;

typedef struct {
    const char *ssid;
    const char *password;
} compiled_wifi_credential_t;

#ifdef VIBE_STICK_WIFI_NETWORKS
static const compiled_wifi_credential_t s_compiled_profiles[] = VIBE_STICK_WIFI_NETWORKS;
#else
static const compiled_wifi_credential_t s_compiled_profiles[] = {
    {VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD},
};
#endif

static const char *TAG = "vibe_wifi";
static SemaphoreHandle_t s_lock;
static wifi_profiles_blob_t s_profiles;

static bool profile_valid(const vibe_wifi_profile_t *profile)
{
    return profile &&
           profile->ssid[0] != '\0' &&
           strnlen(profile->ssid, sizeof(profile->ssid)) < sizeof(profile->ssid) &&
           strnlen(profile->password, sizeof(profile->password)) < sizeof(profile->password);
}

static void load_compiled_profiles(void)
{
    memset(&s_profiles, 0, sizeof(s_profiles));
    s_profiles.magic = WIFI_BLOB_MAGIC;
    s_profiles.version = WIFI_BLOB_VERSION;
    size_t compiled_count = sizeof(s_compiled_profiles) / sizeof(s_compiled_profiles[0]);
    for (size_t i = 0; i < compiled_count && s_profiles.count < VIBE_WIFI_MAX_PROFILES; ++i) {
        const compiled_wifi_credential_t *source = &s_compiled_profiles[i];
        if (!source->ssid || source->ssid[0] == '\0') {
            continue;
        }
        vibe_wifi_profile_t *target = &s_profiles.profiles[s_profiles.count++];
        strlcpy(target->ssid, source->ssid, sizeof(target->ssid));
        strlcpy(target->password, source->password ? source->password : "", sizeof(target->password));
    }
}

static esp_err_t save_profiles_locked(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS");
    esp_err_t err = nvs_set_blob(handle, WIFI_NVS_KEY, &s_profiles, sizeof(s_profiles));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t vibe_wifi_profiles_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock");
    load_compiled_profiles();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "using %u compiled Wi-Fi profile(s)", (unsigned)s_profiles.count);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open NVS");

    wifi_profiles_blob_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, WIFI_NVS_KEY, &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "using %u compiled Wi-Fi profile(s)", (unsigned)s_profiles.count);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read profiles");
    if (size != sizeof(stored) ||
        stored.magic != WIFI_BLOB_MAGIC ||
        stored.version != WIFI_BLOB_VERSION ||
        stored.count == 0 ||
        stored.count > VIBE_WIFI_MAX_PROFILES) {
        ESP_LOGW(TAG, "stored Wi-Fi profiles invalid; using compiled profiles");
        return ESP_OK;
    }
    for (size_t i = 0; i < stored.count; ++i) {
        if (!profile_valid(&stored.profiles[i])) {
            ESP_LOGW(TAG, "stored Wi-Fi profile %u invalid; using compiled profiles", (unsigned)i);
            return ESP_OK;
        }
    }
    s_profiles = stored;
    ESP_LOGI(TAG, "loaded %u Wi-Fi profile(s) from NVS", (unsigned)s_profiles.count);
    return ESP_OK;
}

size_t vibe_wifi_profile_count(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    size_t count = s_profiles.count;
    xSemaphoreGive(s_lock);
    return count;
}

bool vibe_wifi_profile_copy(size_t index, vibe_wifi_profile_t *profile)
{
    if (!profile || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool found = index < s_profiles.count;
    if (found) {
        *profile = s_profiles.profiles[index];
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t vibe_wifi_profiles_apply_json(const char *json, bool *changed)
{
    ESP_RETURN_ON_FALSE(json != NULL && changed != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    *changed = false;
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "parse JSON");

    cJSON *configured = cJSON_GetObjectItemCaseSensitive(root, "configured");
    cJSON *profiles = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsTrue(configured)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsArray(profiles)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    wifi_profiles_blob_t incoming = {
        .magic = WIFI_BLOB_MAGIC,
        .version = WIFI_BLOB_VERSION,
    };
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, profiles) {
        if (incoming.count >= VIBE_WIFI_MAX_PROFILES || !cJSON_IsObject(item)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(item, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(item, "password");
        if (!cJSON_IsString(ssid) || !ssid->valuestring ||
            !cJSON_IsString(password) || !password->valuestring ||
            ssid->valuestring[0] == '\0' ||
            strlen(ssid->valuestring) >= VIBE_WIFI_SSID_BYTES ||
            strlen(password->valuestring) >= VIBE_WIFI_PASSWORD_BYTES) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        vibe_wifi_profile_t *profile = &incoming.profiles[incoming.count++];
        strlcpy(profile->ssid, ssid->valuestring, sizeof(profile->ssid));
        strlcpy(profile->password, password->valuestring, sizeof(profile->password));
    }
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(incoming.count > 0, ESP_ERR_INVALID_SIZE, TAG, "empty profiles");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "lock profiles");

    if (memcmp(&incoming, &s_profiles, sizeof(incoming)) != 0) {
        wifi_profiles_blob_t previous = s_profiles;
        s_profiles = incoming;
        esp_err_t err = save_profiles_locked();
        if (err != ESP_OK) {
            s_profiles = previous;
            xSemaphoreGive(s_lock);
            return err;
        }
        *changed = true;
        ESP_LOGI(TAG, "saved %u Wi-Fi profile(s) from USB", (unsigned)s_profiles.count);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
