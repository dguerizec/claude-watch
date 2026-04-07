#include "usage_store.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "usage_store";

#define MOUNT_POINT "/sdcard"

static spi_host_device_t s_spi_host;
static int s_cs_gpio;
static bool s_configured = false;

esp_err_t usage_store_init(spi_host_device_t spi_host, int cs_gpio)
{
    s_spi_host = spi_host;
    s_cs_gpio = cs_gpio;
    s_configured = true;
    ESP_LOGI(TAG, "SD card configured on SPI%d, CS=GPIO%d (lazy mount)", spi_host + 1, cs_gpio);
    return ESP_OK;
}

esp_err_t usage_store_append(float five_hour, float seven_day)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;

    /* Mount SD card — only while writing to avoid SPI bus conflict with LCD.
     * Two SPI devices on the same bus cause an ISR race condition in the
     * ESP-IDF SPI master driver (espressif/esp-idf#17860). */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_spi_host;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = s_cs_gpio;
    slot_config.host_id = s_spi_host;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s — skipping", esp_err_to_name(ret));
        return ret;
    }

    /* Build path */
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    int mr = mkdir(MOUNT_POINT "/ccusage", 0775);
    if (mr != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir failed: errno=%d", errno);
    }

    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/ccusage/usage_%04d-%02d.csv",
             local.tm_year + 1900, local.tm_mon + 1);

    struct stat st;
    bool new_file = (stat(path, &st) != 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        return ESP_FAIL;
    }

    if (new_file) {
        fprintf(f, "timestamp,five_hour,seven_day\n");
    }

    fprintf(f, "%ld,%.1f,%.1f\n", (long)now, five_hour, seven_day);
    fclose(f);

    ESP_LOGI(TAG, "Stored: %.1f%% / %.1f%% → %s", five_hour, seven_day, path);

    /* Unmount immediately — keeps SD SPI device off the bus so LCD works */
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    return ESP_OK;
}
