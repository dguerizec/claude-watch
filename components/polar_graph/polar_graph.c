#include "polar_graph.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "polar_graph";

#define CX       120
#define CY       120
#define MIN_R    30       /* 0% radius — leaves room for text at center */
#define MAX_R    108
#define W        240
#define H        240
#define STRIP_H  40       /* rows per strip — 240*40*2 = 19,200 bytes */
#define PERIOD_S (7 * 24 * 3600)
#define SPIRAL_N 180      /* points for burn-rate spiral (every 2°) */

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

/* Midpoint circle — 8-way symmetric, no gaps */
static void draw_circle(uint16_t *strip, int sy, int sh, int r, uint16_t c)
{
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        spx(strip, sy, sh, CX+x, CY+y, c); spx(strip, sy, sh, CX-x, CY+y, c);
        spx(strip, sy, sh, CX+x, CY-y, c); spx(strip, sy, sh, CX-x, CY-y, c);
        spx(strip, sy, sh, CX+y, CY+x, c); spx(strip, sy, sh, CX-y, CY+x, c);
        spx(strip, sy, sh, CX+y, CY-x, c); spx(strip, sy, sh, CX-y, CY-x, c);
        y++;
        if (d <= 0) d += 2 * y + 1;
        else { x--; d += 2 * (y - x) + 1; }
    }
}

static void draw_circles(uint16_t *strip, int sy, int sh, uint16_t c)
{
    static const int radii[] = {
        MIN_R,
        MIN_R + (MAX_R - MIN_R) / 4,
        MIN_R + (MAX_R - MIN_R) / 2,
        MIN_R + (MAX_R - MIN_R) * 3 / 4,
        MAX_R
    };
    for (int i = 0; i < 5; i++)
        draw_circle(strip, sy, sh, radii[i], c);
}

static inline void polar_xy(float angle, float r, int *x, int *y)
{
    *x = CX + (int)(r * sinf(angle));
    *y = CY - (int)(r * cosf(angle));
}

