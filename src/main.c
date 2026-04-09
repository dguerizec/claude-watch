#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
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
#include "polar_graph.h"

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
    DISP_MSG_TOGGLE,
    DISP_MSG_SETTINGS,
} disp_msg_type_t;

typedef struct {
    disp_msg_type_t type;
    union {
        wifi_mgr_state_t wifi_state;
        api_usage_t usage;
        int direction;  /* +1 = next, -1 = prev (for DISP_MSG_TOGGLE) */
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
static int s_fetch_period_s = 600;

static void schedule_next_fetch(void);

static void auto_fetch_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Auto-fetch timer fired");
    fetch_and_display_usage();
    schedule_next_fetch();
}

static void schedule_next_fetch(void)
{
    time_t now = time(NULL);
    /* Align to clock: next_fetch = next multiple of period */
    time_t next = now - (now % s_fetch_period_s) + s_fetch_period_s;
    int delay_s = (int)(next - now);
    if (delay_s < 5) delay_s += s_fetch_period_s;  /* safety margin */

    if (!s_auto_fetch_timer) {
        s_auto_fetch_timer = xTimerCreate("auto_fetch",
            pdMS_TO_TICKS(delay_s * 1000),
            pdFALSE, NULL, auto_fetch_timer_cb);  /* one-shot, re-armed in cb */
    } else {
        xTimerChangePeriod(s_auto_fetch_timer, pdMS_TO_TICKS(delay_s * 1000), 0);
    }
    xTimerStart(s_auto_fetch_timer, 0);
    ESP_LOGI(TAG, "Next fetch in %ds (aligned to %d-min grid)", delay_s, s_fetch_period_s / 60);
}

static void start_auto_fetch(void)
{
    if (s_auto_fetch_timer != NULL) return;

    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);
    int interval_min = atoi(creds.fetch_interval);
    if (interval_min <= 0) interval_min = 10;
    s_fetch_period_s = interval_min * 60;

    time_t now = time(NULL);
    bool time_valid = (now > 1700000000);  /* SNTP synced? */

    if (!time_valid) {
        /* Time not synced yet — fetch immediately, align later */
        fetch_and_display_usage();
        s_auto_fetch_timer = xTimerCreate("auto_fetch",
            pdMS_TO_TICKS(s_fetch_period_s * 1000),
            pdFALSE, NULL, auto_fetch_timer_cb);
        xTimerStart(s_auto_fetch_timer, 0);
        ESP_LOGI(TAG, "Auto-fetch started (time not synced, immediate + %dmin)", interval_min);
    } else {
        /* Time valid — align to grid, skip if recent data exists */
        schedule_next_fetch();
        ESP_LOGI(TAG, "Auto-fetch started: every %d min (aligned)", interval_min);
    }
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

    display_text_draw_string_centered(s_panel, 210, "tap: graph", COLOR_DKGREEN, COLOR_BLACK, 1);
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

/* ─── Graph screen ──────────────────────────────────────────────────── */

#define MAX_GRAPH_POINTS 4096

static void draw_polar_screen(api_usage_t *usage, int period_secs, int num_rotations,
                               int num_ticks, time_t reset_epoch, float current_pct,
                               usage_column_t col)
{
    time_t now = time(NULL);
    /* If no fetch yet, use now as reset reference — graph still shows SD history */
    if (reset_epoch == 0) reset_epoch = now;
    time_t data_start = now - num_rotations * (time_t)period_secs;

    usage_data_point_t *points = malloc(MAX_GRAPH_POINTS * sizeof(usage_data_point_t));
    if (!points) {
        ESP_LOGE(TAG, "Failed to alloc graph data");
        return;
    }

    /* Two-pass read: old data downsampled, recent data full resolution */
    time_t recent_start = now - period_secs;
    int old_budget = MAX_GRAPH_POINTS / 4;
    int n_old = usage_store_read(data_start, recent_start - 1, points, old_budget, 8, col);
    int n_recent = usage_store_read(recent_start, now, points + n_old,
                                    MAX_GRAPH_POINTS - n_old, 1, col);
    int n = n_old + n_recent;
    ESP_LOGI(TAG, "Graph: %d old + %d recent = %d points (%s)",
             n_old, n_recent, n, col == USAGE_COL_FIVE_HOUR ? "5h" : "7d");

    polar_graph_draw(s_panel, points, n, period_secs, num_rotations, num_ticks, reset_epoch, now);
    free(points);

    /* Overlay current value at center */
    char pct[16];
    if (current_pct >= 0)
        snprintf(pct, sizeof(pct), "%.0f%%", current_pct);
    else
        strcpy(pct, "--");
    display_text_draw_string_centered(s_panel, 113, pct, 0xFFFF, 0x0000, 2);
}

static void draw_graph_7d(api_usage_t *usage)
{
    draw_polar_screen(usage, 7 * 24 * 3600, 5, 7,
                      usage->seven_day.resets_at_epoch,
                      usage->seven_day.utilization, USAGE_COL_SEVEN_DAY);
}

static void draw_graph_5h(api_usage_t *usage)
{
    draw_polar_screen(usage, 5 * 3600, 5, 5,
                      usage->five_hour.resets_at_epoch,
                      usage->five_hour.utilization, USAGE_COL_FIVE_HOUR);
}

/* ─── Clock screen ─────────────────────────────────────────────────── */

static bool s_clock_cleared = false;

/* WiFi reset button — 60px circle at bottom of clock screen */
#define BTN_CX   120
#define BTN_CY   210
#define BTN_R    30
#define BTN_COLOR 0x000A   /* dark blue */

static void draw_wifi_button(void)
{
    /* Render button into a single buffer to avoid DMA race conditions.
     * One DMA transaction instead of hundreds of individual pixel writes. */
    int bx = BTN_CX - BTN_R;
    int by = BTN_CY - BTN_R;
    int bw = BTN_R * 2;
    int bh = BTN_R * 2;
    if (by + bh > LCD_V_RES) bh = LCD_V_RES - by;

    uint16_t *buf = heap_caps_malloc(bw * bh * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) return;
    memset(buf, 0, bw * bh * sizeof(uint16_t));

    uint16_t bg = swap16(BTN_COLOR);
    uint16_t fg = swap16(COLOR_WHITE);

    /* Filled circle background */
    for (int ly = 0; ly < bh; ly++) {
        int dy = ly - BTN_R;
        if (abs(dy) > BTN_R) continue;
        int dx = (int)sqrtf((float)(BTN_R * BTN_R - dy * dy));
        int x0 = BTN_R - dx, x1 = BTN_R + dx;
        for (int x = x0; x <= x1 && x < bw; x++)
            if (x >= 0) buf[ly * bw + x] = bg;
    }

    /* WiFi icon — dot 8px below button center */
    int dot_ly = BTN_R + 8;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = BTN_R + dx, y = dot_ly + dy;
            if (x >= 0 && x < bw && y >= 0 && y < bh)
                buf[y * bw + x] = fg;
        }

    /* WiFi arcs — top quadrant only */
    int arc_radii[] = {8, 14, 20};
    for (int a = 0; a < 3; a++) {
        int r = arc_radii[a];
        int x = r, y = 0, d = 1 - r;
        while (x >= y) {
            int px1 = BTN_R + y, px2 = BTN_R - y, py = dot_ly - x;
            if (py >= 0 && py < bh) {
                if (px1 >= 0 && px1 < bw) buf[py * bw + px1] = fg;
                if (px2 >= 0 && px2 < bw) buf[py * bw + px2] = fg;
            }
            y++;
            if (d <= 0) d += 2 * y + 1;
            else { x--; d += 2 * (y - x) + 1; }
        }
    }

    esp_lcd_panel_draw_bitmap(s_panel, bx, by, bx + bw, by + bh, buf);
    vTaskDelay(pdMS_TO_TICKS(2));
    free(buf);
}

