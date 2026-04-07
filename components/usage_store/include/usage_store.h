#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SD card on the given SPI bus and mount FAT filesystem.
 * The SPI bus must already be initialized.
 */
esp_err_t usage_store_init(spi_host_device_t spi_host, int cs_gpio);

/**
 * Append a usage data point to the monthly CSV file.
 * File is named usage_YYYY-MM.csv (e.g. usage_2026-04.csv).
 * No-op if SD card is not mounted.
 */
esp_err_t usage_store_append(float five_hour, float seven_day);

#ifdef __cplusplus
}
#endif
