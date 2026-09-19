#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bmv080.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estructura completa de lectura del BMV080
 *
 * Contiene concentración de masa (µg/m³), concentración numérica
 * (particles/m³), flags de hardware y runtime del ciclo de medición.
 * Fuente: bmv080_output_t (BST-BMV080-DS000-10, §5.2.1.3.1)
 */
typedef struct {
    /* Concentración de Masa (µg/m³) */
    float pm1_mass;
    float pm2_5_mass;
    float pm10_mass;

    /* Concentración Numérica (particles/m³) */
    float pm1_count;
    float pm2_5_count;
    float pm10_count;

    /* Estado de Hardware */
    bool  is_obstructed;    /**< Sensor bloqueado — lente sucia/tapada */
    bool  is_outside_range; /**< PM2.5 fuera de rango especificado (0–1000 µg/m³) */
    float runtime_sec;      /**< Tiempo transcurrido desde inicio de medición (s) */
} bmv080_reading_t;

/**
 * @brief Inicializa el láser BMV080 (open + reset + configuración)
 *
 * Realiza bmv080_open(), bmv080_reset() y configura parámetros.
 * Solo necesario en Cold Boot o tras un error fatal.
 *
 * @param i2c_dev_handle Handle I2C del dispositivo configurado
 * @return bmv080_status_code_t Código de estado oficial de Bosch
 */
bmv080_status_code_t bmv080_wrapper_init(i2c_master_dev_handle_t i2c_dev_handle);

/**
 * @brief Inicia la medición continua del láser
 *
 * Llama a bmv080_start_continuous_measurement(). Usar tras bmv080_wrapper_init()
 * o tras un ciclo de Light-Sleep (el handle sobrevive en RAM).
 *
 * @return bmv080_status_code_t E_BMV080_OK si exitoso
 */
bmv080_status_code_t bmv080_wrapper_start(void);

/**
 * @brief Detiene la medición (láser a sleep, < 30 µA)
 *
 * Llama a bmv080_stop_measurement(). No cierra el handle.
 * El handle sobrevive Light-Sleep y puede reanudarse con bmv080_wrapper_start().
 */
void bmv080_wrapper_stop(void);

/**
 * @brief Obtiene la última lectura de PM (solo masa, backward-compatible)
 *
 * @param pm1_out  Puntero PM1.0 (µg/m³), puede ser NULL
 * @param pm25_out Puntero PM2.5 (µg/m³), puede ser NULL
 * @param pm10_out Puntero PM10 (µg/m³), puede ser NULL
 * @return int 0 si exitoso, <0 si error
 */
int bmv080_wrapper_read_data(float *pm1_out, float *pm25_out, float *pm10_out);

/**
 * @brief Lee la medición completa del BMV080 (masa + conteo + estado)
 *
 * Llama a bmv080_serve_interrupt() y copia la última lectura completa.
 *
 * @param reading Puntero a estructura de salida (debe ser != NULL)
 * @return int 0 si exitoso, <0 si error
 */
int bmv080_wrapper_read_full(bmv080_reading_t *reading);

/**
 * @brief Detiene la medición y cierra completamente el handle del sensor
 *
 * Libera el handle de Bosch. Necesario solo antes de apagar por MOSFET
 * o en shutdown del sistema. NO necesario entre ciclos de Light-Sleep.
 */
void bmv080_wrapper_deinit(void);

#ifdef __cplusplus
}
#endif
