#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "gc9a01";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl;
    uint8_t colmod;
} gc9a01_panel_t;

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert);
static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap);
static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on);

esp_err_t esp_lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    gc9a01_panel_t *gc9a01 = calloc(1, sizeof(gc9a01_panel_t));
    ESP_RETURN_ON_FALSE(gc9a01, ESP_ERR_NO_MEM, TAG, "no mem for gc9a01 panel");

    gc9a01->io = io;
    gc9a01->reset_gpio_num = panel_dev_config->reset_gpio_num;

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        gc9a01->colmod = 0x55;
        break;
    case 18:
        gc9a01->colmod = 0x66;
        break;
    default:
        ESP_LOGE(TAG, "unsupported pixel width");
        free(gc9a01);
        return ESP_ERR_NOT_SUPPORTED;
    }

    gc9a01->madctl = 0;
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        gc9a01->madctl |= (1 << 3); /* BGR bit */
    }

    gc9a01->base.del = panel_gc9a01_del;
    gc9a01->base.reset = panel_gc9a01_reset;
    gc9a01->base.init = panel_gc9a01_init;
    gc9a01->base.draw_bitmap = panel_gc9a01_draw_bitmap;
    gc9a01->base.invert_color = panel_gc9a01_invert_color;
    gc9a01->base.mirror = panel_gc9a01_mirror;
    gc9a01->base.swap_xy = panel_gc9a01_swap_xy;
    gc9a01->base.set_gap = panel_gc9a01_set_gap;
    gc9a01->base.disp_on_off = panel_gc9a01_disp_on_off;

    *ret_panel = &(gc9a01->base);
    ESP_LOGI(TAG, "new GC9A01 panel @%p", gc9a01);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    if (gc9a01->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9a01->reset_gpio_num);
    }
    free(gc9a01);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    if (gc9a01->reset_gpio_num >= 0) {
        gpio_set_direction(gc9a01->reset_gpio_num, GPIO_MODE_OUTPUT);
        gpio_set_level(gc9a01->reset_gpio_num, !gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9a01->reset_gpio_num, gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9a01->reset_gpio_num, !gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        /* Software reset */
        esp_lcd_panel_io_tx_param(gc9a01->io, 0x01, NULL, 0); /* SWRESET */
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    /* GC9A01 initialization sequence */
    /* Enable inter-register access */
    esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xEB, (uint8_t[]){0x14}, 1);
    esp_lcd_panel_io_tx_param(io, 0xFE, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xEB, (uint8_t[]){0x14}, 1);
    esp_lcd_panel_io_tx_param(io, 0x84, (uint8_t[]){0x40}, 1);
    esp_lcd_panel_io_tx_param(io, 0x85, (uint8_t[]){0xFF}, 1);
    esp_lcd_panel_io_tx_param(io, 0x86, (uint8_t[]){0xFF}, 1);
    esp_lcd_panel_io_tx_param(io, 0x87, (uint8_t[]){0xFF}, 1);
    esp_lcd_panel_io_tx_param(io, 0x88, (uint8_t[]){0x0A}, 1);
    esp_lcd_panel_io_tx_param(io, 0x89, (uint8_t[]){0x21}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8A, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8B, (uint8_t[]){0x80}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8C, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8D, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8E, (uint8_t[]){0xFF}, 1);
    esp_lcd_panel_io_tx_param(io, 0x8F, (uint8_t[]){0xFF}, 1);
    esp_lcd_panel_io_tx_param(io, 0xB6, (uint8_t[]){0x00, 0x00}, 2);

    /* MADCTL */
    esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]){gc9a01->madctl}, 1);

    /* COLMOD - pixel format */
    esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]){gc9a01->colmod}, 1);

    esp_lcd_panel_io_tx_param(io, 0x90, (uint8_t[]){0x08, 0x08, 0x08, 0x08}, 4);
    esp_lcd_panel_io_tx_param(io, 0xBD, (uint8_t[]){0x06}, 1);
    esp_lcd_panel_io_tx_param(io, 0xBC, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(io, 0xFF, (uint8_t[]){0x60, 0x01, 0x04}, 3);

    /* Power control */
    esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC9, (uint8_t[]){0x22}, 1);
    esp_lcd_panel_io_tx_param(io, 0xBE, (uint8_t[]){0x11}, 1);
    esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]){0x10, 0x0E}, 2);
    esp_lcd_panel_io_tx_param(io, 0xDF, (uint8_t[]){0x21, 0x0C, 0x02}, 3);

    /* Gamma */
    esp_lcd_panel_io_tx_param(io, 0xF0, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6);
    esp_lcd_panel_io_tx_param(io, 0xF1, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6);
    esp_lcd_panel_io_tx_param(io, 0xF2, (uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6);
    esp_lcd_panel_io_tx_param(io, 0xF3, (uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6);

    esp_lcd_panel_io_tx_param(io, 0xED, (uint8_t[]){0x1B, 0x0B}, 2);
    esp_lcd_panel_io_tx_param(io, 0xAE, (uint8_t[]){0x77}, 1);
    esp_lcd_panel_io_tx_param(io, 0xCD, (uint8_t[]){0x63}, 1);
    esp_lcd_panel_io_tx_param(io, 0x70, (uint8_t[]){0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9);
    esp_lcd_panel_io_tx_param(io, 0xE8, (uint8_t[]){0x34}, 1);
    esp_lcd_panel_io_tx_param(io, 0x62, (uint8_t[]){0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12);
    esp_lcd_panel_io_tx_param(io, 0x63, (uint8_t[]){0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12);
    esp_lcd_panel_io_tx_param(io, 0x64, (uint8_t[]){0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7);
    esp_lcd_panel_io_tx_param(io, 0x66, (uint8_t[]){0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10);
    esp_lcd_panel_io_tx_param(io, 0x67, (uint8_t[]){0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10);
    esp_lcd_panel_io_tx_param(io, 0x74, (uint8_t[]){0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7);
    esp_lcd_panel_io_tx_param(io, 0x98, (uint8_t[]){0x3E, 0x07}, 2);
    esp_lcd_panel_io_tx_param(io, 0x35, NULL, 0); /* Tearing effect on */
    esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0); /* Inversion on */

    /* Sleep out */
    esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Display on */
    esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "GC9A01 initialized");
    return ESP_OK;
}

static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    x_start += gc9a01->x_gap;
    x_end += gc9a01->x_gap;
    y_start += gc9a01->y_gap;
    y_end += gc9a01->y_gap;

    /* Column address set */
    esp_lcd_panel_io_tx_param(io, 0x2A, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4);

    /* Row address set */
    esp_lcd_panel_io_tx_param(io, 0x2B, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4);

    /* Memory write */
    size_t len = (x_end - x_start) * (y_end - y_start) * 2; /* 16-bit color */
    esp_lcd_panel_io_tx_color(io, 0x2C, color_data, len);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    uint8_t cmd = invert ? 0x21 : 0x20;
    esp_lcd_panel_io_tx_param(gc9a01->io, cmd, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    gc9a01->madctl &= ~(0x40 | 0x80);
    if (mirror_x) gc9a01->madctl |= 0x40;
    if (mirror_y) gc9a01->madctl |= 0x80;
    esp_lcd_panel_io_tx_param(gc9a01->io, 0x36, (uint8_t[]){gc9a01->madctl}, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    gc9a01->madctl &= ~0x20;
    if (swap) gc9a01->madctl |= 0x20;
    esp_lcd_panel_io_tx_param(gc9a01->io, 0x36, (uint8_t[]){gc9a01->madctl}, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    gc9a01->x_gap = x_gap;
    gc9a01->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    uint8_t cmd = on ? 0x29 : 0x28;
    esp_lcd_panel_io_tx_param(gc9a01->io, cmd, NULL, 0);
    return ESP_OK;
}