static void draw_clock_screen(void)
{
    if (!s_clock_cleared) {
        fill_screen(s_panel, COLOR_BLACK);
        draw_wifi_button();
        s_clock_cleared = true;
    }

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    char date[32], timebuf[32];
    strftime(date, sizeof(date), "%Y-%m-%d", &local);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &local);

    display_text_draw_string_centered(s_panel, 100, date, COLOR_WHITE, COLOR_BLACK, 2);
    display_text_draw_string_centered(s_panel, 125, timebuf, COLOR_WHITE, COLOR_BLACK, 2);
}

static void draw_settings_screen(void)
{
    char url[40];
    snprintf(url, sizeof(url), "http://%s", wifi_mgr_get_sta_ip());
    fill_screen(s_panel, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 8, "Settings", COLOR_WHITE, COLOR_BLACK, 2);
    qr_display_show(s_panel, url, COLOR_WHITE, COLOR_BLACK);
    display_text_draw_string_centered(s_panel, 220, wifi_mgr_get_sta_ip(), COLOR_WHITE, COLOR_BLACK, 1);
}

static bool is_in_wifi_button(uint16_t tx, uint16_t ty)
{
    int dx = (int)tx - BTN_CX;
    int dy = (int)ty - BTN_CY;
    return (dx * dx + dy * dy) <= BTN_R * BTN_R;
}

