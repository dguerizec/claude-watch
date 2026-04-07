#include "polar_graph.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "polar_graph";

#define CX       120
#define CY       120
#define MAX_R    108
#define W        240
#define H        240
#define STRIP_H  40       /* rows per strip — 240*40*2 = 19,200 bytes */
#define PERIOD_S (7 * 24 * 3600)

static inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }

/* ── Strip-local drawing primitives ──────────────────────────────────── */

static inline void spx(uint16_t *strip, int sy, int sh, int x, int y, uint16_t c)
{
    int ly = y - sy;
    if ((unsigned)x < W && (unsigned)ly < (unsigned)sh)
        strip[ly * W + x] = c;
}

static void sline(uint16_t *strip, int sy, int sh,
                  int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), step_x = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), step_y = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        spx(strip, sy, sh, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += step_x; }
        if (e2 <= dx) { err += dx; y0 += step_y; }
    }
}

/* Reference circles — computed analytically per row */
static void draw_circles(uint16_t *strip, int sy, int sh, uint16_t c)
{
    static const int radii[] = { MAX_R / 4, MAX_R / 2, MAX_R * 3 / 4, MAX_R };
    for (int row = sy; row < sy + sh; row++) {
        int dy = row - CY;
        for (int i = 0; i < 4; i++) {
            int r = radii[i];
            if (dy < -r || dy > r) continue;
            int dx = (int)sqrtf((float)(r * r - dy * dy));
            spx(strip, sy, sh, CX - dx, row, c);
            spx(strip, sy, sh, CX + dx, row, c);
        }
    }
}

static inline void polar_xy(float angle, float r, int *x, int *y)
{
    *x = CX + (int)(r * sinf(angle));
    *y = CY - (int)(r * cosf(angle));
}

/* ── Public API ──────────────────────────────────────────────────────── */

void polar_graph_draw(esp_lcd_panel_handle_t panel,
                      const usage_data_point_t *points, int num_points,
                      time_t period_start, time_t now)
{
    /* Pre-compute screen coordinates for all data points */
    int *scr_x = NULL, *scr_y = NULL;
    if (num_points > 0) {
        scr_x = malloc(num_points * sizeof(int));
        scr_y = malloc(num_points * sizeof(int));
        if (!scr_x || !scr_y) {
            free(scr_x); free(scr_y);
            ESP_LOGE(TAG, "Failed to alloc point coords");
            return;
        }
        for (int i = 0; i < num_points; i++) {
            float frac = (float)(points[i].timestamp - period_start) / PERIOD_S;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            float angle = 2.0f * M_PI * frac;
            float r = (points[i].value / 100.0f) * MAX_R;
            if (r < 0) r = 0;
            if (r > MAX_R) r = MAX_R;
            polar_xy(angle, r, &scr_x[i], &scr_y[i]);
        }
    }

    /* Pre-compute "now" hand endpoint */
    float now_frac = (float)(now - period_start) / PERIOD_S;
    if (now_frac < 0.0f) now_frac = 0.0f;
    if (now_frac > 1.0f) now_frac = 1.0f;
    float now_angle = 2.0f * M_PI * now_frac;
    int now_x, now_y;
    polar_xy(now_angle, MAX_R, &now_x, &now_y);

    /* Pre-compute day tick endpoints (7 ticks) */
    int tick_x0[7], tick_y0[7], tick_x1[7], tick_y1[7];
    for (int d = 0; d < 7; d++) {
        float a = 2.0f * M_PI * d / 7.0f;
        polar_xy(a, MAX_R - 8, &tick_x0[d], &tick_y0[d]);
        polar_xy(a, MAX_R,     &tick_x1[d], &tick_y1[d]);
    }

    /* Colors (byte-swapped for SPI) */
    uint16_t c_grid = sw(0x2945);   /* dark gray — reference circles */
    uint16_t c_tick = sw(0x4208);   /* medium gray — day ticks */
    uint16_t c_data = sw(0x07FF);   /* cyan — data line */
    uint16_t c_now  = sw(0xFFFF);   /* white — "now" hand */

    /* Double-buffered strip rendering.
     * esp_lcd_panel_draw_bitmap is async (trans_queue_depth=10): the DMA
     * may still read from the buffer after the call returns. Two buffers
     * ensure the DMA finishes reading buffer A while we render into B. */
    size_t strip_bytes = W * STRIP_H * sizeof(uint16_t);
    uint16_t *buf[2];
    buf[0] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    buf[1] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    if (!buf[0] || !buf[1]) {
        free(buf[0]); free(buf[1]);
        free(scr_x); free(scr_y);
        ESP_LOGE(TAG, "Failed to alloc strip buffers");
        return;
    }

    int bi = 0;
    for (int sy = 0; sy < H; sy += STRIP_H) {
        int sh = (sy + STRIP_H <= H) ? STRIP_H : H - sy;
        uint16_t *strip = buf[bi];
        memset(strip, 0, W * sh * sizeof(uint16_t));

        /* Reference circles */
        draw_circles(strip, sy, sh, c_grid);

        /* Day tick marks */
        for (int d = 0; d < 7; d++) {
            int ymin = tick_y0[d] < tick_y1[d] ? tick_y0[d] : tick_y1[d];
            int ymax = tick_y0[d] > tick_y1[d] ? tick_y0[d] : tick_y1[d];
            if (ymax < sy || ymin >= sy + sh) continue;
            sline(strip, sy, sh, tick_x0[d], tick_y0[d], tick_x1[d], tick_y1[d], c_tick);
        }

        /* Data polyline */
        for (int i = 0; i < num_points - 1; i++) {
            int ymin = scr_y[i] < scr_y[i+1] ? scr_y[i] : scr_y[i+1];
            int ymax = scr_y[i] > scr_y[i+1] ? scr_y[i] : scr_y[i+1];
            if (ymax < sy || ymin >= sy + sh) continue;
            sline(strip, sy, sh, scr_x[i], scr_y[i], scr_x[i+1], scr_y[i+1], c_data);
        }

        /* "Now" radial hand */
        {
            int ymin = CY < now_y ? CY : now_y;
            int ymax = CY > now_y ? CY : now_y;
            if (!(ymax < sy || ymin >= sy + sh))
                sline(strip, sy, sh, CX, CY, now_x, now_y, c_now);
        }

        esp_lcd_panel_draw_bitmap(panel, 0, sy, W, sy + sh, strip);
        bi = 1 - bi;  /* swap buffer for next strip */
    }

    /* Small delay to ensure last DMA transfer completes before freeing */
    vTaskDelay(pdMS_TO_TICKS(2));

    free(buf[0]);
    free(buf[1]);
    free(scr_x);
    free(scr_y);
    ESP_LOGI(TAG, "Graph drawn: %d points", num_points);
}
