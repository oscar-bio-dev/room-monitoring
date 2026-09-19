#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Modos de Energía Seleccionables por el Usuario (vía HMI/Downlink/CHG pin)
 *
 * Solo 2 modos de producción. El modo se puede cambiar dinámicamente
 * vía ESP-NOW downlink o detección del pin de carga (CHG).
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    PM_MODE_5_SEC = 0, // Continuo (~5s): BSEC Continuous 1Hz + SCD41 Periodic
    PM_MODE_5_MIN = 1  // Batería (~300s): BSEC ULP 300s + SCD41 Single-Shot [DEFAULT]
} power_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Modos de Muestreo del BME688/BSEC
 *
 * BSEC_CONTINUOUS (1 Hz) → Warmup (12 pulsos) Y Mode 5SEC producción.
 * BSEC_ULP       (300s)  → Mode 5MIN producción (match nativo perfecto).
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    BME_MODE_BSEC_CONTINUOUS = 0, // BSEC a 1 Hz (Warmup + MODE_5_SEC)
    BME_MODE_BSEC_ULP        = 1  // BSEC ULP a 300s (MODE_5_MIN)
} bme_sampling_mode_t;

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/* ────────────────────────────────────────────────────────────────────────────
 * Estados internos de la Máquina de Doble Despertar (legacy Deep Sleep)
 * Encapsulado para v2.0. No usado en producción Light-Sleep.
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    PM_STATE_WAKE_A = 0, // Disparo simultáneo de sensores
    PM_STATE_WAKE_B = 1  // Recolección de datos, empaquetado Protobuf y transmisión
} power_wake_state_t;
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Estructura de Configuración del Nodo
 *
 * En Light-Sleep: vive en RAM estática normal (sobrevive entre ciclos).
 * En Deep Sleep (legacy): marcada con RTC_DATA_ATTR en el .c.
 * ──────────────────────────────────────────────────────────────────────────── */
typedef struct {
    power_mode_t        mode;                    // Modo de energía actual
    bme_sampling_mode_t bme_mode;                // Modo de muestreo del BME688
    uint8_t             warmup_cycles_remaining; // Ciclos de calentamiento pendientes (Warmup)
    bool                is_calibrating;          // true durante los 12 pulsos iniciales
} node_config_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Constantes de Diseño
 * ──────────────────────────────────────────────────────────────────────────── */
#define PM_WARMUP_TOTAL_CYCLES 12   // 12 pulsos × 5s = 60s de calentamiento
#define PM_MICROSLEEP_US 4950000ULL // 4.95s Light-Sleep (SCD41 requiere 5s total)
#define PM_BSEC_TICK_US 1000000ULL  // 1s Light-Sleep para sub-bucle BSEC a 1 Hz

/* ────────────────────────────────────────────────────────────────────────────
 * API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/**
 * @brief Obtiene el estado actual del ciclo de Doble Despertar (WAKE_A o WAKE_B).
 *        Solo disponible en modo Deep Sleep (legacy v2.0).
 */
power_wake_state_t power_manager_get_wake_state(void);
#endif

/**
 * @brief Obtiene un puntero de solo lectura a la configuración actual del nodo.
 */
const node_config_t *power_manager_get_config(void);

/**
 * @brief Cambia el modo de energía del nodo. Actualiza automáticamente el bme_mode.
 */
void power_manager_set_mode(power_mode_t mode);

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/**
 * @brief Ejecuta la política de sueño según el estado actual de la FSM.
 *        Solo disponible en modo Deep Sleep (legacy v2.0).
 */
void power_manager_execute_sleep_cycle(void);
#endif

/**
 * @brief Decrementa el contador de warmup. Cuando llega a 0, transiciona a Fase 2.
 */
void power_manager_tick_warmup(void);

/**
 * @brief Retorna true si el sistema está en la Fase 1 de calibración.
 */
bool power_manager_is_calibrating(void);

#ifdef __cplusplus
}
#endif
