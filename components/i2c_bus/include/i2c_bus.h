#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pines estandarizados del bus I2C para nuestra PCB
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22

/**
 * @brief Inicializa el bus I2C maestro y ejecuta la rutina de recuperación
 *        (Sanity Check de 9 pulsos) en caso de que un esclavo esté bloqueado.
 *
 * @param bus_handle Puntero al manejador del bus para ser poblado.
 * @return esp_err_t ESP_OK en éxito, o un código de error.
 */
esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_handle);

#ifdef __cplusplus
}
#endif
