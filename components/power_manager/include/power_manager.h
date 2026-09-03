#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Estados de la máquina de Doble Despertar
typedef enum {
    PM_STATE_WAKE_A = 0, // Disparo de sensores ópticos/láser
    PM_STATE_WAKE_B = 1  // Recolección y transmisión
} power_wake_state_t;

/**
 * @brief Obtiene el estado actual del ciclo de Doble Despertar.
 */
power_wake_state_t power_manager_get_wake_state(void);

/**
 * @brief Entra en Light Sleep para WAKE_A o Deep Sleep con pines aislados para WAKE_B.
 *        WAKE_A conserva el contexto I2C y FreeRTOS durante el micro-sleep de 4.85s.
 *        WAKE_B programa el sleep principal (3s en calibración, 5 mins en normal).
 */
void power_manager_execute_sleep_cycle(void);

/**
 * @brief Habilita o deshabilita el modo de calibración (ciclo rápido de 3s)
 */
void power_manager_set_calibration_mode(bool active);

/**
 * @brief Retorna true si el sistema está en modo de calibración
 */
bool power_manager_is_calibration_mode(void);

#ifdef __cplusplus
}
#endif
