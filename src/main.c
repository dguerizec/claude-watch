#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static const char *TAG = "claude-monitor";

/* Seeed XIAO ESP32-S3 Round Display pinout
 * SPI bus shared between LCD and SD card
 * I2C bus shared between CHSC6X touch (0x2E) and PCF8563 RTC (0x51)
 */
#define PIN_LCD_SCLK    GPIO_NUM_7   /* D8 */
#define PIN_LCD_MOSI    GPIO_NUM_9   /* D10 */
#define PIN_LCD_CS      GPIO_NUM_2   /* D1 */
#define PIN_LCD_DC      GPIO_NUM_4   /* D3 */
#define PIN_LCD_BL      GPIO_NUM_43  /* D6 */

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

static esp_lcd_panel_handle_t s_panel = NULL;

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

/* SNTP time sync — called once when WiFi connects */
static void init_time_sync(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    /* Apply timezone from NVS, default to Europe/Paris */
    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);
    const char *tz = strlen(creds.timezone) > 0 ? creds.timezone : "CET-1CEST,M3.5.0,M10.5.0/3";
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone: %s", tz);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_interval(15 * 60 * 1000);  /* resync every 15 min */
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized — resync every 15 min");
}

/* WiFi state change callback — updates the display */
static void wifi_state_cb(wifi_mgr_state_t state, void *arg)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)arg;

    switch (state) {
    case WIFI_MGR_STATE_PROVISIONING: {
        /* QR code encodes WiFi credentials: phone auto-connects, then captive portal opens */
        char qr_text[80];
        snprintf(qr_text, sizeof(qr_text), "WIFI:T:nopass;S:%s;;", wifi_mgr_get_ap_ssid());
        fill_screen(panel, COLOR_DKBLUE);
        display_text_draw_string_centered(panel, 8, "Setup WiFi", COLOR_WHITE, COLOR_DKBLUE, 2);
        qr_display_show(panel, qr_text, COLOR_BLACK, COLOR_WHITE);
        display_text_draw_string_centered(panel, 220, wifi_mgr_get_ap_ssid(), COLOR_WHITE, COLOR_DKBLUE, 1);
        break;
    }

    case WIFI_MGR_STATE_CONNECTING:
        fill_screen(panel, COLOR_BLACK);
        display_text_draw_string_centered(panel, 108, "Connecting...", COLOR_WHITE, COLOR_BLACK, 2);
        break;

    case WIFI_MGR_STATE_CONNECTED: {
        init_time_sync();
        char url[40];
        snprintf(url, sizeof(url), "http://%s", wifi_mgr_get_sta_ip());
        fill_screen(panel, COLOR_DKGREEN);
        display_text_draw_string_centered(panel, 8, "Connected!", COLOR_WHITE, COLOR_DKGREEN, 2);
        qr_display_show(panel, url, COLOR_BLACK, COLOR_WHITE);
        display_text_draw_string_centered(panel, 220, wifi_mgr_get_sta_ip(), COLOR_WHITE, COLOR_DKGREEN, 1);
        break;
    }

    case WIFI_MGR_STATE_FAILED:
        fill_screen(panel, COLOR_DKRED);
        display_text_draw_string_centered(panel, 100, "WiFi Failed", COLOR_WHITE, COLOR_DKRED, 2);
        display_text_draw_string_centered(panel, 130, "Hold to reset", COLOR_WHITE, COLOR_DKRED, 2);
        break;

    default:
        break;
    }
}

/* Display usage data on screen */
static void display_usage(api_usage_t *usage)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 30, "Claude Usage", COLOR_WHITE, COLOR_BLACK, 2);

    /* 5-hour usage */
    char line[48];
    snprintf(line, sizeof(line), "5h: %.0f%%", usage->five_hour.utilization);
    display_text_draw_string_centered(s_panel, 75, line, COLOR_GREEN, COLOR_BLACK, 3);
    snprintf(line, sizeof(line), "reset %s", usage->five_hour.resets_at);
    display_text_draw_string_centered(s_panel, 100, line, COLOR_WHITE, COLOR_BLACK, 1);

    /* 7-day usage */
    snprintf(line, sizeof(line), "7d: %.0f%%", usage->seven_day.utilization);
    display_text_draw_string_centered(s_panel, 135, line, COLOR_GREEN, COLOR_BLACK, 3);
    snprintf(line, sizeof(line), "reset %s", usage->seven_day.resets_at);
    display_text_draw_string_centered(s_panel, 160, line, COLOR_WHITE, COLOR_BLACK, 1);

    /* Tap hint */
    display_text_draw_string_centered(s_panel, 210, "tap to refresh", COLOR_DKGREEN, COLOR_BLACK, 1);
}

