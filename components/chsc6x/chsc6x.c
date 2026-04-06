#include <stdlib.h>
#include <string.h>
#include "chsc6x.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "chsc6x";

#define CHSC6X_ADDR 0x2E
#define CHSC6X_READ_LEN 5

struct chsc6x_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    int int_gpio_num;
};

esp_err_t chsc6x_init(const chsc6x_config_t *config, chsc6x_handle_t *handle)
{
    struct chsc6x_dev_t *dev = calloc(1, sizeof(struct chsc6x_dev_t));
    if (!dev) return ESP_ERR_NO_MEM;

    dev->int_gpio_num = config->int_gpio_num;

    /* INT pin as input with pull-up (active LOW when touched) */
    if (dev->int_gpio_num >= 0) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << dev->int_gpio_num,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&int_cfg);
    }

    /* Add I2C device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CHSC6X_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(config->i2c_bus, &dev_cfg, &dev->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add I2C device: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    ESP_LOGI(TAG, "CHSC6X touch initialized (addr=0x%02X, INT=GPIO%d)", CHSC6X_ADDR, dev->int_gpio_num);
    *handle = dev;
    return ESP_OK;
}

esp_err_t chsc6x_read(chsc6x_handle_t handle, chsc6x_touch_data_t *data)
{
    memset(data, 0, sizeof(*data));

    /* Check INT pin — LOW means touch is active */
    if (handle->int_gpio_num >= 0 && gpio_get_level(handle->int_gpio_num) != 0) {
        return ESP_OK; /* No touch */
    }

    /* Read 5 bytes from the controller */
    uint8_t buf[CHSC6X_READ_LEN] = {0};
    esp_err_t ret = i2c_master_receive(handle->i2c_dev, buf, CHSC6X_READ_LEN, 50);
    if (ret != ESP_OK) {
        return ret;
    }

    /* buf[0] == 0x01 means valid touch */
    if (buf[0] == 0x01) {
        data->touched = true;
        data->x = 239 - buf[2];
        data->y = buf[4];
    }

    return ESP_OK;
}

esp_err_t chsc6x_del(chsc6x_handle_t handle)
{
    if (handle) {
        i2c_master_bus_rm_device(handle->i2c_dev);
        if (handle->int_gpio_num >= 0) gpio_reset_pin(handle->int_gpio_num);
        free(handle);
    }
    return ESP_OK;
}