/* ─── Display task — sole owner of SPI bus ──────────────────────────── */

typedef enum { VIEW_USAGE, VIEW_GRAPH_7D, VIEW_GRAPH_5H, VIEW_CLOCK, VIEW_COUNT } view_t;

/* Configurable screen cycle — loaded from NVS */
static view_t s_view_cycle[VIEW_COUNT] = { VIEW_USAGE, VIEW_GRAPH_7D, VIEW_GRAPH_5H, VIEW_CLOCK };
static int s_view_cycle_len = VIEW_COUNT;
static int s_view_idx = 0;         /* current index in s_view_cycle */
static api_usage_t s_last_usage = {0};
static bool s_has_usage = false;
static bool s_settings_overlay = false;

#define s_current_view s_view_cycle[s_view_idx]

static void load_display_config(void)
{
    char cfg[64];
    wifi_mgr_get_display_config(cfg, sizeof(cfg));
    s_view_cycle_len = 0;
    char *saveptr, *tok;
    for (tok = strtok_r(cfg, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        int id = 0, en = 1;
        sscanf(tok, "%d:%d", &id, &en);
        if (en && id >= 0 && id < VIEW_COUNT && s_view_cycle_len < VIEW_COUNT)
            s_view_cycle[s_view_cycle_len++] = (view_t)id;
    }
    if (s_view_cycle_len == 0) {
        s_view_cycle[0] = VIEW_USAGE;
        s_view_cycle_len = 1;
    }
    /* Clamp index if cycle shrank, but don't reset to 0 */
    if (s_view_idx >= s_view_cycle_len)
        s_view_idx = 0;
}

static void display_task(void *arg)
{
    disp_msg_t msg;

    while (1) {
        bool clock_active = (s_current_view == VIEW_CLOCK && !s_settings_overlay
                             && wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED
                             && s_has_usage);
        TickType_t wait = clock_active ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
        if (xQueueReceive(s_display_queue, &msg, wait) != pdTRUE) {
            if (clock_active)
                draw_clock_screen();
            continue;
        }

        switch (msg.type) {
        case DISP_MSG_WIFI_STATE:
            switch (msg.wifi_state) {
            case WIFI_MGR_STATE_PROVISIONING:
                draw_provisioning_screen();
                break;
            case WIFI_MGR_STATE_CONNECTING: {
                wifi_mgr_credentials_t cr;
                wifi_mgr_get_credentials(&cr);
                /* Only show "Connecting..." on first setup */
                if (!s_has_usage && strlen(cr.refresh_token) == 0)
                    draw_connecting_screen();
                break;
            }
            case WIFI_MGR_STATE_CONNECTED: {
                init_time_sync();
                wifi_mgr_credentials_t creds;
                wifi_mgr_get_credentials(&creds);
                if (!s_has_usage && strlen(creds.refresh_token) == 0) {
                    /* First setup — show settings QR */
                    draw_connected_screen();
                } else if (!s_has_usage) {
                    /* Normal reboot — draw initial view immediately */
                    s_clock_cleared = false;
                    switch (s_current_view) {
                    case VIEW_CLOCK:    draw_clock_screen(); break;
                    case VIEW_USAGE:    draw_loading_screen(); break;
                    case VIEW_GRAPH_7D: draw_loading_screen(); break;
                    case VIEW_GRAPH_5H: draw_loading_screen(); break;
                    default: break;
                    }
                }
                start_auto_fetch();
            }
                break;
            case WIFI_MGR_STATE_FAILED:
                /* Only show failure if we have nothing else to display */
                if (!s_has_usage)
                    draw_failed_screen();
                break;
            default:
                break;
            }
            break;

        case DISP_MSG_LOADING:
            /* Silent — initial view already drawn by CONNECTED handler */
            break;

        case DISP_MSG_USAGE: {
            bool first = !s_has_usage;
            s_last_usage = msg.usage;
            s_has_usage = true;
            usage_store_append(msg.usage.five_hour.utilization, msg.usage.seven_day.utilization);
            if (s_current_view == VIEW_GRAPH_7D)
                draw_graph_7d(&msg.usage);
            else if (s_current_view == VIEW_GRAPH_5H)
                draw_graph_5h(&msg.usage);
            else if (s_current_view == VIEW_USAGE)
                draw_usage_screen(&msg.usage);
            else if (s_current_view == VIEW_CLOCK && first)
                draw_clock_screen();  /* first data arrived — draw clock over boot splash */
            break;
        }

        case DISP_MSG_ERROR:
            /* Don't overwrite display for a background fetch error */
            if (!s_has_usage)
                draw_error_screen(msg.usage.error);
            else
                ESP_LOGW(TAG, "Fetch error (display unchanged): %s", msg.usage.error);
            break;

        case DISP_MSG_NO_TOKEN:
            draw_no_token_screen();
            break;

        case DISP_MSG_TOGGLE:
            s_settings_overlay = false;
            load_display_config();  /* pick up any web UI changes */
            if (msg.direction < 0)
                s_view_idx = (s_view_idx - 1 + s_view_cycle_len) % s_view_cycle_len;
            else
                s_view_idx = (s_view_idx + 1) % s_view_cycle_len;
            s_clock_cleared = false;
            switch (s_current_view) {
            case VIEW_USAGE:    draw_usage_screen(&s_last_usage); break;
            case VIEW_GRAPH_7D: draw_graph_7d(&s_last_usage); break;
            case VIEW_GRAPH_5H: draw_graph_5h(&s_last_usage); break;
            case VIEW_CLOCK:    draw_clock_screen(); break;
            default: break;
            }
            break;

        case DISP_MSG_SETTINGS:
            s_settings_overlay = true;
            draw_settings_screen();
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

    /* Load display config from NVS */
    load_display_config();

    /* Create display queue + task BEFORE starting WiFi.
     * From this point on, only display_task touches the LCD/SD. */
    s_display_queue = xQueueCreate(4, sizeof(disp_msg_t));
    xTaskCreate(display_task, "display", 12288, NULL, 5, NULL);

    /* Start WiFi — callbacks will now go through the queue */
    wifi_mgr_start();

    /* Main loop — gesture detection, no direct LCD access.
     * Swipe left/right = next/prev screen, tap = WiFi button action. */
    bool touching = false;
    uint16_t t_sx = 0, t_sy = 0, t_lx = 0, t_ly = 0;
    int t_frames = 0;

    #define SWIPE_MIN    40   /* min pixels for a swipe */
    #define TAP_MAX      15   /* max pixels for a tap */
    #define LONG_PRESS   30   /* frames (× 100ms = 3s) */

    while (1) {
        if (touch_ok) {
            chsc6x_touch_data_t td;
            if (chsc6x_read(touch, &td) == ESP_OK) {
                if (td.touched) {
                    if (!touching) {
                        t_sx = td.x; t_sy = td.y;
                        t_frames = 0;
                        touching = true;
                    }
                    t_lx = td.x; t_ly = td.y;
                    t_frames++;

                    /* Long press on WiFi button → reset WiFi */
                    if (t_frames == LONG_PRESS
                        && s_current_view == VIEW_CLOCK
                        && is_in_wifi_button(t_sx, t_sy)
                        && abs((int)t_lx - (int)t_sx) < TAP_MAX
                        && abs((int)t_ly - (int)t_sy) < TAP_MAX) {
                        ESP_LOGI(TAG, "WiFi reset (long press)");
                        wifi_mgr_erase_credentials();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                } else if (touching) {
                    touching = false;
                    int dx = (int)t_lx - (int)t_sx;
                    int dy = (int)t_ly - (int)t_sy;
                    wifi_mgr_state_t st = wifi_mgr_get_state();

                    if (abs(dx) > SWIPE_MIN && abs(dx) > abs(dy) * 2) {
                        /* Horizontal swipe */
                        int dir = (dx < 0) ? 1 : -1;  /* left = next, right = prev */
                        ESP_LOGI(TAG, "Swipe %s", dir > 0 ? "next" : "prev");
                        disp_msg_t tmsg = { .type = DISP_MSG_TOGGLE, .direction = dir };
                        xQueueSend(s_display_queue, &tmsg, pdMS_TO_TICKS(100));
                    } else if (abs(dx) < TAP_MAX && abs(dy) < TAP_MAX && t_frames < 10) {
                        /* Tap */
                        if (st == WIFI_MGR_STATE_CONNECTED
                            && s_current_view == VIEW_CLOCK
                            && is_in_wifi_button(t_sx, t_sy)) {
                            ESP_LOGI(TAG, "WiFi button tap — settings QR");
                            disp_msg_t smsg = { .type = DISP_MSG_SETTINGS };
                            xQueueSend(s_display_queue, &smsg, pdMS_TO_TICKS(100));
                        } else if (st == WIFI_MGR_STATE_FAILED) {
                            ESP_LOGI(TAG, "Tap in FAILED — rebooting");
                            wifi_mgr_erase_credentials();
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(200));  /* debounce */
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));  /* 50ms polling for smoother gesture detection */
    }
}