static void display_usage_error(const char *error)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 80, "Error", COLOR_RED, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 110, error, COLOR_WHITE, COLOR_BLACK, 1);
    display_text_draw_string_centered(s_panel, 140, "tap to retry", COLOR_WHITE, COLOR_BLACK, 1);
}

static void display_no_token(void)
{
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 60, "No Token", COLOR_RED, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 100, "Set credentials at:", COLOR_WHITE, COLOR_BLACK, 1);
    char url[40];
    snprintf(url, sizeof(url), "http://%s", wifi_mgr_get_sta_ip());
    display_text_draw_string_centered(s_panel, 120, url, COLOR_GREEN, COLOR_BLACK, 1);
    qr_display_show(s_panel, url, COLOR_WHITE, COLOR_BLACK);
}

static TaskHandle_t s_fetch_task = NULL;

static void fetch_usage_task(void *arg)
{
    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);

    if (strlen(creds.refresh_token) == 0) {
        display_no_token();
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Show loading */
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 108, "Loading...", COLOR_WHITE, COLOR_BLACK, 2);

    api_usage_t usage;
    esp_err_t err = api_client_get_usage(creds.access_token, creds.refresh_token, &usage);

    if (err == ESP_OK && usage.valid) {
        display_usage(&usage);
    } else {
        display_usage_error(usage.error);
    }

    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static void fetch_and_display_usage(void)
{
    /* Prevent concurrent fetches */
    if (s_fetch_task != NULL) return;
    /* TLS needs ~16KB stack */
    xTaskCreate(fetch_usage_task, "fetch_usage", 16384, NULL, 5, &s_fetch_task);
}

/* Check for touch-hold reset gesture (3 seconds) */
static bool check_reset_gesture(chsc6x_handle_t touch)
{
    if (!touch) return false;

    ESP_LOGI(TAG, "Hold touch 3s to reset WiFi...");
    display_text_draw_string_centered(s_panel, 130, "Hold 3s: reset", COLOR_WHITE, COLOR_BLACK, 1);

    int held_count = 0;
    for (int i = 0; i < 30; i++) {  /* 30 × 100ms = 3s */
        chsc6x_touch_data_t td;
        if (chsc6x_read(touch, &td) == ESP_OK && td.touched) {
            held_count++;
        } else {
            held_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return held_count >= 25;  /* ~2.5s continuous hold */
}

void app_main(void)
{
    ESP_LOGI(TAG, "Claude Monitor starting...");

    /* Backlight ON */
    lcd_backlight_init();

    /* SPI bus */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

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

    /* Boot splash */
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 100, "Claude Monitor", COLOR_WHITE, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 120, "Starting...", COLOR_GREEN, COLOR_BLACK, 2);

    /* Check for WiFi reset gesture */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    if (touch_ok && check_reset_gesture(touch)) {
        fill_screen(s_panel, COLOR_BLACK);
        display_text_draw_string_centered(s_panel, 108, "WiFi Reset!", COLOR_RED, COLOR_BLACK, 2);
        /* Init NVS first so we can erase */
        wifi_mgr_init(wifi_state_cb, s_panel);
        wifi_mgr_erase_credentials();
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        /* Normal init */
        wifi_mgr_init(wifi_state_cb, s_panel);
    }

    /* Start WiFi (provisioning or station) */
    wifi_mgr_start();

    /* Main loop */
    while (1) {
        if (touch_ok) {
            chsc6x_touch_data_t td;
            if (chsc6x_read(touch, &td) == ESP_OK && td.touched) {
                wifi_mgr_state_t st = wifi_mgr_get_state();
                if (st == WIFI_MGR_STATE_CONNECTED) {
                    ESP_LOGI(TAG, "Touch — fetching usage");
                    fetch_and_display_usage();
                    /* Debounce: wait for touch release */
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
