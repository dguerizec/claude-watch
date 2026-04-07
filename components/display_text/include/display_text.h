#pragma once

#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_CHAR_WIDTH  5
#define FONT_CHAR_HEIGHT 7
#define FONT_CHAR_SPACING 1  /* 1px gap between characters */

/**
 * Draw a single character at (x, y) with the given scale.
 * Each character is 5x7 pixels at scale=1, 10x14 at scale=2, etc.
 */
void display_text_draw_char(esp_lcd_panel_handle_t panel, int x, int y,
                            char c, uint16_t fg, uint16_t bg, int scale);

/**
 * Draw a null-terminated string starting at (x, y).
 */
void display_text_draw_string(esp_lcd_panel_handle_t panel, int x, int y,
                              const char *str, uint16_t fg, uint16_t bg, int scale);

/**
 * Draw a null-terminated string horizontally centered on a 240px display at row y.
 */
void display_text_draw_string_centered(esp_lcd_panel_handle_t panel, int y,
                                       const char *str, uint16_t fg, uint16_t bg, int scale);

#ifdef __cplusplus
}
#endif
