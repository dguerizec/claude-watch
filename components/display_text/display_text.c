#include <string.h>
#include "display_text.h"
#include "font_5x7.h"
#include "esp_heap_caps.h"

#define LCD_WIDTH 240

static inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

void display_text_draw_char(esp_lcd_panel_handle_t panel, int x, int y,
                            char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = font_5x7[c - 0x20];

    int char_w = FONT_CHAR_WIDTH * scale;
    int char_h = FONT_CHAR_HEIGHT * scale;

    /* Clip: skip if entirely off-screen */
    if (x + char_w <= 0 || x >= LCD_WIDTH || y + char_h <= 0 || y >= LCD_WIDTH)
        return;

    uint16_t fg_s = swap16(fg);
    uint16_t bg_s = swap16(bg);

    /* Render one row of scaled pixels at a time via DMA buffer */
    uint16_t *buf = heap_caps_malloc(char_w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) return;

    for (int row = 0; row < FONT_CHAR_HEIGHT; row++) {
        /* Build one row of the character */
        for (int col = 0; col < FONT_CHAR_WIDTH; col++) {
            uint16_t pixel = (glyph[col] & (1 << row)) ? fg_s : bg_s;
            for (int sx = 0; sx < scale; sx++) {
                buf[col * scale + sx] = pixel;
            }
        }

        /* Draw this row 'scale' times (vertical scaling) */
        for (int sy = 0; sy < scale; sy++) {
            int py = y + row * scale + sy;
            if (py >= 0 && py < LCD_WIDTH) {
                int x0 = x < 0 ? 0 : x;
                int x1 = (x + char_w) > LCD_WIDTH ? LCD_WIDTH : (x + char_w);
                int offset = x < 0 ? -x : 0;
                esp_lcd_panel_draw_bitmap(panel, x0, py, x1, py + 1, buf + offset);
            }
        }
    }

    free(buf);
}

void display_text_draw_string(esp_lcd_panel_handle_t panel, int x, int y,
                              const char *str, uint16_t fg, uint16_t bg, int scale)
{
    int step = (FONT_CHAR_WIDTH + FONT_CHAR_SPACING) * scale;
    for (int i = 0; str[i] != '\0'; i++) {
        display_text_draw_char(panel, x + i * step, y, str[i], fg, bg, scale);
    }
}

void display_text_draw_string_centered(esp_lcd_panel_handle_t panel, int y,
                                       const char *str, uint16_t fg, uint16_t bg, int scale)
{
    int len = strlen(str);
    int step = (FONT_CHAR_WIDTH + FONT_CHAR_SPACING) * scale;
    int total_w = len * step - FONT_CHAR_SPACING * scale;
    int x = (LCD_WIDTH - total_w) / 2;
    display_text_draw_string(panel, x, y, str, fg, bg, scale);
}
