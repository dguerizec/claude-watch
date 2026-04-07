#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate and display a QR code on the GC9A01 240x240 round display.
 * The QR code is centered and scaled to fit within the visible circular area.
 *
 * @param panel  LCD panel handle
 * @param text   Text to encode (e.g. "http://192.168.4.1")
 * @param fg     Foreground color (dark modules), typically 0x0000 (black)
 * @param bg     Background color (light modules), typically 0xFFFF (white)
 * @return ESP_OK on success, ESP_FAIL on encoding error
 */
esp_err_t qr_display_show(esp_lcd_panel_handle_t panel, const char *text,
                          uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif
