#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_stick_config.h"
#include "vibe_usb.h"
#include "vibe_wifi.h"
#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vibe_stick_ui_assets.h"
#include "iot_button.h"
#include "lvgl.h"
#include "mdns.h"
#include "nvs_flash.h"

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 135
#define LCD_V_RES 240
#define LCD_X_GAP 52
#define LCD_Y_GAP 40
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX 255
#define LCD_BACKLIGHT_DEFAULT 150
#define LCD_BACKLIGHT_DIMMED 35
#define LCD_DIM_AFTER_MS 60000
#define LVGL_DRAW_BUF_LINES 24
#define LVGL_TICK_PERIOD_MS 10
#define BATTERY_FILL_MAX_WIDTH 20
#define AUDIO_UPLOAD_TX_BUFFER_SIZE 8192
#define AUDIO_UPLOAD_TIMEOUT_MS 60000
#define AUDIO_UPLOAD_ATTEMPTS 3
#define AUDIO_UPLOAD_RETRY_DELAY_MS 500
#define RECORDING_STOP_TIMEOUT_MS 60000
#define RECORDING_RELEASE_DEBOUNCE_MS 80
#define RECORDING_HARD_LIMIT_MS 55000
#define BRIDGE_FAILURES_BEFORE_WIFI_ROTATE 5
#define WIFI_CONNECT_FAILURES_BEFORE_ROTATE 2
#define WIFI_PROFILE_SYNC_MS 5000

#define PIN_BUTTON_FRONT 11
#define PIN_BUTTON_SIDE 12
#define PIN_LCD_MOSI 39
#define PIN_LCD_SCK 40
#define PIN_LCD_DC 45
#define PIN_LCD_CS 41
#define PIN_LCD_RST 21
#define PIN_LCD_BL 38

static const char *TAG = "vibe_stick";

typedef enum {
    VIBE_STICK_EVENT_POLL_STATE,
    VIBE_STICK_EVENT_SHORT_PRESS,
    VIBE_STICK_EVENT_DOUBLE_CLICK,
    VIBE_STICK_EVENT_LONG_START,
    VIBE_STICK_EVENT_LONG_STOP,
    VIBE_STICK_EVENT_PROVIDER_NEXT,
} agent_event_type_t;

typedef struct {
    agent_event_type_t type;
} agent_event_t;

typedef enum {
    PROVIDER_CODEX = 0,
    PROVIDER_CLAUDE = 1,
    PROVIDER_COUNT,
} agent_provider_t;

typedef struct {
    agent_provider_t id;
    const char *key;
    const char *display_name;
    const lv_image_dsc_t *icon;
    lv_color_t accent_color;
    bool enabled;
    bool implemented;
} agent_provider_config_t;

typedef struct {
    char time[8];
    bool wifi;
    bool ble;
    int battery;
    bool battery_charging;
    bool usb_powered;
    char codex_status[24];
    char project[40];
    int quota_5h;
    int quota_7d;
    bool quota_5h_valid;
    bool quota_7d_valid;
    char quota_updated_at[8];
    bool quota_stale;
    char alert_event_id[56];
    char alert_type[24];
    char alert_message[80];
} agent_state_t;

typedef struct {
    char status[24];
    char project[40];
    int quota_5h;
    int quota_7d;
    bool quota_5h_valid;
    bool quota_7d_valid;
    char quota_updated_at[8];
    bool quota_stale;
} provider_display_state_t;

typedef struct {
    char *data;
    int capacity;
    int used;
} http_response_capture_t;

static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_lvgl_lock;
static SemaphoreHandle_t s_bridge_request_lock;
static bool s_wifi_connected;
static size_t s_wifi_network_index;
static unsigned s_wifi_profile_failure_count;
static bool s_wifi_rotation_requested;
static char s_bridge_host[64] = VIBE_STICK_BRIDGE_HOST;
static uint16_t s_bridge_port = VIBE_STICK_BRIDGE_PORT;
static bool s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
static unsigned s_bridge_failure_count;
static bool s_recording_overlay_visible;
static volatile bool s_long_press_active;
static int64_t s_recording_started_ms;
static bool s_recording_confirmation_pending;
static char s_last_alert_event_id[56];
static char s_last_alert_type[24];
static bool s_alert_sound_baseline_ready;
static char s_recording_session_id[40];
static int64_t s_last_user_activity_ms;
static bool s_backlight_dimmed;

static lv_display_t *s_display;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_wifi_name_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_cap;
static lv_obj_t *s_battery_bolt;
static lv_obj_t *s_provider_icon;
static lv_obj_t *s_provider_label;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_label;
static lv_obj_t *s_quota_7d_title_label;
static lv_obj_t *s_quota_7d_bar;
static lv_obj_t *s_quota_7d_label;
static lv_obj_t *s_quota_status_label;

static const int s_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int s_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};

static uint8_t *ima_adpcm_encode(const int16_t *samples, size_t sample_count, size_t *encoded_len)
{
    if (!samples || sample_count == 0 || !encoded_len) {
        return NULL;
    }
    size_t code_count = sample_count - 1;
    size_t output_len = 4 + (code_count + 1) / 2;
    uint8_t *output = heap_caps_calloc(1, output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = calloc(1, output_len);
    }
    if (!output) {
        return NULL;
    }

    int predictor = samples[0];
    int index = 0;
    output[0] = (uint8_t)(predictor & 0xff);
    output[1] = (uint8_t)((predictor >> 8) & 0xff);
    output[2] = (uint8_t)index;

    for (size_t i = 1; i < sample_count; ++i) {
        int step = s_ima_step_table[index];
        int difference = (int)samples[i] - predictor;
        int code = 0;
        if (difference < 0) {
            code = 8;
            difference = -difference;
        }
        int delta = step >> 3;
        if (difference >= step) {
            code |= 4;
            difference -= step;
            delta += step;
        }
        if (difference >= (step >> 1)) {
            code |= 2;
            difference -= step >> 1;
            delta += step >> 1;
        }
        if (difference >= (step >> 2)) {
            code |= 1;
            delta += step >> 2;
        }
        predictor += (code & 8) ? -delta : delta;
        if (predictor > INT16_MAX) predictor = INT16_MAX;
        if (predictor < INT16_MIN) predictor = INT16_MIN;
        index += s_ima_index_table[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        size_t code_index = i - 1;
        if ((code_index & 1) == 0) {
            output[4 + code_index / 2] = (uint8_t)(code & 0x0f);
        } else {
            output[4 + code_index / 2] |= (uint8_t)((code & 0x0f) << 4);
        }
    }
    *encoded_len = output_len;
    return output;
}
static lv_obj_t *s_recording_overlay;
static lv_obj_t *s_recording_wave_group;
static lv_obj_t *s_recording_wave_bars[5];
static lv_obj_t *s_recording_title;
static lv_obj_t *s_recording_hint;

static agent_state_t s_state = {
    .time = "--:--",
    .wifi = false,
    .ble = false,
    .battery = 0,
    .battery_charging = false,
    .usb_powered = false,
    .codex_status = "OFFLINE",
    .project = "vibestick",
    .quota_5h = 0,
    .quota_7d = 0,
    .quota_5h_valid = false,
    .quota_7d_valid = false,
    .quota_updated_at = "",
    .quota_stale = false,
    .alert_event_id = "",
    .alert_type = "NONE",
    .alert_message = "",
};

static provider_display_state_t s_provider_states[PROVIDER_COUNT] = {
    [PROVIDER_CODEX] = {
        .status = "OFFLINE",
        .project = "vibestick",
        .quota_5h = 0,
        .quota_7d = 0,
        .quota_5h_valid = false,
        .quota_7d_valid = false,
        .quota_updated_at = "",
        .quota_stale = false,
    },
    [PROVIDER_CLAUDE] = {
        .status = "OFFLINE",
        .project = "vibestick",
        .quota_5h = 0,
        .quota_7d = 0,
        .quota_5h_valid = false,
        .quota_7d_valid = false,
        .quota_updated_at = "",
        .quota_stale = false,
    },
};

static const agent_provider_config_t s_provider_configs[] = {
    {
        .id = PROVIDER_CODEX,
        .key = "codex",
        .display_name = "Codex",
        .icon = &vibe_stick_provider_codex_icon_40,
        .accent_color = LV_COLOR_MAKE(0x4d, 0x82, 0xff),
        .enabled = true,
        .implemented = true,
    },
    {
        .id = PROVIDER_CLAUDE,
        .key = "claude",
        .display_name = "Claude",
        .icon = &vibe_stick_provider_claude_icon_40,
        .accent_color = LV_COLOR_MAKE(0xd9, 0x77, 0x57),
        .enabled = true,
        .implemented = true,
    },
};

static agent_provider_t s_current_provider = PROVIDER_CODEX;
static bool s_provider_manually_selected;

static const lv_point_precise_t s_battery_bolt_points[] = {
    {3, 0},
    {1, 3},
    {3, 3},
    {2, 7},
    {6, 2},
    {4, 2},
};

static void render_state(void);

static bool queue_event(agent_event_type_t type)
{
    if (!s_event_queue) {
        return false;
    }
    agent_event_t event = {.type = type};
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE;
}

static const agent_provider_config_t *provider_config(agent_provider_t provider)
{
    for (size_t i = 0; i < sizeof(s_provider_configs) / sizeof(s_provider_configs[0]); ++i) {
        if (s_provider_configs[i].id == provider) {
            return &s_provider_configs[i];
        }
    }
    return &s_provider_configs[0];
}

static const agent_provider_config_t *current_provider_config(void)
{
    return provider_config(s_current_provider);
}

static provider_display_state_t *provider_display_state(agent_provider_t provider)
{
    if ((int)provider >= 0 && provider < PROVIDER_COUNT) {
        return &s_provider_states[provider];
    }
    return &s_provider_states[PROVIDER_CODEX];
}

static provider_display_state_t *current_provider_display_state(void)
{
    return provider_display_state(s_current_provider);
}

static bool provider_from_key(const char *key, agent_provider_t *provider)
{
    if (!key || key[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_provider_configs) / sizeof(s_provider_configs[0]); ++i) {
        if (strcmp(s_provider_configs[i].key, key) == 0) {
            if (provider) {
                *provider = s_provider_configs[i].id;
            }
            return true;
        }
    }
    return false;
}

static bool set_current_provider_from_key(const char *key)
{
    agent_provider_t provider = PROVIDER_CODEX;
    if (provider_from_key(key, &provider)) {
        s_current_provider = provider;
        return true;
    }
    return false;
}

static agent_provider_t next_enabled_provider(agent_provider_t current)
{
    const size_t count = sizeof(s_provider_configs) / sizeof(s_provider_configs[0]);
    size_t current_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s_provider_configs[i].id == current) {
            current_index = i;
            break;
        }
    }
    for (size_t offset = 1; offset <= count; ++offset) {
        const size_t candidate_index = (current_index + offset) % count;
        if (s_provider_configs[candidate_index].enabled) {
            return s_provider_configs[candidate_index].id;
        }
    }
    return PROVIDER_CODEX;
}

static void switch_provider(void)
{
    if (s_recording_overlay_visible) {
        ESP_LOGI(TAG, "provider switch ignored while overlay is visible");
        return;
    }
    s_current_provider = next_enabled_provider(s_current_provider);
    s_provider_manually_selected = true;
    const agent_provider_config_t *provider = current_provider_config();
    ESP_LOGI(TAG, "provider switched to %s", provider->key);
    render_state();
}

static void lvgl_lock(void)
{
    if (s_lvgl_lock) {
        xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    }
}

static void lvgl_unlock(void)
{
    if (s_lvgl_lock) {
        xSemaphoreGive(s_lvgl_lock);
    }
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        lvgl_lock();
        uint32_t wait_ms = lv_timer_handler();
        lvgl_unlock();
        if (wait_ms < 5) {
            wait_ms = 5;
        }
        if (wait_ms > 250) {
            wait_ms = 250;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, width * height);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void set_backlight(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void mark_user_activity(void)
{
    s_last_user_activity_ms = esp_timer_get_time() / 1000;
    if (s_backlight_dimmed) {
        set_backlight(LCD_BACKLIGHT_DEFAULT);
        s_backlight_dimmed = false;
    }
}

static void maybe_dim_display(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        s_last_user_activity_ms = now_ms;
        return;
    }
    if (!s_backlight_dimmed &&
        s_last_user_activity_ms > 0 &&
        now_ms - s_last_user_activity_ms >= LCD_DIM_AFTER_MS) {
        set_backlight(LCD_BACKLIGHT_DIMMED);
        s_backlight_dimmed = true;
    }
}

static void init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    set_backlight(LCD_BACKLIGHT_DEFAULT);
    s_last_user_activity_ms = esp_timer_get_time() / 1000;
}

static esp_err_t init_display(void)
{
    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle),
                        TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP), TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(s_display, panel);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    size_t buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "lvgl buffer");
    lv_display_set_buffers(s_display, buf, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io_handle, &callbacks, s_display),
                        TAG, "panel cb");

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "tick start");

    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 3, NULL);
    return ESP_OK;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, int32_t width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int32_t width)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, width, 5);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2a2d33), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xf4f5f7), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    return bar;
}

static lv_obj_t *make_plain_obj(lv_obj_t *parent, int32_t w, int32_t h,
                                lv_color_t color, lv_opa_t opa, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static void create_provider_icon(lv_obj_t *parent)
{
    s_provider_icon = lv_image_create(parent);
    lv_image_set_src(s_provider_icon, current_provider_config()->icon);
    lv_obj_align(s_provider_icon, LV_ALIGN_TOP_LEFT, 18, 52);
}

static const char *status_text_for(const char *status)
{
    if (strcmp(status, "RUNNING") == 0) {
        return "RUNNING";
    }
    if (strcmp(status, "DONE") == 0) {
        return "DONE";
    }
    if (strcmp(status, "APPROVAL") == 0) {
        return "APPROVAL";
    }
    if (strcmp(status, "ERROR") == 0) {
        return "ERROR";
    }
    if (strcmp(status, "OFFLINE") == 0) {
        return "OFFLINE";
    }
    if (strcmp(status, "IDLE") == 0 || strcmp(status, "UNKNOWN") == 0) {
        return "IDLE";
    }
    return "IDLE";
}

static void set_battery_ui(int battery_value, bool charging, bool usb_powered)
{
    if (battery_value < 0) {
        battery_value = 0;
    } else if (battery_value > 100) {
        battery_value = 100;
    }

    char battery[8];
    if (battery_value > 0) {
        snprintf(battery, sizeof(battery), "%d%%", battery_value);
    } else {
        snprintf(battery, sizeof(battery), "--%%");
    }
    lv_label_set_text(s_battery_label, battery);

    int fill_width = battery_value > 0 ? (battery_value * 20) / 100 : 0;
    if (fill_width < 1 && battery_value > 0) {
        fill_width = 1;
    }

    const bool external_power = charging || usb_powered;
    const lv_color_t normal_color = lv_color_hex(0xf3f4f6);
    const lv_color_t charging_color = lv_color_hex(0x32d583);

    lv_obj_set_style_border_color(s_battery_icon, normal_color, 0);
    lv_obj_set_style_bg_color(s_battery_fill, external_power ? charging_color : normal_color, 0);
    lv_obj_set_style_bg_color(s_battery_cap, normal_color, 0);
    lv_obj_set_width(s_battery_fill, fill_width);

    if (s_battery_bolt) {
        if (external_power) {
            lv_obj_clear_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void wave_bar_height_cb(void *obj, int32_t height)
{
    lv_obj_set_height((lv_obj_t *)obj, height);
}

static void stop_recording_wave(void)
{
    static const int heights[5] = {14, 22, 32, 22, 14};
    for (int i = 0; i < 5; ++i) {
        if (s_recording_wave_bars[i]) {
            lv_anim_delete(s_recording_wave_bars[i], NULL);
            lv_obj_set_height(s_recording_wave_bars[i], heights[i]);
        }
    }
}

static void start_recording_wave(void)
{
    static const int min_heights[5] = {10, 14, 18, 14, 10};
    static const int max_heights[5] = {24, 34, 48, 34, 24};
    stop_recording_wave();
    for (int i = 0; i < 5; ++i) {
        if (!s_recording_wave_bars[i]) {
            continue;
        }
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_recording_wave_bars[i]);
        lv_anim_set_values(&anim, min_heights[i], max_heights[i]);
        lv_anim_set_duration(&anim, 460);
        lv_anim_set_playback_duration(&anim, 460);
        lv_anim_set_delay(&anim, i * 70);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&anim, wave_bar_height_cb);
        lv_anim_start(&anim);
    }
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050608), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    s_wifi_label = make_label(screen, "WIFI", &lv_font_montserrat_10, lv_color_hex(0xf3f4f6), 38, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_LEFT, 9, 9);
    s_wifi_name_label = make_label(screen, "", &lv_font_montserrat_10,
                                   lv_color_hex(0x9aa0aa), 117, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_wifi_name_label, LV_ALIGN_TOP_LEFT, 9, 25);

    s_battery_label = make_label(screen, "--%", &lv_font_montserrat_10, lv_color_hex(0xf3f4f6), 28, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -35, 9);
    s_battery_icon = make_plain_obj(screen, 26, 13, lv_color_hex(0x000000), LV_OPA_TRANSP, 3);
    lv_obj_set_style_border_width(s_battery_icon, 1, 0);
    lv_obj_set_style_border_color(s_battery_icon, lv_color_hex(0xf3f4f6), 0);
    lv_obj_align(s_battery_icon, LV_ALIGN_TOP_RIGHT, -7, 9);
    s_battery_fill = make_plain_obj(s_battery_icon, 1, 9, lv_color_hex(0xf3f4f6), LV_OPA_COVER, 2);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);
    s_battery_bolt = lv_line_create(s_battery_icon);
    lv_line_set_points(s_battery_bolt, s_battery_bolt_points,
                       sizeof(s_battery_bolt_points) / sizeof(s_battery_bolt_points[0]));
    lv_obj_set_style_line_width(s_battery_bolt, 1, 0);
    lv_obj_set_style_line_color(s_battery_bolt, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_line_rounded(s_battery_bolt, true, 0);
    lv_obj_align(s_battery_bolt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
    s_battery_cap = make_plain_obj(screen, 2, 7, lv_color_hex(0xf3f4f6), LV_OPA_COVER, 1);
    lv_obj_align_to(s_battery_cap, s_battery_icon, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    create_provider_icon(screen);

    s_status_dot = lv_obj_create(screen);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 7, 7);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xf3f4f6), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_align(s_status_dot, LV_ALIGN_TOP_LEFT, 72, 80);

    s_provider_label = make_label(screen, "Codex", &lv_font_montserrat_16, lv_color_hex(0xf3f4f6), 60, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_provider_label, LV_ALIGN_TOP_LEFT, 72, 51);

    s_status_label = make_label(screen, "IDLE", &lv_font_montserrat_12,
                                lv_color_hex(0xf3f4f6), 52, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 82, 73);

    lv_obj_t *quota_wrap = make_plain_obj(screen, LCD_H_RES - 16, 104, lv_color_hex(0x0e1014), LV_OPA_COVER, 8);
    lv_obj_set_style_border_width(quota_wrap, 1, 0);
    lv_obj_set_style_border_color(quota_wrap, lv_color_hex(0x22252b), 0);
    lv_obj_align(quota_wrap, LV_ALIGN_TOP_MID, 0, 118);

    s_quota_7d_title_label = make_label(screen, "7D", &lv_font_montserrat_12,
                                        lv_color_hex(0x8a9099), 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_quota_7d_title_label, LV_ALIGN_TOP_MID, 0, 133);
    s_quota_7d_label = make_label(screen, "--%", &lv_font_montserrat_20,
                                  lv_color_hex(0xf3f4f6), 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_quota_7d_label, LV_ALIGN_TOP_MID, 0, 153);
    s_quota_7d_bar = make_bar(screen, 100);
    lv_obj_align(s_quota_7d_bar, LV_ALIGN_TOP_MID, 0, 190);
    s_quota_status_label = make_label(screen, "WAIT", &lv_font_montserrat_10,
                                      lv_color_hex(0x686e78), 84, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_quota_status_label, LV_ALIGN_TOP_MID, 0, 207);
    lv_obj_add_flag(s_quota_status_label, LV_OBJ_FLAG_HIDDEN);

    s_recording_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_recording_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_recording_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_recording_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_recording_overlay, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_opa(s_recording_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_recording_overlay, 0, 0);
    lv_obj_add_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);

    s_recording_wave_group = lv_obj_create(s_recording_overlay);
    lv_obj_remove_style_all(s_recording_wave_group);
    lv_obj_set_size(s_recording_wave_group, 82, 58);
    lv_obj_set_flex_flow(s_recording_wave_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_recording_wave_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_recording_wave_group, 6, 0);
    lv_obj_align(s_recording_wave_group, LV_ALIGN_CENTER, 0, -34);
    static const int initial_wave_heights[5] = {14, 22, 32, 22, 14};
    for (int i = 0; i < 5; ++i) {
        s_recording_wave_bars[i] = make_plain_obj(s_recording_wave_group, 6, initial_wave_heights[i],
                                                  lv_color_hex(0xf4f5f7), LV_OPA_COVER, 3);
    }

    s_recording_title = make_label(s_recording_overlay, "LISTENING", &lv_font_montserrat_16,
                                   lv_color_hex(0xf4f5f7), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_recording_title, LV_ALIGN_CENTER, 0, 22);
    s_recording_hint = make_label(s_recording_overlay, "Release to send", &lv_font_montserrat_12,
                                  lv_color_hex(0x8b9098), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_recording_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void set_quota_label(lv_obj_t *bar, lv_obj_t *label, int value, bool valid, lv_color_t accent_color)
{
    lv_obj_set_style_bg_color(bar, valid ? accent_color : lv_color_hex(0x4b4f57), LV_PART_INDICATOR);
    if (!valid) {
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_label_set_text(label, "--%");
        return;
    }
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", value);
    lv_label_set_text(label, text);
}

static void set_quota_title(lv_obj_t *label, const char *prefix, bool stale)
{
    if (stale) {
        char text[8];
        snprintf(text, sizeof(text), "%s*", prefix);
        lv_label_set_text(label, text);
    } else {
        lv_label_set_text(label, prefix);
    }
}

static void set_status_color(const agent_provider_config_t *provider, const char *status)
{
    lv_color_t color = lv_color_hex(0x9aa0aa);
    if (!provider->implemented) {
        color = lv_color_hex(0x9aa0aa);
    } else if (strcmp(status, "RUNNING") == 0 || strcmp(status, "DONE") == 0) {
        color = provider->accent_color;
    } else if (strcmp(status, "APPROVAL") == 0) {
        color = lv_color_hex(0xcfd3da);
    } else if (strcmp(status, "IDLE") == 0 || strcmp(status, "UNKNOWN") == 0) {
        color = lv_color_hex(0x9aa0aa);
    } else if (strcmp(status, "ERROR") == 0 || strcmp(status, "OFFLINE") == 0) {
        color = lv_color_hex(0x686e78);
    }
    lv_obj_set_style_bg_color(s_status_dot, color, 0);
}

static void current_wifi_name(char *name, size_t name_len)
{
    if (!name || name_len == 0) {
        return;
    }
    name[0] = '\0';
    if (s_wifi_connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            strlcpy(name, (const char *)ap_info.ssid, name_len);
            return;
        }
    }
    size_t count = vibe_wifi_profile_count();
    vibe_wifi_profile_t profile = {0};
    if (count > 0 && vibe_wifi_profile_copy(s_wifi_network_index % count, &profile)) {
        strlcpy(name, profile.ssid, name_len);
    }
}

static void render_state(void)
{
    lvgl_lock();
    const agent_provider_config_t *provider = current_provider_config();
    const provider_display_state_t *display_state = current_provider_display_state();
    const bool implemented = provider->implemented;
    const bool q7_valid = implemented && display_state->quota_7d_valid;
    const bool quota_stale = implemented && display_state->quota_stale;
    const char *status_key = implemented ? display_state->status : "UNIMPLEMENTED";

    const bool usb_runtime = vibe_usb_ready();
    const bool bridge_transport_available = usb_runtime || s_wifi_connected;
    lv_label_set_text(s_wifi_label, usb_runtime ? "USB" : (s_wifi_connected ? "WIFI" : "OFF"));
    lv_obj_set_style_text_color(s_wifi_label,
                                bridge_transport_available ? lv_color_hex(0xf3f4f6) : lv_color_hex(0x686e78),
                                0);
    char network_name[VIBE_WIFI_SSID_BYTES] = {0};
    current_wifi_name(network_name, sizeof(network_name));
    char network_text[48] = {0};
    if (s_wifi_connected) {
        strlcpy(network_text, network_name, sizeof(network_text));
    } else if (network_name[0] != '\0') {
        snprintf(network_text, sizeof(network_text), "TRY %s", network_name);
    }
    lv_label_set_text(s_wifi_name_label, network_text);
    lv_obj_set_style_text_color(s_wifi_name_label,
                                s_wifi_connected ? lv_color_hex(0x9aa0aa) : lv_color_hex(0x686e78),
                                0);
    set_battery_ui(s_state.battery, s_state.battery_charging, s_state.usb_powered);
    if (provider->icon) {
        lv_image_set_src(s_provider_icon, provider->icon);
        lv_obj_clear_flag(s_provider_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_provider_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_provider_label, provider->display_name);
    lv_obj_set_style_text_color(s_provider_label, provider->implemented ? lv_color_hex(0xf3f4f6) : lv_color_hex(0xd7d9de), 0);
    lv_label_set_text(s_status_label, implemented ? status_text_for(display_state->status) : "IDLE");
    set_status_color(provider, status_key);
    set_quota_title(s_quota_7d_title_label, "7D", quota_stale);
    set_quota_label(s_quota_7d_bar, s_quota_7d_label, display_state->quota_7d,
                    q7_valid, provider->accent_color);
    lv_label_set_text(s_quota_status_label, "");
    lv_obj_add_flag(s_quota_status_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

static void show_recording_overlay(const char *title, const char *hint, bool visible)
{
    lvgl_lock();
    if (visible) {
        if (title) {
            lv_label_set_text(s_recording_title, title);
        }
        if (hint) {
            lv_label_set_text(s_recording_hint, hint);
            if (hint[0] == '\0') {
                lv_obj_add_flag(s_recording_hint, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_recording_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_clear_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);
        start_recording_wave();
    } else {
        stop_recording_wave();
        lv_obj_add_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_recording_overlay_visible = visible;
    lvgl_unlock();
}

static bool sound_for_alert_type(const char *type, agent_sound_t *sound)
{
    if (strcmp(type, "DONE") == 0 ||
        strcmp(type, "COMPLETED") == 0 ||
        strcmp(type, "SUCCESS") == 0) {
        *sound = VIBE_STICK_SOUND_DONE;
        return true;
    }
    if (strcmp(type, "ERROR") == 0 ||
        strcmp(type, "FAILED") == 0 ||
        strcmp(type, "FAILURE") == 0) {
        *sound = VIBE_STICK_SOUND_ERROR;
        return true;
    }
    if (strcmp(type, "APPROVAL") == 0 ||
        strcmp(type, "WAITING_APPROVAL") == 0 ||
        strcmp(type, "PENDING_APPROVAL") == 0 ||
        strcmp(type, "NEEDS_APPROVAL") == 0) {
        *sound = VIBE_STICK_SOUND_APPROVAL;
        return true;
    }
    return false;
}

static void remember_alert_sound_baseline(void)
{
    strlcpy(s_last_alert_event_id, s_state.alert_event_id, sizeof(s_last_alert_event_id));
    strlcpy(s_last_alert_type, s_state.alert_type, sizeof(s_last_alert_type));
    s_alert_sound_baseline_ready = true;
}

static bool should_play_alert_sound(void)
{
    agent_sound_t ignored;
    const bool target = sound_for_alert_type(s_state.alert_type, &ignored);

    if (!s_alert_sound_baseline_ready) {
        remember_alert_sound_baseline();
        return false;
    }

    if (!target) {
        remember_alert_sound_baseline();
        return false;
    }

    bool should_play = false;
    if (s_state.alert_event_id[0] != '\0') {
        should_play = strcmp(s_last_alert_event_id, s_state.alert_event_id) != 0;
    } else {
        should_play = strcmp(s_last_alert_type, s_state.alert_type) != 0;
    }
    remember_alert_sound_baseline();
    return should_play;
}

static void maybe_handle_alert(void)
{
    agent_sound_t sound;
    if (!sound_for_alert_type(s_state.alert_type, &sound)) {
        (void)should_play_alert_sound();
        return;
    }
    if (!should_play_alert_sound()) {
        return;
    }
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        ESP_LOGI(TAG, "skip alert sound while recording overlay is active type=%s",
                 s_state.alert_type);
        return;
    }

    esp_err_t err = vibe_audio_play_sound(sound);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "alert sound skipped type=%s err=%s",
                 s_state.alert_type, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "alert type=%s project=%s message=%s",
             s_state.alert_type, s_state.project, s_state.alert_message);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_response_capture_t *capture = (http_response_capture_t *)evt->user_data;
    if (!capture->data || capture->capacity <= 0 || capture->used >= capture->capacity - 1) {
        return ESP_OK;
    }

    int remaining = capture->capacity - 1 - capture->used;
    int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
    memcpy(capture->data + capture->used, evt->data, copy_len);
    capture->used += copy_len;
    capture->data[capture->used] = '\0';
    return ESP_OK;
}

static bool mdns_result_is_vibestick(const mdns_result_t *result)
{
    if (!result) {
        return false;
    }
    for (size_t i = 0; i < result->txt_count; i++) {
        const mdns_txt_item_t *item = &result->txt[i];
        if (item->key && item->value &&
            strcmp(item->key, "name") == 0 &&
            strcmp(item->value, "vibestick-bridge") == 0) {
            return true;
        }
    }
    return false;
}

static bool discover_bridge_endpoint(void)
{
#if VIBE_STICK_BRIDGE_DISCOVERY
    static int64_t last_attempt_ms;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_attempt_ms > 0 && now_ms - last_attempt_ms < 3000) {
        return false;
    }
    last_attempt_ms = now_ms;

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(
        VIBE_STICK_BRIDGE_SERVICE_TYPE,
        VIBE_STICK_BRIDGE_SERVICE_PROTOCOL,
        VIBE_STICK_BRIDGE_DISCOVERY_TIMEOUT_MS,
        8,
        &results
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bridge discovery failed: %s", esp_err_to_name(err));
        return false;
    }

    bool found = false;
    for (mdns_result_t *result = results; result && !found; result = result->next) {
        if (!mdns_result_is_vibestick(result) || result->port == 0) {
            continue;
        }
        for (mdns_ip_addr_t *address = result->addr; address; address = address->next) {
            if (address->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            char host[sizeof(s_bridge_host)] = {0};
            if (!esp_ip4addr_ntoa(&address->addr.u_addr.ip4, host, sizeof(host))) {
                continue;
            }
            strlcpy(s_bridge_host, host, sizeof(s_bridge_host));
            s_bridge_port = result->port;
            found = true;
            ESP_LOGI(TAG, "bridge discovered host=%s port=%u instance=%s",
                     s_bridge_host, (unsigned)s_bridge_port,
                     result->instance_name ? result->instance_name : "");
            break;
        }
    }
    mdns_query_results_free(results);
    if (!found) {
        ESP_LOGW(TAG, "VibeStick Bridge service not found; keeping compatibility fallback");
    }
    return found;
#else
    return false;
#endif
}

static void ensure_bridge_endpoint(void)
{
    if (!s_bridge_discovery_required || !s_wifi_connected) {
        return;
    }
    if (discover_bridge_endpoint()) {
        s_bridge_discovery_required = false;
    }
}

static esp_err_t http_request_timeout(const char *method, const char *path, const char *body,
                                      char *response, int response_len, int timeout_ms)
{
    ensure_bridge_endpoint();
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u%s", s_bridge_host, (unsigned)s_bridge_port, path);
    http_response_capture_t capture = {
        .data = response,
        .capacity = response_len,
        .used = 0,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", __DATE__ " " __TIME__);
    if (strlen(VIBE_STICK_BRIDGE_TOKEN) > 0) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && response && response_len > 0 && capture.used == 0) {
        ESP_LOGW(TAG, "http %s %s status=%d empty response", method, path, status_code);
    }
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        s_bridge_discovery_required = false;
    } else if (err != ESP_OK) {
        s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
    }
    return err;
}

static esp_err_t http_post_binary(const char *path, const uint8_t *body, size_t body_len,
                                  const char *encoding, size_t pcm_samples,
                                  char *response, int response_len)
{
    ensure_bridge_endpoint();
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%u%s", s_bridge_host, (unsigned)s_bridge_port, path);
    http_response_capture_t capture = {
        .data = response,
        .capacity = response_len,
        .used = 0,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = AUDIO_UPLOAD_TIMEOUT_MS,
        .buffer_size_tx = AUDIO_UPLOAD_TX_BUFFER_SIZE,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", __DATE__ " " __TIME__);
    if (strlen(VIBE_STICK_BRIDGE_TOKEN) > 0) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Vibe-Stick-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Vibe-Stick-Channels", "1");
    esp_http_client_set_header(client, "X-Vibe-Stick-Bits-Per-Sample", "16");
    if (encoding && encoding[0] != '\0') {
        char sample_count[24];
        snprintf(sample_count, sizeof(sample_count), "%u", (unsigned)pcm_samples);
        esp_http_client_set_header(client, "X-Vibe-Stick-Audio-Encoding", encoding);
        esp_http_client_set_header(client, "X-Vibe-Stick-PCM-Samples", sample_count);
    }
    esp_http_client_set_post_field(client, (const char *)body, body_len);

    wifi_ps_type_t previous_ps = WIFI_PS_MIN_MODEM;
    bool restore_wifi_ps = esp_wifi_get_ps(&previous_ps) == ESP_OK;
    if (previous_ps != WIFI_PS_NONE) {
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "disable Wi-Fi power save for upload failed: %s", esp_err_to_name(ps_err));
            restore_wifi_ps = false;
        }
    }

    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    int status_code = esp_http_client_get_status_code(client);
    if (restore_wifi_ps && previous_ps != WIFI_PS_NONE) {
        esp_err_t ps_err = esp_wifi_set_ps(previous_ps);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "restore Wi-Fi power save after upload failed: %s", esp_err_to_name(ps_err));
        }
    }
    ESP_LOGI(TAG, "audio upload bytes=%u elapsed_ms=%lld status=%d result=%s",
             (unsigned)body_len, (long long)elapsed_ms, status_code, esp_err_to_name(err));
    if (err == ESP_OK && response && response_len > 0 && capture.used == 0) {
        ESP_LOGW(TAG, "http POST %s status=%d empty response", path, status_code);
    }
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        s_bridge_discovery_required = false;
    } else if (err != ESP_OK) {
        s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
    }
    return err;
}

