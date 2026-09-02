#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "bmv080.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa el láser BMV080 utilizando la API oficial de Bosch
 *
 * @param i2c_dev_handle Handle I2C del dispositivo configurado (0x54)
 * @return bmv080_status_code_t Código de estado oficial de Bosch
 */
bmv080_status_code_t bmv080_wrapper_init(i2c_master_dev_handle_t i2c_dev_handle);

/**
 * @brief Obtiene la última lectura de PM2.5 del láser
 *
 * @param pm25_out Puntero donde se almacenará el resultado (ug/m3)
 * @return int 0 si fue exitoso, <0 si hubo error.
 */
int bmv080_wrapper_read_pm25(float *pm25_out);

/**
 * @brief Detiene la medición y cierra el handle del sensor
 */
void bmv080_wrapper_deinit(void);

#ifdef __cplusplus
}
#endif
