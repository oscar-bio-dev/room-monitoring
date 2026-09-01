#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCD41_I2C_ADDR 0x62

typedef struct {
    uint16_t co2;         // ppm
    float    temperature; // Celsius
    float    humidity;    // Relative Humidity %
} scd41_data_t;

/**
 * @brief Envia el comando (0x219D) de Single-Shot al SCD41.
 *        Tarda exactamente 5.0 segundos en finalizar la medición.
 */
esp_err_t scd41_trigger_single_shot(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Lee la medición tras completarse el Single-Shot.
 */
esp_err_t scd41_read_measurement(i2c_master_dev_handle_t dev_handle, scd41_data_t *out_data);

#ifdef __cplusplus
}
#endif
