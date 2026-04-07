#pragma once

#include <time.h>
#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t timestamp;
    float value;  /* usage percentage 0–100 */
} usage_data_point_t;

/**
 * Configure SD card SPI parameters (lazy mount — no SPI activity here).
 */
esp_err_t usage_store_init(spi_host_device_t spi_host, int cs_gpio);

/**
 * Append a usage data point to the monthly CSV file.
 * Mounts SD, writes, unmounts.
 */
esp_err_t usage_store_append(float five_hour, float seven_day);

/**
 * Read historical data points from SD card CSV files.
 * Returns the seven_day values for timestamps in [from, to].
 * Mounts SD, reads, unmounts.
 *
 * @param from       Start of time range (epoch)
 * @param to         End of time range (epoch)
 * @param buf        Output buffer for data points
 * @param max_points Maximum number of points to read
 * @return Number of points read, or 0 on error
 */
int usage_store_read(time_t from, time_t to, usage_data_point_t *buf, int max_points);

#ifdef __cplusplus
}
#endif