static esp_err_t bridge_request_timeout(const char *method, const char *path, const char *body,
                                        char *response, int response_len, int timeout_ms)
{
    if (!s_bridge_request_lock ||
        xSemaphoreTake(s_bridge_request_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (vibe_usb_ready()) {
        err = vibe_usb_request(method, path, body, response, response_len, timeout_ms);
        ESP_LOGI(TAG, "bridge %s %s transport=USB result=%s",
                 method, path, esp_err_to_name(err));
    } else if (!s_wifi_connected) {
        if (response && response_len > 0) {
            response[0] = '\0';
        }
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = http_request_timeout(method, path, body, response, response_len, timeout_ms);
    }
    xSemaphoreGive(s_bridge_request_lock);
    return err;
}

static esp_err_t bridge_request(const char *method, const char *path, const char *body,
                                char *response, int response_len)
{
    return bridge_request_timeout(method, path, body, response, response_len, 2500);
}

static esp_err_t bridge_post_audio_unlocked(const uint8_t *body, size_t body_len,
                                            char *response, int response_len)
{
    if (vibe_usb_ready()) {
        int64_t started_us = esp_timer_get_time();
        esp_err_t err = vibe_usb_post_audio(
            s_recording_session_id,
            body,
            body_len,
            response,
            response_len,
            AUDIO_UPLOAD_TIMEOUT_MS
        );
        ESP_LOGI(TAG, "audio upload bytes=%u elapsed_ms=%lld transport=USB result=%s",
                 (unsigned)body_len,
                 (long long)((esp_timer_get_time() - started_us) / 1000),
                 esp_err_to_name(err));
        return err;
    }
    if (!s_wifi_connected) {
        if (response && response_len > 0) {
            response[0] = '\0';
        }
        return ESP_ERR_INVALID_STATE;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s?compact=1&session_id=%s",
             VIBE_STICK_RECORDING_AUDIO_PATH, s_recording_session_id);
    size_t sample_count = body_len / sizeof(int16_t);
    size_t encoded_len = 0;
    uint8_t *encoded = ima_adpcm_encode((const int16_t *)body, sample_count, &encoded_len);
    if (!encoded) {
        ESP_LOGW(TAG, "IMA ADPCM allocation failed; uploading PCM");
        return http_post_binary(path, body, body_len, NULL, 0, response, response_len);
    }
    ESP_LOGI(TAG, "audio compressed pcm_bytes=%u adpcm_bytes=%u",
             (unsigned)body_len, (unsigned)encoded_len);
    esp_err_t err = http_post_binary(path, encoded, encoded_len, "ima-adpcm",
                                     sample_count, response, response_len);
    free(encoded);
    return err;
}

static esp_err_t bridge_post_audio(const uint8_t *body, size_t body_len,
                                   char *response, int response_len)
{
    if (!s_bridge_request_lock ||
        xSemaphoreTake(s_bridge_request_lock, pdMS_TO_TICKS(AUDIO_UPLOAD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = bridge_post_audio_unlocked(body, body_len, response, response_len);
    xSemaphoreGive(s_bridge_request_lock);
    return err;
}

static void copy_json_string(cJSON *root, const char *key, char *target, size_t target_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(target, item->valuestring, target_len);
    }
}

static bool json_percent_value(cJSON *item, int *value)
{
    if (cJSON_IsNumber(item)) {
        *value = item->valueint;
    } else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(item->valuestring, &end, 10);
        if (!end || end == item->valuestring) {
            return false;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == '%') {
            end++;
        }
        if (*end != '\0') {
            return false;
        }
        *value = (int)parsed;
    } else {
        return false;
    }
    if (*value < 0) {
        *value = 0;
    } else if (*value > 100) {
        *value = 100;
    }
    return true;
}

static void parse_provider_fields(cJSON *source, provider_display_state_t *target)
{
    copy_json_string(source, "status", target->status, sizeof(target->status));
    copy_json_string(source, "project", target->project, sizeof(target->project));
    copy_json_string(source, "quota_updated_at", target->quota_updated_at, sizeof(target->quota_updated_at));

    cJSON *quota_5h = cJSON_GetObjectItemCaseSensitive(source, "quota_5h_remaining");
    cJSON *quota_7d = cJSON_GetObjectItemCaseSensitive(source, "quota_7d_remaining");
    cJSON *stale = cJSON_GetObjectItemCaseSensitive(source, "quota_stale");
    int quota_value = 0;
    target->quota_5h_valid = json_percent_value(quota_5h, &quota_value);
    if (target->quota_5h_valid) {
        target->quota_5h = quota_value;
    }
    target->quota_7d_valid = json_percent_value(quota_7d, &quota_value);
    if (target->quota_7d_valid) {
        target->quota_7d = quota_value;
    }
    target->quota_stale = cJSON_IsBool(stale) ? cJSON_IsTrue(stale) : false;
}

static void parse_provider_json(cJSON *state_root, cJSON *provider)
{
    char provider_key[16] = "";
    copy_json_string(provider, "id", provider_key, sizeof(provider_key));
    if (provider_key[0] == '\0') {
        copy_json_string(state_root, "active_provider", provider_key, sizeof(provider_key));
    }
    agent_provider_t provider_id = s_current_provider;
    if (provider_key[0] != '\0' && provider_from_key(provider_key, &provider_id)) {
        if (!s_provider_manually_selected) {
            s_current_provider = provider_id;
        }
    }

    provider_display_state_t *display_state = provider_display_state(provider_id);
    parse_provider_fields(provider, display_state);
    ESP_LOGI(TAG, "provider parsed key=%s status=%s q5=%s%d q7=%s%d stale=%d",
             provider_config(provider_id)->key,
             display_state->status,
             display_state->quota_5h_valid ? "" : "invalid:",
             display_state->quota_5h,
             display_state->quota_7d_valid ? "" : "invalid:",
             display_state->quota_7d,
             display_state->quota_stale);
}

static void parse_codex_json(cJSON *codex)
{
    provider_display_state_t *display_state = provider_display_state(PROVIDER_CODEX);
    parse_provider_fields(codex, display_state);
    ESP_LOGI(TAG, "codex parsed status=%s q5=%s%d q7=%s%d stale=%d",
             display_state->status,
             display_state->quota_5h_valid ? "" : "invalid:",
             display_state->quota_5h,
             display_state->quota_7d_valid ? "" : "invalid:",
             display_state->quota_7d,
             display_state->quota_stale);
}

static bool parse_state_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *bridge_name = cJSON_GetObjectItemCaseSensitive(root, "bridge_name");
    if (cJSON_IsString(bridge_name) && bridge_name->valuestring &&
        strcmp(bridge_name->valuestring, "vibestick-bridge") != 0) {
        cJSON_Delete(root);
        s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
        return false;
    }
    cJSON *state_root = root;
    cJSON *wrapped_state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsObject(wrapped_state)) {
        state_root = wrapped_state;
    }

    copy_json_string(state_root, "time", s_state.time, sizeof(s_state.time));
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(state_root, "wifi");
    cJSON *ble = cJSON_GetObjectItemCaseSensitive(state_root, "ble");
    s_state.wifi = cJSON_IsBool(wifi) ? cJSON_IsTrue(wifi) : s_state.wifi;
    s_state.ble = cJSON_IsBool(ble) ? cJSON_IsTrue(ble) : s_state.ble;

    cJSON *provider = cJSON_GetObjectItemCaseSensitive(state_root, "provider");
    cJSON *codex = cJSON_GetObjectItemCaseSensitive(state_root, "codex");
    if (cJSON_IsObject(provider)) {
        parse_provider_json(state_root, provider);
    } else {
        char active_provider[16] = "";
        copy_json_string(state_root, "active_provider", active_provider, sizeof(active_provider));
        if (active_provider[0] != '\0' && !s_provider_manually_selected) {
            set_current_provider_from_key(active_provider);
        }
    }
    if (cJSON_IsObject(codex)) {
        parse_codex_json(codex);
    }

    cJSON *alert = cJSON_GetObjectItemCaseSensitive(state_root, "alert");
    if (cJSON_IsObject(alert)) {
        copy_json_string(alert, "event_id", s_state.alert_event_id, sizeof(s_state.alert_event_id));
        copy_json_string(alert, "type", s_state.alert_type, sizeof(s_state.alert_type));
        copy_json_string(alert, "message", s_state.alert_message, sizeof(s_state.alert_message));
    }
    cJSON_Delete(root);
    return true;
}

static void poll_state(void)
{
    char response[1536] = {0};
    int battery_level = 0;
    if (vibe_board_battery_level(&battery_level) == ESP_OK) {
        s_state.battery = battery_level;
    }
    bool charging = false;
    bool usb_powered = false;
    bool power_read_ok = false;
    if (vibe_board_battery_charging(&charging) == ESP_OK) {
        s_state.battery_charging = charging;
        power_read_ok = true;
    }
    if (vibe_board_usb_powered(&usb_powered) == ESP_OK) {
        s_state.usb_powered = usb_powered;
        power_read_ok = true;
    }
    static bool last_power_logged = false;
    static bool last_charging = false;
    static bool last_usb_powered = false;
    if (power_read_ok &&
        (!last_power_logged ||
         last_charging != s_state.battery_charging ||
         last_usb_powered != s_state.usb_powered)) {
        ESP_LOGI(TAG, "power status battery=%d charging=%d usb=%d",
                 s_state.battery, s_state.battery_charging, s_state.usb_powered);
        last_power_logged = true;
        last_charging = s_state.battery_charging;
        last_usb_powered = s_state.usb_powered;
    }
    esp_err_t err = bridge_request("GET", VIBE_STICK_STATE_PATH, NULL, response, sizeof(response));
    if (err != ESP_OK || response[0] == '\0' || !parse_state_json(response)) {
        size_t network_count = vibe_wifi_profile_count();
        if (!vibe_usb_ready() &&
            s_wifi_connected &&
            network_count > 1 &&
            !s_long_press_active &&
            !vibe_audio_is_recording() &&
            !s_recording_confirmation_pending) {
            s_bridge_failure_count++;
            if (s_bridge_failure_count >= BRIDGE_FAILURES_BEFORE_WIFI_ROTATE) {
                s_bridge_failure_count = 0;
                s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
                ESP_LOGW(TAG, "bridge unreachable; rotating from Wi-Fi profile %u of %u",
                         (unsigned)(s_wifi_network_index + 1), (unsigned)network_count);
                s_wifi_rotation_requested = true;
                esp_err_t disconnect_err = esp_wifi_disconnect();
                if (disconnect_err != ESP_OK) {
                    s_wifi_rotation_requested = false;
                    ESP_LOGW(TAG, "Wi-Fi profile rotation failed: %s",
                             esp_err_to_name(disconnect_err));
                }
            }
        }
        provider_display_state_t *display_state = current_provider_display_state();
        strlcpy(display_state->status, "OFFLINE", sizeof(display_state->status));
        s_state.wifi = s_wifi_connected;
        render_state();
        return;
    }
    s_bridge_failure_count = 0;
    render_state();
    maybe_handle_alert();
}

static void post_simple_event(const char *event_name, const char *path)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"event\":\"%s\",\"source\":\"sticks3\"}", event_name);
    char response[512] = {0};
    const char *target_path = path ? path : VIBE_STICK_EVENT_PATH;
    esp_err_t err = bridge_request("POST", target_path, body, response, sizeof(response));
    if (err == ESP_OK && response[0] != '\0' && parse_state_json(response)) {
        render_state();
    }
}

