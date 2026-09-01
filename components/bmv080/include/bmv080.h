#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BMV080_I2C_ADDR 0x54

typedef struct {
    float pm2_5; // Particulate Matter 2.5 (ug/m3)
} bmv080_data_t;

/**
 * @brief Enciende el láser óptico del BMV080 y arranca el conteo.
 */
esp_err_t bmv080_trigger_measurement(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Lee los conteos de partículas y apaga el láser para ahorrar energía.
 */
esp_err_t bmv080_read_measurement(i2c_master_dev_handle_t dev_handle, bmv080_data_t *out_data);

#ifdef __cplusplus
}
#endif
