#include <string.h>
#include "qr_display.h"
#include "qrcodegen.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define LCD_SIZE 240
/* Safe zone diameter to stay within the round display's visible area */
#define QR_SAFE_ZONE 200
/* Max QR version we need: 10 is plenty for short URLs (up to ~174 chars) */
#define QR_VERSION_MAX 10
#define QR_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)

static const char *TAG = "qr_display";

static inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

esp_err_t qr_display_show(esp_lcd_panel_handle_t panel, const char *text,
                          uint16_t fg, uint16_t bg)
{
    /* Heap-allocate QR buffers (avoid ~8KB stack usage) */
    uint8_t *qr_buf = malloc(QR_BUF_LEN);
    uint8_t *temp_buf = malloc(QR_BUF_LEN);
    if (!qr_buf || !temp_buf) {
        ESP_LOGE(TAG, "Failed to allocate QR buffers");
        free(qr_buf);
        free(temp_buf);
        return ESP_FAIL;
    }

    bool ok = qrcodegen_encodeText(text, temp_buf, qr_buf,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, QR_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    /* temp_buf no longer needed after encoding */
    free(temp_buf);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to encode QR code for: %s", text);
        free(qr_buf);
        return ESP_FAIL;
    }

    int qr_size = qrcodegen_getSize(qr_buf);
    /* Add 2-module quiet zone on each side */
    int total_modules = qr_size + 4;
    int scale = QR_SAFE_ZONE / total_modules;
    if (scale < 1) scale = 1;

    int qr_pixels = total_modules * scale;
    int offset_x = (LCD_SIZE - qr_pixels) / 2;
    int offset_y = (LCD_SIZE - qr_pixels) / 2;

    ESP_LOGI(TAG, "QR: %dx%d modules, scale=%d, %dx%d px, offset=(%d,%d)",
             qr_size, qr_size, scale, qr_pixels, qr_pixels, offset_x, offset_y);

    uint16_t fg_s = swap16(fg);
    uint16_t bg_s = swap16(bg);

    /* Allocate one line buffer for a full row of the QR image */
    uint16_t *line_buf = heap_caps_malloc(qr_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line_buf) {
        ESP_LOGE(TAG, "Failed to allocate QR line buffer");
        return ESP_FAIL;
    }

    /* Fill background for the entire QR area first (including quiet zone) */
    for (int i = 0; i < qr_pixels; i++) {
        line_buf[i] = bg_s;
    }
    for (int py = 0; py < qr_pixels; py++) {
        esp_lcd_panel_draw_bitmap(panel, offset_x, offset_y + py,
                                  offset_x + qr_pixels, offset_y + py + 1, line_buf);
    }

    /* Draw QR modules row by row */
    for (int qy = 0; qy < qr_size; qy++) {
        /* Build one row of scaled modules */
        for (int qx = 0; qx < qr_size; qx++) {
            uint16_t color = qrcodegen_getModule(qr_buf, qx, qy) ? fg_s : bg_s;
            int px_start = (qx + 2) * scale;  /* +2 for quiet zone */
            for (int sx = 0; sx < scale; sx++) {
                line_buf[px_start + sx] = color;
            }
        }

        /* Draw this row 'scale' times */
        for (int sy = 0; sy < scale; sy++) {
            int py = offset_y + (qy + 2) * scale + sy;
            esp_lcd_panel_draw_bitmap(panel, offset_x, py,
                                      offset_x + qr_pixels, py + 1, line_buf);
        }

        /* Reset quiet zone columns for next row */
        for (int i = 0; i < 2 * scale; i++) {
            line_buf[i] = bg_s;
            line_buf[qr_pixels - 1 - i] = bg_s;
        }
    }

    free(line_buf);
    free(qr_buf);
    ESP_LOGI(TAG, "QR code displayed successfully");
    return ESP_OK;
}
