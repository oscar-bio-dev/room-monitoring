#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el sensor de gas BME688 y el motor BSEC 3.0 (IAQ)
 *
 * @param i2c_dev_handle Handle I2C del dispositivo configurado (0x76 o 0x77)
 * @param rv_dev_handle Handle I2C del RTC RV-1805
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bme688_bsec_init(i2c_master_dev_handle_t i2c_dev_handle, i2c_master_dev_handle_t rv_dev_handle);

/**
 * @brief Obtiene la última lectura del Índice de Calidad del Aire (IAQ)
 *
 * @param iaq Puntero donde se almacenará el IAQ actual (0-500)
 * @param accuracy Puntero para la precisión del estado del algoritmo (0-3)
 * @param temperature Puntero para la temperatura compensada (opcional)
 * @param humidity Puntero para la humedad compensada (opcional)
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bme688_bsec_read_iaq(float *iaq, uint8_t *accuracy, float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif
