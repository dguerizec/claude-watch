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

static inline void polar_xy(float angle, float r, int *x, int *y)
{
    *x = CX + (int)(r * sinf(angle));
    *y = CY - (int)(r * cosf(angle));
}

/* Draw an arc between two polar points, interpolating in polar space.
 * For small angular distances, this degenerates to a single sline. */
static void polar_arc(uint16_t *strip, int sy, int sh,
                      float a0, float r0, float a1, float r1, uint16_t c)
{
    float da = a1 - a0;
    /* Shortest path around the circle */
    if (da > M_PI) da -= 2.0f * M_PI;
    if (da < -M_PI) da += 2.0f * M_PI;

    /* ~3 pixels per sub-segment at the outer radius */
    int steps = (int)(fabsf(da) * MAX_R / 3.0f) + 1;
    if (steps > 120) steps = 120;

    int px = CX + (int)(r0 * sinf(a0));
    int py = CY - (int)(r0 * cosf(a0));
    for (int s = 1; s <= steps; s++) {
        float t = (float)s / steps;
        float a = a0 + da * t;
        float r = r0 + (r1 - r0) * t;
        int nx = CX + (int)(r * sinf(a));
        int ny = CY - (int)(r * cosf(a));
        sline(strip, sy, sh, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

/* Compute burn rate (0–100) for a timestamp within its billing period */
static inline float burn_rate_at(time_t ts, time_t week_start, int psecs)
{
    float frac = (float)(ts - week_start) / psecs;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac * 100.0f;
}

/* ── Color tables — progressively dimmer for older rotations ──────────── */

#define MAX_ROTATIONS 8

/* Green (under budget): bright → dim */
static const uint16_t greens[MAX_ROTATIONS] = {
    0x07E0, 0x04E0, 0x02E0, 0x0160, 0x00C0, 0x0060, 0x0040, 0x0020,
};

/* Red (over budget): bright → dim */
static const uint16_t reds[MAX_ROTATIONS] = {
    0xF800, 0xA000, 0x6000, 0x3000, 0x1800, 0x1000, 0x0800, 0x0800,
};

/* ── Public API ──────────────────────────────────────────────────────── */

void polar_graph_draw(esp_lcd_panel_handle_t panel,
                      const usage_data_point_t *points, int num_points,
                      int period_secs, int num_rotations, int num_ticks,
                      time_t period_end, time_t now,
                      float recovery_angle)
{
    /* Pre-compute per-point: screen coords, week index, color.
     * int16_t for coords (0–239 range) to halve memory vs int. */
    if (num_rotations > MAX_ROTATIONS) num_rotations = MAX_ROTATIONS;

    int16_t *scr_x = NULL, *scr_y = NULL;
    float *pt_angle = NULL, *pt_radius = NULL;
    uint8_t *pt_week = NULL;
    uint16_t *pt_color = NULL;

    if (num_points > 0) {
        scr_x = malloc(num_points * sizeof(int16_t));
        scr_y = malloc(num_points * sizeof(int16_t));
        pt_angle = malloc(num_points * sizeof(float));
        pt_radius = malloc(num_points * sizeof(float));
        pt_week = malloc(num_points);
        pt_color = malloc(num_points * sizeof(uint16_t));
        if (!scr_x || !scr_y || !pt_angle || !pt_radius || !pt_week || !pt_color) {
            free(scr_x); free(scr_y); free(pt_angle); free(pt_radius);
            free(pt_week); free(pt_color);
            ESP_LOGE(TAG, "Failed to alloc point arrays");
            return;
        }
        for (int i = 0; i < num_points; i++) {
            /* Angular position (wraps every 7 days, north = period_end) */
            float frac = (float)(points[i].timestamp - period_end) / period_secs;
            frac = fmodf(frac, 1.0f);
            if (frac < 0.0f) frac += 1.0f;
            float angle = 2.0f * M_PI * frac;
            float r = MIN_R + (points[i].value / 100.0f) * (MAX_R - MIN_R);
            if (r < MIN_R) r = MIN_R;
            if (r > MAX_R) r = MAX_R;
            pt_angle[i] = angle;
            pt_radius[i] = r;
            int sx, sy;
            polar_xy(angle, r, &sx, &sy);
            scr_x[i] = sx;
            scr_y[i] = sy;

            /* Week index: 0 = current, 1 = previous, ... */
            int w_actual = (int)((float)(period_end - points[i].timestamp) / period_secs);
            if (w_actual < 0) w_actual = 0;
            int w = (w_actual >= num_rotations) ? num_rotations - 1 : w_actual;
            pt_week[i] = w;

            /* Color: green/red based on burn rate — use actual period, not clamped */
            time_t week_start = period_end - (w_actual + 1) * (time_t)period_secs;
            float br = burn_rate_at(points[i].timestamp, week_start, period_secs);
            bool over = (points[i].value >= br);
            int dim = (w_actual >= MAX_ROTATIONS) ? MAX_ROTATIONS - 1 : w_actual;
            pt_color[i] = sw(over ? reds[dim] : greens[dim]);
        }
    }

    /* Pre-compute burn-rate spiral */
    int spi_x[SPIRAL_N], spi_y[SPIRAL_N];
    for (int i = 0; i < SPIRAL_N; i++) {
        float frac = (float)i / SPIRAL_N;
        float angle = 2.0f * M_PI * frac;
        float r = MIN_R + frac * (MAX_R - MIN_R);
        polar_xy(angle, r, &spi_x[i], &spi_y[i]);
    }

    /* Pre-compute "now" hand */
    float now_frac = (float)(now - period_end) / period_secs;
    now_frac = fmodf(now_frac, 1.0f);
    if (now_frac < 0.0f) now_frac += 1.0f;
    float now_angle = 2.0f * M_PI * now_frac;
    int now_x0, now_y0, now_x1, now_y1;
    polar_xy(now_angle, MIN_R, &now_x0, &now_y0);
    polar_xy(now_angle, MAX_R, &now_x1, &now_y1);

    /* Pre-compute recovery marker triangle (red, pointing inward) */
    int tr_tip_x = -1, tr_tip_y, tr_bl_x, tr_bl_y, tr_br_x, tr_br_y;
    if (recovery_angle >= 0) {
        float delta = 0.03f;  /* angular half-width ~3px at edge */
        polar_xy(recovery_angle, MAX_R - 8, &tr_tip_x, &tr_tip_y);
        polar_xy(recovery_angle - delta, MAX_R, &tr_bl_x, &tr_bl_y);
        polar_xy(recovery_angle + delta, MAX_R, &tr_br_x, &tr_br_y);
    }

    /* Pre-compute ticks */
    if (num_ticks > 12) num_ticks = 12;
    int tick_x0[12], tick_y0[12], tick_x1[12], tick_y1[12];
    for (int d = 0; d < num_ticks; d++) {
        float a = 2.0f * M_PI * d / num_ticks;
        polar_xy(a, MAX_R - 8, &tick_x0[d], &tick_y0[d]);
        polar_xy(a, MAX_R,     &tick_x1[d], &tick_y1[d]);
    }

    /* Colors for non-data elements */
    uint16_t c_grid   = sw(0x2945);
    uint16_t c_tick   = sw(0x4208);
    uint16_t c_spiral = sw(0x4208);
    uint16_t c_now    = sw(0x4208);

    /* Double-buffered strip rendering */
    size_t strip_bytes = W * STRIP_H * sizeof(uint16_t);
    uint16_t *buf[2];
    buf[0] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    buf[1] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);
    if (!buf[0] || !buf[1]) {
        free(buf[0]); free(buf[1]);
        free(scr_x); free(scr_y); free(pt_week); free(pt_color);
        ESP_LOGE(TAG, "Failed to alloc strip buffers");
        return;
    }

    int bi = 0;
    for (int sy = 0; sy < H; sy += STRIP_H) {
        int sh = (sy + STRIP_H <= H) ? STRIP_H : H - sy;
        uint16_t *strip = buf[bi];
        memset(strip, 0, W * sh * sizeof(uint16_t));

        /* Reference circles (0% and 100%) */
        draw_circle(strip, sy, sh, MIN_R, c_grid);
        draw_circle(strip, sy, sh, MAX_R, c_grid);

        /* Tick marks */
        for (int d = 0; d < num_ticks; d++) {
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

        /* Data polyline — oldest week first, newest last (z-order).
         * Skip segment if value decreases (= billing period reset).
         * Use polar_arc to interpolate along the circle instead of
         * straight Bresenham lines that cut chords. */
        for (int w = num_rotations - 1; w >= 0; w--) {
            for (int i = 0; i < num_points - 1; i++) {
                if (pt_week[i] != w || pt_week[i + 1] != w) continue;
                if (points[i].value > points[i + 1].value) continue;
                /* Coarse Y cull using max possible radius */
                float rmax = pt_radius[i] > pt_radius[i+1] ? pt_radius[i] : pt_radius[i+1];
                if (CY - (int)rmax > sy + sh || CY + (int)rmax < sy) continue;
                polar_arc(strip, sy, sh,
                          pt_angle[i], pt_radius[i],
                          pt_angle[i+1], pt_radius[i+1], pt_color[i + 1]);
            }
        }

        /* "Now" hand — from MIN_R to MAX_R only */
        {
            int ymin = now_y0 < now_y1 ? now_y0 : now_y1;
            int ymax = now_y0 > now_y1 ? now_y0 : now_y1;
            if (!(ymax < sy || ymin >= sy + sh))
                sline(strip, sy, sh, now_x0, now_y0, now_x1, now_y1, c_now);
        }

        /* Recovery marker — red triangle pointing inward */
        if (tr_tip_x >= 0) {
            uint16_t c_marker = sw(0xF800);
            int ys[3] = { tr_tip_y, tr_bl_y, tr_br_y };
            int ymin = ys[0], ymax = ys[0];
            for (int k = 1; k < 3; k++) {
                if (ys[k] < ymin) ymin = ys[k];
                if (ys[k] > ymax) ymax = ys[k];
            }
            if (!(ymax < sy || ymin >= sy + sh)) {
                sline(strip, sy, sh, tr_tip_x, tr_tip_y, tr_bl_x, tr_bl_y, c_marker);
                sline(strip, sy, sh, tr_tip_x, tr_tip_y, tr_br_x, tr_br_y, c_marker);
                sline(strip, sy, sh, tr_bl_x, tr_bl_y, tr_br_x, tr_br_y, c_marker);
            }
        }

        esp_lcd_panel_draw_bitmap(panel, 0, sy, W, sy + sh, strip);
        bi = 1 - bi;
    }

    vTaskDelay(pdMS_TO_TICKS(2));

    free(buf[0]);
    free(buf[1]);
    free(scr_x);
    free(scr_y);
    free(pt_angle);
    free(pt_radius);
    free(pt_week);
    free(pt_color);
    ESP_LOGI(TAG, "Graph drawn: %d points, %d weeks", num_points, num_rotations);
}
