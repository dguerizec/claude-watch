#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "driver/i2c_master.h"
#include "chsc6x.h"

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

static void draw_pixel(esp_lcd_panel_handle_t panel, int x, int y, uint16_t color)
{
    uint16_t c = swap16(color);
    esp_lcd_panel_draw_bitmap(panel, x, y, x + 1, y + 1, &c);
}

static void draw_hline(esp_lcd_panel_handle_t panel, int x0, int x1, int y, uint16_t color)
{
    int len = x1 - x0;
    uint16_t *buf = heap_caps_malloc(len * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(buf);
    uint16_t c = swap16(color);
    for (int i = 0; i < len; i++) buf[i] = c;
    esp_lcd_panel_draw_bitmap(panel, x0, y, x1, y + 1, buf);
    free(buf);
}

static void draw_vline(esp_lcd_panel_handle_t panel, int x, int y0, int y1, uint16_t color)
{
    uint16_t c = swap16(color);
    for (int y = y0; y < y1; y++) {
        esp_lcd_panel_draw_bitmap(panel, x, y, x + 1, y + 1, &c);
    }
}

static void draw_axes(esp_lcd_panel_handle_t panel)
{
    #define AXIS_RED   0xF800
    #define AXIS_GREEN 0x07E0
    #define TICK_LEN   3

    /* X axis — red horizontal line at y=120 */
    draw_hline(panel, 0, LCD_H_RES, LCD_V_RES / 2, AXIS_RED);
    /* Y axis — green vertical line at x=120 */
    draw_vline(panel, LCD_H_RES / 2, 0, LCD_V_RES, AXIS_GREEN);

    /* Ticks every 10px on X axis */
    for (int x = 0; x < LCD_H_RES; x += 10) {
        for (int t = -TICK_LEN; t <= TICK_LEN; t++) {
            int y = LCD_V_RES / 2 + t;
            if (y >= 0 && y < LCD_V_RES)
                draw_pixel(panel, x, y, AXIS_RED);
        }
    }

    /* Ticks every 10px on Y axis */
    for (int y = 0; y < LCD_V_RES; y += 10) {
        for (int t = -TICK_LEN; t <= TICK_LEN; t++) {
            int x = LCD_H_RES / 2 + t;
            if (x >= 0 && x < LCD_H_RES)
                draw_pixel(panel, x, y, AXIS_GREEN);
        }
    }
}

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
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Display initialized — 240x240 GC9A01");

    /* I2C bus */
    ESP_LOGI(TAG, "I2C init: SDA=GPIO%d SCL=GPIO%d", PIN_TOUCH_SDA, PIN_TOUCH_SCL);
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
        ESP_LOGI(TAG, "Touch ready — tap to change color");
    } else {
        ESP_LOGW(TAG, "Touch init failed — running without touch");
    }

    /* Black background + axes */
    fill_screen(panel_handle, 0x0000);
    draw_axes(panel_handle);
    ESP_LOGI(TAG, "Axes drawn — touch to see coordinates");

    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    while (1) {
        if (touch_ok) {
            chsc6x_touch_data_t td;
            if (chsc6x_read(touch, &td) == ESP_OK && td.touched) {
                ESP_LOGI(TAG, "Touch: x=%d y=%d", td.x, td.y);
                /* Draw 5x5 white dot */
                for (int dy = -2; dy <= 2; dy++) {
                    for (int dx = -2; dx <= 2; dx++) {
                        int px = td.x + dx, py = td.y + dy;
                        if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES)
                            draw_pixel(panel_handle, px, py, 0xFFFF);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
