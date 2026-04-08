#pragma once

#include <time.h>
#include "esp_lcd_panel_ops.h"
#include "usage_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draw a polar graph on the round display.
 *
 * Maps a time period onto a full circle:
 *   - North (top) = reset time (end of billing period)
 *   - Clockwise winding
 *   - Center = 0%, edge = 100%
 *   - A radial "now" line separates current from stale data
 *
 * @param panel        LCD panel handle
 * @param points       Array of (timestamp, value) data points
 * @param num_points   Number of data points
 * @param period_secs  Duration of one rotation in seconds (e.g. 7*86400 or 5*3600)
 * @param num_rotations Number of historical rotations to display
 * @param num_ticks    Number of tick marks around the circle (e.g. 7 for days, 5 for hours)
 * @param period_end   Reset time — mapped to north (12 o'clock)
 * @param now          Current time (epoch) — for the "now" hand
 */
void polar_graph_draw(esp_lcd_panel_handle_t panel,
                      const usage_data_point_t *points, int num_points,
                      int period_secs, int num_rotations, int num_ticks,
                      time_t period_end, time_t now);

#ifdef __cplusplus
}
#endif
