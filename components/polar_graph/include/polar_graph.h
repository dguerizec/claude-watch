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
 * The graph maps a 7-day period onto a full circle:
 *   - North (top) = period start and end
 *   - Clockwise winding
 *   - Center = 0%, edge = 100%
 *   - A radial "now" line separates current from stale data
 *
 * @param panel        LCD panel handle
 * @param points       Array of (timestamp, value) data points
 * @param num_points   Number of data points
 * @param period_start Start of the 7-day period (epoch)
 * @param now          Current time (epoch) — for the "now" hand
 */
void polar_graph_draw(esp_lcd_panel_handle_t panel,
                      const usage_data_point_t *points, int num_points,
                      time_t period_start, time_t now);

#ifdef __cplusplus
}
#endif