/* Compute burn rate (0–100) for a timestamp within its billing period */
static inline float burn_rate_at(time_t ts, time_t billing_start)
{
    float frac = (float)(ts - billing_start) / PERIOD_S;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac * 100.0f;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void polar_graph_draw(esp_lcd_panel_handle_t panel,
                      const usage_data_point_t *points, int num_points,
                      time_t period_end, time_t now)
{
    time_t billing_start = period_end - PERIOD_S;
    time_t prev_billing_start = billing_start - PERIOD_S;

    /* Find boundary between previous and current billing week */
    int boundary = num_points;
    for (int i = 0; i < num_points; i++) {
        if (points[i].timestamp >= billing_start) {
            boundary = i;
            break;
        }
    }

    /* Pre-compute screen coordinates and per-point colors */
    int *scr_x = NULL, *scr_y = NULL;
    uint16_t *pt_color = NULL;

    /* Colors (byte-swapped for SPI) */
    uint16_t c_grid     = sw(0x2945);   /* dark gray — reference circles */
    uint16_t c_tick     = sw(0x4208);   /* medium gray — day ticks */
    uint16_t c_spiral   = sw(0x4208);   /* dark gray — burn rate spiral */
    uint16_t c_now      = sw(0x4208);   /* dark gray — "now" hand */
    uint16_t c_curr_ok  = sw(0x07E0);   /* bright green — current week, under budget */
    uint16_t c_curr_over= sw(0xF800);   /* bright red — current week, over budget */
    uint16_t c_prev_ok  = sw(0x03E0);   /* dark green — previous week, under budget */
    uint16_t c_prev_over= sw(0x7800);   /* dark red — previous week, over budget */

    if (num_points > 0) {
        scr_x = malloc(num_points * sizeof(int));
        scr_y = malloc(num_points * sizeof(int));
        pt_color = malloc(num_points * sizeof(uint16_t));
        if (!scr_x || !scr_y || !pt_color) {
            free(scr_x); free(scr_y); free(pt_color);
            ESP_LOGE(TAG, "Failed to alloc point arrays");
            return;
        }
        for (int i = 0; i < num_points; i++) {
            float frac = (float)(points[i].timestamp - period_end) / PERIOD_S;
            frac = fmodf(frac, 1.0f);
            if (frac < 0.0f) frac += 1.0f;
            float angle = 2.0f * M_PI * frac;
            float r = MIN_R + (points[i].value / 100.0f) * (MAX_R - MIN_R);
            if (r < MIN_R) r = MIN_R;
            if (r > MAX_R) r = MAX_R;
            polar_xy(angle, r, &scr_x[i], &scr_y[i]);

            /* Determine color: green if under burn rate, red if over */
            bool is_current = (i >= boundary);
            time_t bs = is_current ? billing_start : prev_billing_start;
            float br = burn_rate_at(points[i].timestamp, bs);
            bool over = (points[i].value >= br);
            if (is_current)
                pt_color[i] = over ? c_curr_over : c_curr_ok;
            else
                pt_color[i] = over ? c_prev_over : c_prev_ok;
        }
    }

    /* Pre-compute burn-rate spiral (Archimedean: 0% at north → 100% at north) */
    int spi_x[SPIRAL_N], spi_y[SPIRAL_N];
    for (int i = 0; i < SPIRAL_N; i++) {
        float frac = (float)i / SPIRAL_N;
        float angle = 2.0f * M_PI * frac;
        float r = MIN_R + frac * (MAX_R - MIN_R);
        polar_xy(angle, r, &spi_x[i], &spi_y[i]);
    }

    /* Pre-compute "now" hand endpoint */
    float now_frac = (float)(now - period_end) / PERIOD_S;
    now_frac = fmodf(now_frac, 1.0f);
    if (now_frac < 0.0f) now_frac += 1.0f;
    float now_angle = 2.0f * M_PI * now_frac;
    int now_x0, now_y0, now_x1, now_y1;
    polar_xy(now_angle, MIN_R, &now_x0, &now_y0);
    polar_xy(now_angle, MAX_R, &now_x1, &now_y1);

    /* Pre-compute day tick endpoints (7 ticks) */
    int tick_x0[7], tick_y0[7], tick_x1[7], tick_y1[7];
    for (int d = 0; d < 7; d++) {
        float a = 2.0f * M_PI * d / 7.0f;
        polar_xy(a, MAX_R - 8, &tick_x0[d], &tick_y0[d]);
        polar_xy(a, MAX_R,     &tick_x1[d], &tick_y1[d]);
    }

    /* Double-buffered strip rendering */
    size_t strip_bytes = W * STRIP_H * sizeof(uint16_t);
    uint16_t *buf[2];
    buf[0] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    buf[1] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    if (!buf[0] || !buf[1]) {
        free(buf[0]); free(buf[1]);
        free(scr_x); free(scr_y); free(pt_color);
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

        /* Burn-rate spiral */
        for (int i = 0; i < SPIRAL_N - 1; i++) {
            int ymin = spi_y[i] < spi_y[i+1] ? spi_y[i] : spi_y[i+1];
            int ymax = spi_y[i] > spi_y[i+1] ? spi_y[i] : spi_y[i+1];
            if (ymax < sy || ymin >= sy + sh) continue;
            sline(strip, sy, sh, spi_x[i], spi_y[i], spi_x[i+1], spi_y[i+1], c_spiral);
        }

        /* Data polyline — color per segment based on burn rate */
        for (int i = 0; i < num_points - 1; i++) {
            if (i + 1 == boundary) continue;  /* break at period boundary */
            int ymin = scr_y[i] < scr_y[i+1] ? scr_y[i] : scr_y[i+1];
            int ymax = scr_y[i] > scr_y[i+1] ? scr_y[i] : scr_y[i+1];
            if (ymax < sy || ymin >= sy + sh) continue;
            sline(strip, sy, sh, scr_x[i], scr_y[i], scr_x[i+1], scr_y[i+1], pt_color[i + 1]);
        }

        /* "Now" hand — from MIN_R to MAX_R only */
        {
            int ymin = now_y0 < now_y1 ? now_y0 : now_y1;
            int ymax = now_y0 > now_y1 ? now_y0 : now_y1;
            if (!(ymax < sy || ymin >= sy + sh))
                sline(strip, sy, sh, now_x0, now_y0, now_x1, now_y1, c_now);
        }

        esp_lcd_panel_draw_bitmap(panel, 0, sy, W, sy + sh, strip);
        bi = 1 - bi;
    }

    vTaskDelay(pdMS_TO_TICKS(2));

    free(buf[0]);
    free(buf[1]);
    free(scr_x);
    free(scr_y);
    free(pt_color);
    ESP_LOGI(TAG, "Graph drawn: %d points, boundary=%d", num_points, boundary);
}
