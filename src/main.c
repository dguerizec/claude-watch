#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "driver/i2c_master.h"
#include "chsc6x.h"
#include "display_text.h"
#include "qr_display.h"
#include "wifi_manager.h"
#include "api_client.h"
#include "usage_store.h"

static const char *TAG = "claude-monitor";

/* Seeed XIAO ESP32-S3 Round Display pinout
 * SPI bus shared between LCD and SD card
 * I2C bus shared between CHSC6X touch (0x2E) and PCF8563 RTC (0x51)
 */
#define PIN_SPI_SCLK    GPIO_NUM_7   /* D8 — shared LCD + SD */
#define PIN_SPI_MOSI    GPIO_NUM_9   /* D10 — shared LCD + SD */
#define PIN_SPI_MISO    GPIO_NUM_8   /* D9 — SD card read */
#define PIN_LCD_CS      GPIO_NUM_2   /* D1 */
#define PIN_LCD_DC      GPIO_NUM_4   /* D3 */
#define PIN_LCD_BL      GPIO_NUM_43  /* D6 */
#define PIN_SD_CS       GPIO_NUM_3   /* D2 */

#define PIN_TOUCH_SDA   GPIO_NUM_5   /* D4 */
#define PIN_TOUCH_SCL   GPIO_NUM_6   /* D5 */
#define PIN_TOUCH_INT   GPIO_NUM_44  /* D7 — active LOW when touched */

#define LCD_H_RES       240
#define LCD_V_RES       240
#define LCD_PIXEL_CLK   (40 * 1000 * 1000)

/* Colors (RGB565) */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_DKBLUE  0x000A
#define COLOR_DKGREEN 0x0320
#define COLOR_DKRED   0x8000

/* ─── Display message queue ─────────────────────────────────────────────
 * All LCD and SD card operations go through display_task via this queue.
 * This avoids concurrent SPI access from multiple FreeRTOS tasks. */

typedef enum {
    DISP_MSG_WIFI_STATE,
    DISP_MSG_LOADING,
    DISP_MSG_USAGE,
    DISP_MSG_ERROR,
    DISP_MSG_NO_TOKEN,
} disp_msg_type_t;

typedef struct {
    disp_msg_type_t type;
    union {
        wifi_mgr_state_t wifi_state;
        api_usage_t usage;
    };
} disp_msg_t;

static QueueHandle_t s_display_queue = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

/* ─── LCD helpers ───────────────────────────────────────────────────── */

static void lcd_backlight_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_LCD_BL, 1);
}

static uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

static void fill_screen(esp_lcd_panel_handle_t panel, uint16_t color)
{
    uint16_t *line_buf = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(line_buf);
    uint16_t c = swap16(color);
    for (int i = 0; i < LCD_H_RES; i++) {
        line_buf[i] = c;
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y + 1, line_buf);
    }
    free(line_buf);
}

/* ─── SNTP time sync ────────────────────────────────────────────────── */

static void init_time_sync(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);
    const char *tz = strlen(creds.timezone) > 0 ? creds.timezone : "CET-1CEST,M3.5.0,M10.5.0/3";
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone: %s", tz);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_interval(15 * 60 * 1000);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized — resync every 15 min");
}

/* ─── Fetch task (HTTP only — no LCD access) ────────────────────────── */

static TaskHandle_t s_fetch_task = NULL;

static void fetch_usage_task(void *arg)
{
    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);

    if (strlen(creds.refresh_token) == 0) {
        disp_msg_t msg = { .type = DISP_MSG_NO_TOKEN };
        xQueueSend(s_display_queue, &msg, portMAX_DELAY);
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Signal loading */
    disp_msg_t msg = { .type = DISP_MSG_LOADING };
    xQueueSend(s_display_queue, &msg, portMAX_DELAY);

    /* HTTP request (needs 16KB stack for TLS) */
    api_usage_t usage;
    esp_err_t err = api_client_get_usage(creds.access_token, creds.refresh_token, &usage);

    if (err == ESP_OK && usage.valid) {
        msg.type = DISP_MSG_USAGE;
    } else {
        msg.type = DISP_MSG_ERROR;
    }
    msg.usage = usage;
    xQueueSend(s_display_queue, &msg, portMAX_DELAY);

    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static void fetch_and_display_usage(void)
{
    if (s_fetch_task != NULL) return;
    xTaskCreate(fetch_usage_task, "fetch_usage", 16384, NULL, 5, &s_fetch_task);
}

/* ─── Auto-fetch timer ──────────────────────────────────────────────── */

static TimerHandle_t s_auto_fetch_timer = NULL;

static void auto_fetch_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Auto-fetch timer fired");
    fetch_and_display_usage();
}

