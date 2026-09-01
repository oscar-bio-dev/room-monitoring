#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Función de lectura I2C compatible con las APIs oficiales de Bosch (BSEC y BMV080-Sensor-API)
 *
 * @param reg_addr Registro de origen para iniciar la lectura
 * @param reg_data Puntero al buffer donde almacenar los datos leídos
 * @param len Cantidad de bytes a leer
 * @param intf_ptr Puntero genérico a la interfaz (se castea a i2c_master_dev_handle_t)
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bosch_hal_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr);

/**
 * @brief Función de escritura I2C compatible con las APIs oficiales de Bosch
 *
 * @param reg_addr Registro de destino para escribir
 * @param reg_data Puntero al buffer con los datos a enviar
 * @param len Cantidad de bytes a enviar (excluyendo la dirección del registro)
 * @param intf_ptr Puntero genérico a la interfaz (se castea a i2c_master_dev_handle_t)
 * @return int8_t 0 (Éxito) o distinto de 0 (Error)
 */
int8_t bosch_hal_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr);

/**
 * @brief Retardo bloqueante/pasivo en microsegundos compatible con APIs de Bosch
 *
 * Evita bloqueos activos si period_us >= 1000us cediendo el control a FreeRTOS,
 * logrando eficiencia Zero-CPU.
 *
 * @param period_us Tiempo en microsegundos
 * @param intf_ptr Puntero genérico a la interfaz (normalmente sin uso para delays)
 */
void bosch_hal_delay_us(uint32_t period_us, void *intf_ptr);

#ifdef __cplusplus
}
#endif
