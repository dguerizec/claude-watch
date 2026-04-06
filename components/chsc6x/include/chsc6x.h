#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool touched;
    uint16_t x;
    uint16_t y;
} chsc6x_touch_data_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    int int_gpio_num;   /* Touch interrupt GPIO (active LOW when touched) */
} chsc6x_config_t;

typedef struct chsc6x_dev_t *chsc6x_handle_t;

esp_err_t chsc6x_init(const chsc6x_config_t *config, chsc6x_handle_t *handle);
esp_err_t chsc6x_read(chsc6x_handle_t handle, chsc6x_touch_data_t *data);
esp_err_t chsc6x_del(chsc6x_handle_t handle);

#ifdef __cplusplus
}
#endif