static void start_auto_fetch(void)
{
    if (s_auto_fetch_timer != NULL) return;

    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);
    int interval_min = atoi(creds.fetch_interval);
    if (interval_min <= 0) interval_min = 10;

    fetch_and_display_usage();

    s_auto_fetch_timer = xTimerCreate("auto_fetch",
        pdMS_TO_TICKS(interval_min * 60 * 1000),
        pdTRUE, NULL, auto_fetch_timer_cb);
    xTimerStart(s_auto_fetch_timer, 0);
    ESP_LOGI(TAG, "Auto-fetch started: every %d min", interval_min);
}

/* ─── Display screens (called only from display_task) ───────────────── */

static void draw_provisioning_screen(void)
{
    char qr_text[80];
    snprintf(qr_text, sizeof(qr_text), "WIFI:T:nopass;S:%s;;", wifi_mgr_get_ap_ssid());
    fill_screen(s_panel, COLOR_DKBLUE);
    display_text_draw_string_centered(s_panel, 8, "Setup WiFi", COLOR_WHITE, COLOR_DKBLUE, 2);
    qr_display_show(s_panel, qr_text, COLOR_BLACK, COLOR_WHITE);
    display_text_draw_string_centered(s_panel, 220, wifi_mgr_get_ap_ssid(), COLOR_WHITE, COLOR_DKBLUE, 1);
}

static void draw_connecting_screen(void)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 108, "Connecting...", COLOR_WHITE, COLOR_BLACK, 2);
}

static void draw_connected_screen(void)
{
    char url[40];
    snprintf(url, sizeof(url), "http://%s", wifi_mgr_get_sta_ip());
    fill_screen(s_panel, COLOR_DKGREEN);
    display_text_draw_string_centered(s_panel, 8, "Connected!", COLOR_WHITE, COLOR_DKGREEN, 2);
    qr_display_show(s_panel, url, COLOR_BLACK, COLOR_WHITE);
    display_text_draw_string_centered(s_panel, 220, wifi_mgr_get_sta_ip(), COLOR_WHITE, COLOR_DKGREEN, 1);
}

static void draw_failed_screen(void)
{
    fill_screen(s_panel, COLOR_DKRED);
    display_text_draw_string_centered(s_panel, 100, "WiFi Failed", COLOR_WHITE, COLOR_DKRED, 2);
    display_text_draw_string_centered(s_panel, 130, "Hold to reset", COLOR_WHITE, COLOR_DKRED, 2);
}

static void draw_loading_screen(void)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 108, "Loading...", COLOR_WHITE, COLOR_BLACK, 2);
}

static void draw_usage_screen(api_usage_t *usage)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 30, "Claude Usage", COLOR_WHITE, COLOR_BLACK, 2);

    char line[48];
    snprintf(line, sizeof(line), "5h: %.0f%%", usage->five_hour.utilization);
    display_text_draw_string_centered(s_panel, 75, line, COLOR_GREEN, COLOR_BLACK, 3);
    snprintf(line, sizeof(line), "reset %s", usage->five_hour.resets_at);
    display_text_draw_string_centered(s_panel, 100, line, COLOR_WHITE, COLOR_BLACK, 1);

    snprintf(line, sizeof(line), "7d: %.0f%%", usage->seven_day.utilization);
    display_text_draw_string_centered(s_panel, 135, line, COLOR_GREEN, COLOR_BLACK, 3);
    snprintf(line, sizeof(line), "reset %s", usage->seven_day.resets_at);
    display_text_draw_string_centered(s_panel, 160, line, COLOR_WHITE, COLOR_BLACK, 1);

    display_text_draw_string_centered(s_panel, 210, "tap to refresh", COLOR_DKGREEN, COLOR_BLACK, 1);
}

static void draw_error_screen(const char *error)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 80, "Error", COLOR_RED, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 110, error, COLOR_WHITE, COLOR_BLACK, 1);
    display_text_draw_string_centered(s_panel, 140, "tap to retry", COLOR_WHITE, COLOR_BLACK, 1);
}

static void draw_no_token_screen(void)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 60, "No Token", COLOR_RED, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 100, "Set credentials at:", COLOR_WHITE, COLOR_BLACK, 1);
    char url[40];
    snprintf(url, sizeof(url), "http://%s", wifi_mgr_get_sta_ip());
    display_text_draw_string_centered(s_panel, 120, url, COLOR_GREEN, COLOR_BLACK, 1);
    qr_display_show(s_panel, url, COLOR_WHITE, COLOR_BLACK);
}

/* ─── Display task — sole owner of SPI bus ──────────────────────────── */

static void display_task(void *arg)
{
    disp_msg_t msg;

    while (1) {
        if (xQueueReceive(s_display_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        switch (msg.type) {
        case DISP_MSG_WIFI_STATE:
            switch (msg.wifi_state) {
            case WIFI_MGR_STATE_PROVISIONING:
                draw_provisioning_screen();
                break;
            case WIFI_MGR_STATE_CONNECTING:
                draw_connecting_screen();
                break;
            case WIFI_MGR_STATE_CONNECTED:
                init_time_sync();
                draw_connected_screen();
                start_auto_fetch();
                break;
            case WIFI_MGR_STATE_FAILED:
                draw_failed_screen();
                break;
            default:
                break;
            }
            break;

        case DISP_MSG_LOADING:
            draw_loading_screen();
            break;

        case DISP_MSG_USAGE:
            /* SD write first (mount/unmount), then LCD draw — sequential, no conflict */
            usage_store_append(msg.usage.five_hour.utilization, msg.usage.seven_day.utilization);
            draw_usage_screen(&msg.usage);
            break;

        case DISP_MSG_ERROR:
            draw_error_screen(msg.usage.error);
            break;

        case DISP_MSG_NO_TOKEN:
            draw_no_token_screen();
            break;
        }
    }
}

/* ─── WiFi callback — just enqueue, no LCD access ───────────────────── */

static void wifi_state_cb(wifi_mgr_state_t state, void *arg)
{
    (void)arg;
    disp_msg_t msg = { .type = DISP_MSG_WIFI_STATE, .wifi_state = state };
    xQueueSend(s_display_queue, &msg, pdMS_TO_TICKS(100));
}

/* ─── Touch-hold reset gesture (3 seconds) ──────────────────────────── */

static bool check_reset_gesture(chsc6x_handle_t touch)
{
    if (!touch) return false;

    ESP_LOGI(TAG, "Hold touch 3s to reset WiFi...");
    display_text_draw_string_centered(s_panel, 130, "Hold 3s: reset", COLOR_WHITE, COLOR_BLACK, 1);

    int held_count = 0;
    for (int i = 0; i < 30; i++) {
        chsc6x_touch_data_t td;
        if (chsc6x_read(touch, &td) == ESP_OK && td.touched) {
            held_count++;
        } else {
            held_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return held_count >= 25;
}

/* ─── Main ──────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Claude Monitor starting...");

    /* Backlight ON */
    lcd_backlight_init();

    /* SPI bus (shared LCD + SD card) */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SPI_SCLK,
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* SD card storage config (lazy mount — actual mount in display_task) */
    usage_store_init(SPI2_HOST, PIN_SD_CS);

    /* LCD panel IO (SPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLK,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

    /* GC9A01 panel driver */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "Display initialized — 240x240 GC9A01");

    /* I2C bus */
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));

    /* CHSC6X touch controller */
    chsc6x_config_t touch_cfg = {
        .i2c_bus = i2c_bus,
        .int_gpio_num = PIN_TOUCH_INT,
    };
    chsc6x_handle_t touch = NULL;
    bool touch_ok = (chsc6x_init(&touch_cfg, &touch) == ESP_OK);

    if (touch_ok) {
        ESP_LOGI(TAG, "Touch ready");
    } else {
        ESP_LOGW(TAG, "Touch init failed — running without touch");
    }

    /* Boot splash — single-threaded here, direct LCD access is safe */
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 100, "Claude Monitor", COLOR_WHITE, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 120, "Starting...", COLOR_GREEN, COLOR_BLACK, 2);

    /* Check for WiFi reset gesture (still single-threaded) */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    if (touch_ok && check_reset_gesture(touch)) {
        fill_screen(s_panel, COLOR_BLACK);
        display_text_draw_string_centered(s_panel, 108, "WiFi Reset!", COLOR_RED, COLOR_BLACK, 2);
        wifi_mgr_init(wifi_state_cb, NULL);
        wifi_mgr_erase_credentials();
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        wifi_mgr_init(wifi_state_cb, NULL);
    }

    /* Create display queue + task BEFORE starting WiFi.
     * From this point on, only display_task touches the LCD/SD. */
    s_display_queue = xQueueCreate(4, sizeof(disp_msg_t));
    xTaskCreate(display_task, "display", 8192, NULL, 5, NULL);

    /* Start WiFi — callbacks will now go through the queue */
    wifi_mgr_start();

    /* Main loop — touch handling only, no direct LCD access */
    while (1) {
        if (touch_ok) {
            chsc6x_touch_data_t td;
            if (chsc6x_read(touch, &td) == ESP_OK && td.touched) {
                wifi_mgr_state_t st = wifi_mgr_get_state();
                if (st == WIFI_MGR_STATE_CONNECTED) {
                    ESP_LOGI(TAG, "Touch — fetching usage");
                    fetch_and_display_usage();
                    vTaskDelay(pdMS_TO_TICKS(500));
                } else if (st == WIFI_MGR_STATE_FAILED) {
                    ESP_LOGI(TAG, "Touch in FAILED state — erasing and rebooting");
                    wifi_mgr_erase_credentials();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