static bool parse_recording_session_id(const char *json, char *session_id, size_t session_id_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *sid = cJSON_IsObject(recording) ? cJSON_GetObjectItemCaseSensitive(recording, "session_id") : NULL;
    bool ok = false;
    if (cJSON_IsString(sid) && sid->valuestring && sid->valuestring[0] != '\0') {
        strlcpy(session_id, sid->valuestring, session_id_len);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool is_recording_failure_status(const char *status)
{
    return strcmp(status, "transcription_failed") == 0 ||
           strcmp(status, "transcript_rejected") == 0 ||
           strcmp(status, "paste_failed") == 0 ||
           strcmp(status, "audio_failed") == 0 ||
           strcmp(status, "audio_skipped") == 0 ||
           strcmp(status, "start_failed") == 0 ||
           strcmp(status, "stop_failed") == 0;
}

static bool parse_recording_status(const char *json, char *status_text, size_t status_text_len)
{
    if (status_text_len > 0) {
        status_text[0] = '\0';
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *status = cJSON_IsObject(recording) ?
        cJSON_GetObjectItemCaseSensitive(recording, "status") : NULL;
    bool ok = false;
    if (cJSON_IsString(status) && status->valuestring) {
        strlcpy(status_text, status->valuestring, status_text_len);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void generate_recording_session_id(char *session_id, size_t session_id_len)
{
    if (session_id_len < 33) {
        if (session_id_len > 0) {
            session_id[0] = '\0';
        }
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        uint32_t value = esp_random();
        session_id[i] = hex[value & 0x0f];
    }
    session_id[32] = '\0';
}

static bool upload_recording_audio(void)
{
    size_t audio_len = 0;
    const uint8_t *audio = vibe_audio_data(&audio_len);
    if (!audio || audio_len == 0 || s_recording_session_id[0] == '\0') {
        ESP_LOGW(TAG, "skip audio upload audio=%p len=%u session=%s",
                 audio, (unsigned)audio_len, s_recording_session_id);
        return false;
    }
    for (int attempt = 1; attempt <= AUDIO_UPLOAD_ATTEMPTS; ++attempt) {
        char response[768] = {0};
        esp_err_t err = bridge_post_audio(audio, audio_len, response, sizeof(response));
        char response_session_id[40] = {0};
        char recording_status[32] = {0};
        bool acknowledged =
            err == ESP_OK &&
            response[0] != '\0' &&
            parse_recording_session_id(
                response,
                response_session_id,
                sizeof(response_session_id)
            ) &&
            strcmp(response_session_id, s_recording_session_id) == 0 &&
            parse_recording_status(response, recording_status, sizeof(recording_status)) &&
            strcmp(recording_status, "audio_ready") == 0;
        if (acknowledged) {
            if (parse_state_json(response)) {
                render_state();
            }
            ESP_LOGI(TAG, "audio upload confirmed session=%s attempt=%d",
                     s_recording_session_id, attempt);
            return true;
        }
        ESP_LOGW(TAG,
                 "audio upload not confirmed session=%s attempt=%d err=%s status=%s",
                 s_recording_session_id,
                 attempt,
                 esp_err_to_name(err),
                 recording_status);
        if (attempt < AUDIO_UPLOAD_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_UPLOAD_RETRY_DELAY_MS));
        }
    }
    return false;
}

static void handle_recording_start(void)
{
    generate_recording_session_id(s_recording_session_id, sizeof(s_recording_session_id));
    if (s_recording_session_id[0] == '\0') {
        ESP_LOGW(TAG, "recording start failed: no session id");
        return;
    }

    esp_err_t audio_err = vibe_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording start failed: %s", esp_err_to_name(audio_err));
        s_recording_session_id[0] = '\0';
        return;
    }
    s_recording_started_ms = esp_timer_get_time() / 1000;
    show_recording_overlay("LISTENING", "Release to send", true);

    char body[192];
    snprintf(body, sizeof(body),
             "{\"event\":\"button_long_start\",\"source\":\"sticks3\","
             "\"audio_source\":\"sticks3_pcm\",\"session_id\":\"%s\"}",
             s_recording_session_id);
    char response[1024] = {0};
    esp_err_t err = bridge_request("POST", VIBE_STICK_RECORDING_START_PATH, body, response, sizeof(response));
    char response_session_id[40] = {0};
    char recording_status[32] = {0};
    bool start_acknowledged =
        err == ESP_OK &&
        response[0] != '\0' &&
        parse_recording_session_id(response, response_session_id, sizeof(response_session_id)) &&
        strcmp(response_session_id, s_recording_session_id) == 0 &&
        parse_recording_status(response, recording_status, sizeof(recording_status)) &&
        strcmp(recording_status, "recording") == 0;
    if (start_acknowledged) {
        if (parse_state_json(response)) {
            render_state();
        }
    } else {
        ESP_LOGW(TAG, "recording start not confirmed err=%s status=%s",
                 esp_err_to_name(err), recording_status);
        (void)vibe_audio_stop();
        vibe_audio_clear();
        s_recording_started_ms = 0;
        s_recording_session_id[0] = '\0';
        s_long_press_active = false;
        show_recording_overlay("START FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(900));
        show_recording_overlay(NULL, NULL, false);
    }
}

static void handle_recording_stop(void)
{
    s_recording_started_ms = 0;
    show_recording_overlay("UPLOADING", "", true);
    if (s_recording_session_id[0] == '\0') {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        poll_state();
        show_recording_overlay(NULL, NULL, false);
        return;
    }

    esp_err_t audio_err = vibe_audio_stop();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording stop failed: %s", esp_err_to_name(audio_err));
    }

    bool upload_confirmed = upload_recording_audio();
    if (!upload_confirmed) {
        ESP_LOGW(TAG, "recording stop suppressed because upload was not confirmed");
        vibe_audio_clear();
        s_recording_session_id[0] = '\0';
        show_recording_overlay("UPLOAD FAILED", "Try again", true);
        vTaskDelay(pdMS_TO_TICKS(1200));
        poll_state();
        show_recording_overlay(NULL, NULL, false);
        return;
    }
    vibe_audio_clear();

    show_recording_overlay("TRANSCRIBING", "", true);
    char body[192];
    snprintf(body, sizeof(body),
             "{\"event\":\"button_long_stop\",\"source\":\"sticks3\","
             "\"paste\":true,\"session_id\":\"%s\"}",
             s_recording_session_id);
    char response[1024] = {0};
    esp_err_t err = bridge_request_timeout("POST", VIBE_STICK_RECORDING_STOP_PATH "?compact=1",
                                           body, response, sizeof(response), RECORDING_STOP_TIMEOUT_MS);
    bool recording_failed = false;
    char recording_status[32] = {0};
    if (err == ESP_OK && response[0] != '\0') {
        if (parse_recording_status(response, recording_status, sizeof(recording_status))) {
            recording_failed = is_recording_failure_status(recording_status);
            if (recording_failed) {
                ESP_LOGW(TAG, "recording failed status=%s", recording_status);
            }
        }
        if (parse_state_json(response)) {
            render_state();
        }
    }
    if (err == ESP_OK && strcmp(recording_status, "pasted") == 0) {
        s_recording_confirmation_pending = true;
        show_recording_overlay("TEXT READY", "1x Run / 2x Clear", true);
    } else if (err != ESP_OK || recording_failed) {
        ESP_LOGW(TAG, "recording stop bridge request failed: %s", esp_err_to_name(err));
        const char *title = (strcmp(recording_status, "audio_skipped") == 0 ||
                             strcmp(recording_status, "transcript_rejected") == 0)
            ? "NO SPEECH" : "ASR FAILED";
        show_recording_overlay(title, "", true);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    s_recording_session_id[0] = '\0';
    poll_state();
    if (!s_recording_confirmation_pending) {
        show_recording_overlay(NULL, NULL, false);
    }
}

static void handle_recording_confirmation(bool confirm)
{
    if (!s_recording_confirmation_pending) {
        return;
    }
    const char *path = confirm
        ? VIBE_STICK_RECORDING_CONFIRM_PATH "?compact=1"
        : VIBE_STICK_RECORDING_CANCEL_PATH "?compact=1";
    show_recording_overlay(confirm ? "RUNNING" : "CLEARING", "", true);
    char response[1024] = {0};
    esp_err_t err = bridge_request("POST", path, "{}", response, sizeof(response));
    char recording_status[32] = {0};
    bool status_ok = err == ESP_OK &&
        parse_recording_status(response, recording_status, sizeof(recording_status));
    bool action_ok = status_ok &&
        ((confirm && strcmp(recording_status, "submitted") == 0) ||
         (!confirm && strcmp(recording_status, "cleared") == 0));

    s_recording_confirmation_pending = false;
    if (action_ok) {
        show_recording_overlay(confirm ? "SUBMITTED" : "CLEARED", "", true);
    } else {
        ESP_LOGW(TAG, "recording %s failed err=%s status=%s",
                 confirm ? "confirm" : "cancel", esp_err_to_name(err), recording_status);
        show_recording_overlay(confirm ? "RUN FAILED" : "CLEAR FAILED", "", true);
    }
    vTaskDelay(pdMS_TO_TICKS(action_ok ? 500 : 900));
    poll_state();
    show_recording_overlay(NULL, NULL, false);
}

static esp_err_t configure_wifi_profile(size_t index)
{
    vibe_wifi_profile_t profile = {0};
    ESP_RETURN_ON_FALSE(vibe_wifi_profile_copy(index, &profile),
                        ESP_ERR_NOT_FOUND, TAG, "Wi-Fi profile missing");
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, profile.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, profile.password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode =
        profile.password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static bool wifi_reason_requires_rotation(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        size_t network_count = vibe_wifi_profile_count();
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconnected ? disconnected->reason : WIFI_REASON_UNSPECIFIED;
        bool rotate = s_wifi_rotation_requested;
        s_wifi_rotation_requested = false;
        if (!rotate) {
            s_wifi_profile_failure_count++;
            rotate = wifi_reason_requires_rotation(reason) ||
                     s_wifi_profile_failure_count >= WIFI_CONNECT_FAILURES_BEFORE_ROTATE;
        }
        if (rotate && network_count > 1) {
            s_wifi_network_index = (s_wifi_network_index + 1) % network_count;
            s_wifi_profile_failure_count = 0;
            esp_err_t config_err = configure_wifi_profile(s_wifi_network_index);
            if (config_err != ESP_OK) {
                ESP_LOGW(TAG, "switch Wi-Fi profile failed: %s", esp_err_to_name(config_err));
            } else {
                ESP_LOGI(TAG, "trying Wi-Fi profile %u of %u",
                         (unsigned)(s_wifi_network_index + 1), (unsigned)network_count);
            }
        } else {
            ESP_LOGI(TAG, "retrying Wi-Fi profile %u of %u reason=%u failure=%u",
                     (unsigned)(s_wifi_network_index + 1),
                     (unsigned)network_count,
                     (unsigned)reason,
                     s_wifi_profile_failure_count);
        }
        esp_wifi_connect();
        render_state();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        s_wifi_profile_failure_count = 0;
        s_wifi_rotation_requested = false;
        s_bridge_failure_count = 0;
        s_bridge_discovery_required = VIBE_STICK_BRIDGE_DISCOVERY;
        render_state();
        queue_event(VIBE_STICK_EVENT_POLL_STATE);
    }
}

static esp_err_t init_wifi(void)
{
    size_t network_count = vibe_wifi_profile_count();
    if (network_count == 0) {
        ESP_LOGW(TAG, "no Wi-Fi profiles configured; Wi-Fi disabled");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(configure_wifi_profile(0), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_LOGI(TAG, "configured %u Wi-Fi profile(s)", (unsigned)network_count);
    return ESP_OK;
}

static esp_err_t bootstrap_wifi_profiles_to_usb(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *profiles = cJSON_CreateArray();
    if (!root || !profiles) {
        cJSON_Delete(root);
        cJSON_Delete(profiles);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "profiles", profiles);
    size_t count = vibe_wifi_profile_count();
    for (size_t i = 0; i < count; ++i) {
        vibe_wifi_profile_t profile = {0};
        if (!vibe_wifi_profile_copy(i, &profile)) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item ||
            !cJSON_AddStringToObject(item, "ssid", profile.ssid) ||
            !cJSON_AddStringToObject(item, "password", profile.password)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(profiles, item);
    }

    char body[1536] = {0};
    bool encoded = cJSON_PrintPreallocated(root, body, sizeof(body), false);
    cJSON_Delete(root);
    if (!encoded) {
        return ESP_ERR_INVALID_SIZE;
    }
    char response[256] = {0};
    return bridge_request_timeout(
        "POST",
        VIBE_STICK_WIFI_BOOTSTRAP_PATH,
        body,
        response,
        sizeof(response),
        2500
    );
}

static void maybe_sync_wifi_profiles(void)
{
    static int64_t last_attempt_ms;
    if (!vibe_usb_ready()) {
        last_attempt_ms = 0;
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_attempt_ms > 0 && now_ms - last_attempt_ms < WIFI_PROFILE_SYNC_MS) {
        return;
    }
    last_attempt_ms = now_ms;

    char response[2048] = {0};
    esp_err_t err = bridge_request_timeout(
        "GET",
        VIBE_STICK_WIFI_PROFILES_PATH,
        NULL,
        response,
        sizeof(response),
        2500
    );
    if (err != ESP_OK || response[0] == '\0') {
        ESP_LOGW(TAG, "USB Wi-Fi profile sync unavailable: %s", esp_err_to_name(err));
        return;
    }
    bool changed = false;
    err = vibe_wifi_profiles_apply_json(response, &changed);
    if (err == ESP_ERR_NOT_FOUND) {
        err = bootstrap_wifi_profiles_to_usb();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "initialized Mac Wi-Fi profile list over USB");
        } else {
            ESP_LOGW(TAG, "USB Wi-Fi profile initialization failed: %s", esp_err_to_name(err));
        }
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB Wi-Fi profile sync rejected: %s", esp_err_to_name(err));
        return;
    }
    if (changed) {
        size_t count = vibe_wifi_profile_count();
        s_wifi_network_index = count > 0 ? s_wifi_network_index % count : 0;
        ESP_LOGI(TAG, "USB Wi-Fi profiles updated; %u profile(s) available", (unsigned)count);
        render_state();
    }
}

static void button_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    mark_user_activity();
    ESP_LOGI(TAG, "front button single click");
    queue_event(VIBE_STICK_EVENT_SHORT_PRESS);
}

static void button_double_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    mark_user_activity();
    ESP_LOGI(TAG, "front button double click");
    queue_event(VIBE_STICK_EVENT_DOUBLE_CLICK);
}

static void side_button_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    mark_user_activity();
    queue_event(VIBE_STICK_EVENT_PROVIDER_NEXT);
}

