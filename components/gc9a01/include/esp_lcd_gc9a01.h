#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create LCD panel for GC9A01
 *
 * @param[in] io LCD panel IO handle
 * @param[in] panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
