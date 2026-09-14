#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Modos de Energía Seleccionables por el Usuario (vía HMI/Downlink)
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    PM_MODE_5_SEC = 0, // Transmisión cada ~5 segundos  (Light-Sleep entre ciclos)
    PM_MODE_1_MIN = 1, // Transmisión cada ~60 segundos (Deep-Sleep 55s + 5s Light)
    PM_MODE_5_MIN = 2  // Transmisión cada ~300 segundos (Deep-Sleep 295s + 5s Light) [DEFAULT]
} power_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Modos de Muestreo del BME688/BSEC
 *
 * BSEC_CONTINUOUS (1 Hz)  → Fase 1 (12 pulsos): Sub-bucle de 1s alimentando BSEC.
 * BSEC_ULP       (300s)   → Fase 2 MODE_5_MIN: Match nativo perfecto.
 * RAW_FORCED               → Fase 2 MODE_5_SEC / MODE_1_MIN: Sin BSEC, lectura directa.
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    BME_MODE_BSEC_CONTINUOUS = 0, // BSEC a 1 Hz (Fase 1 calibración)
    BME_MODE_BSEC_ULP        = 1, // BSEC ULP a 300s (Fase 2 default)
    BME_MODE_RAW_FORCED      = 2  // Sin BSEC, Forced Mode directo (Fase 2 5s/1min)
} bme_sampling_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Estados internos de la Máquina de Doble Despertar
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    PM_STATE_WAKE_A = 0, // Disparo simultáneo de sensores
    PM_STATE_WAKE_B = 1  // Recolección de datos, empaquetado Protobuf y transmisión
} power_wake_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Estructura de Configuración del Nodo (Persistente en RTC RAM)
 * Sobrevive al Deep Sleep. Modificable por downlink ESP-NOW.
 * ──────────────────────────────────────────────────────────────────────────── */
typedef struct {
    power_mode_t        mode;                    // Modo de energía actual
    bme_sampling_mode_t bme_mode;                // Modo de muestreo del BME688
    uint8_t             warmup_cycles_remaining; // Ciclos de calentamiento pendientes (Fase 1)
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

/**
 * @brief Obtiene el estado actual del ciclo de Doble Despertar (WAKE_A o WAKE_B).
 */
power_wake_state_t power_manager_get_wake_state(void);

/**
 * @brief Obtiene un puntero de solo lectura a la configuración actual del nodo.
 */
const node_config_t *power_manager_get_config(void);

/**
 * @brief Cambia el modo de energía del nodo. Actualiza automáticamente el bme_mode.
 */
void power_manager_set_mode(power_mode_t mode);

/**
 * @brief Ejecuta la política de sueño según el estado actual de la FSM.
 */
void power_manager_execute_sleep_cycle(void);

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