static void button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    mark_user_activity();
    ESP_LOGI(TAG, "front button long press start");
    s_long_press_active = true;
    queue_event(VIBE_STICK_EVENT_LONG_START);
}

static void button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (s_long_press_active) {
        if (queue_event(VIBE_STICK_EVENT_LONG_STOP)) {
            s_long_press_active = false;
        } else {
            ESP_LOGW(TAG, "button release queue full; GPIO recovery will stop recording");
        }
    }
}

static esp_err_t init_button(void)
{
    button_handle_t button = NULL;
    button_handle_t side_button = NULL;
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = PIN_BUTTON_FRONT,
        .active_level = 0,
        .enable_power_save = false,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &gpio_config, &button), TAG, "button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, button_single_click_cb, NULL),
                        TAG, "button single");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, button_double_click_cb, NULL),
                        TAG, "button double");
    button_event_args_t long_press_args = {
        .long_press = {
            .press_time = 450,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_START, &long_press_args, button_long_start_cb, NULL),
                        TAG, "button long");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, button_up_cb, NULL),
                        TAG, "button up");

    const button_gpio_config_t side_gpio_config = {
        .gpio_num = PIN_BUTTON_SIDE,
        .active_level = 0,
        .enable_power_save = false,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &side_gpio_config, &side_button), TAG, "side button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(side_button, BUTTON_SINGLE_CLICK, NULL,
                                               side_button_single_click_cb, NULL),
                        TAG, "side button single");
    return ESP_OK;
}

