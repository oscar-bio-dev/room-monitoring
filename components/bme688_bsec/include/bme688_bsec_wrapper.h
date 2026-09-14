#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el sensor de gas BME688 y el motor BSEC 3.0 (IAQ).
 *
 * @param i2c_dev_handle Handle I2C del dispositivo (0x76 o 0x77)
 * @param rv_dev_handle Handle I2C del RTC RV-1805
 * @param sample_rate Tasa de muestreo BSEC (ej. BSEC_SAMPLE_RATE_CONT=1.0f,
 *                    BSEC_SAMPLE_RATE_LP=0.33f, BSEC_SAMPLE_RATE_ULP=0.0033f)
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bme688_bsec_init(i2c_master_dev_handle_t i2c_dev_handle, i2c_master_dev_handle_t rv_dev_handle,
                        float sample_rate);

/**
 * @brief Reconfigura las suscripciones BSEC con una nueva tasa de muestreo.
 * @param sample_rate Nueva tasa de muestreo (ej. BSEC_SAMPLE_RATE_ULP)
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bme688_bsec_set_sample_rate(float sample_rate);

/**
 * @brief Ejecuta un ciclo completo de BSEC: sensor_control → medición → do_steps.
 *
 * @return int8_t 0 (nueva medición), -1 (error), -2 (BSEC dice "no medir aún")
 */
int8_t bme688_bsec_read_iaq(float *iaq, uint8_t *accuracy, float *temperature, float *humidity, float *pressure,
                            float *gas_resistance);

/**
 * @brief Lectura directa en Forced Mode (sin BSEC). Obtiene T/P/H/Gas crudos.
 *        Usar cuando el intervalo de muestreo no coincide con LP ni ULP.
 *
 * @return int8_t 0 (Éxito) o -1 (Error)
 */
int8_t bme688_raw_forced_read(float *temperature, float *humidity, float *pressure, float *gas_resistance);

#ifdef __cplusplus
}
#endif