static void recover_missed_button_release(void)
{
    static int64_t release_detected_ms;
    if (!s_long_press_active || !vibe_audio_is_recording()) {
        release_detected_ms = 0;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool physically_released = gpio_get_level(PIN_BUTTON_FRONT) != 0;
    if (physically_released) {
        if (release_detected_ms == 0) {
            release_detected_ms = now_ms;
        } else if (now_ms - release_detected_ms >= RECORDING_RELEASE_DEBOUNCE_MS) {
            ESP_LOGW(TAG, "button release callback missed; stopping from GPIO level");
            s_long_press_active = false;
            release_detected_ms = 0;
            handle_recording_stop();
            return;
        }
    } else {
        release_detected_ms = 0;
    }

    if (s_recording_started_ms > 0 &&
        now_ms - s_recording_started_ms >= RECORDING_HARD_LIMIT_MS) {
        ESP_LOGW(TAG, "recording hard limit reached; forcing stop");
        s_long_press_active = false;
        release_detected_ms = 0;
        handle_recording_stop();
    }
}

static void app_task(void *arg)
{
    (void)arg;
    agent_event_t event;
    while (true) {
        recover_missed_button_release();
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case VIBE_STICK_EVENT_POLL_STATE:
            poll_state();
            break;
        case VIBE_STICK_EVENT_SHORT_PRESS:
            if (s_recording_confirmation_pending) {
                handle_recording_confirmation(true);
            } else {
                post_simple_event("button_short", NULL);
            }
            break;
        case VIBE_STICK_EVENT_DOUBLE_CLICK:
            if (s_recording_confirmation_pending) {
                handle_recording_confirmation(false);
            } else {
                post_simple_event("button_double", VIBE_STICK_QUOTA_REFRESH_PATH);
                poll_state();
            }
            break;
        case VIBE_STICK_EVENT_LONG_START:
            if (!s_recording_confirmation_pending) {
                handle_recording_start();
            }
            break;
        case VIBE_STICK_EVENT_LONG_STOP:
            if (!s_recording_confirmation_pending) {
                handle_recording_stop();
            }
            break;
        case VIBE_STICK_EVENT_PROVIDER_NEXT:
            switch_provider();
            break;
        }
    }
}

static void network_task(void *arg)
{
    (void)arg;
    int64_t last_poll = 0;
    while (true) {
        maybe_sync_wifi_profiles();
        maybe_dim_display();
        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((s_wifi_connected || vibe_usb_ready()) &&
            now_ms - last_poll >= VIBE_STICK_STATE_POLL_MS) {
            last_poll = now_ms;
            poll_state();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot %s version=%s build=%s %s transport=%s",
             FIRMWARE_NAME, FIRMWARE_VERSION, __DATE__, __TIME__, TRANSPORT);
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_init_power());
    ESP_ERROR_CHECK(vibe_wifi_profiles_init());
    s_event_queue = xQueueCreate(10, sizeof(agent_event_t));
    s_lvgl_lock = xSemaphoreCreateMutex();
    s_bridge_request_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_event_queue && s_lvgl_lock && s_bridge_request_lock
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(init_display());
    lvgl_lock();
    create_ui();
    lvgl_unlock();
    render_state();
    ESP_ERROR_CHECK(init_button());
    ESP_ERROR_CHECK(vibe_audio_init());
    ESP_ERROR_CHECK(vibe_usb_init());
    ESP_ERROR_CHECK(init_wifi());
    xTaskCreate(app_task, "agent_app", 6144, NULL, 4, NULL);
    xTaskCreate(network_task, "agent_network", 6144, NULL, 3, NULL);
}
