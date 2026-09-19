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
 *        Usar en MODE_5_MIN donde el sensor duerme entre ciclos.
 */
esp_err_t scd41_trigger_single_shot(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Lee la medición tras completarse el Single-Shot o Periodic.
 */
esp_err_t scd41_read_measurement(i2c_master_dev_handle_t dev_handle, scd41_data_t *out_data);

/**
 * @brief Inicia el modo de medición periódica (0x21B1, cada ~5s).
 *        El SCD41 mide continuamente y mantiene el último resultado disponible.
 *        Usar en MODE_5_SEC para alimentar el bucle continuo.
 */
esp_err_t scd41_start_periodic_measurement(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Detiene el modo de medición periódica (0x3F86).
 *        Debe llamarse antes de enviar otros comandos (ej. single-shot).
 *        Espera 500ms internamente como requiere el datasheet.
 */
esp_err_t scd41_stop_periodic_measurement(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Verifica si hay datos listos en modo periódico (0xE4B8).
 * @param[out] ready  true si hay una medición disponible para leer.
 */
esp_err_t scd41_get_data_ready(i2c_master_dev_handle_t dev_handle, bool *ready);

/**
 * @brief Establece la presión ambiental para compensación de CO2 (0xE000).
 *
 * Puede enviarse durante periodic measurement o antes de single-shot.
 * Sobrescribe cualquier compensación por altitud previa.
 * Fuente: SCD4x Datasheet v1.7, §3.7.5.
 *
 * @param dev_handle Handle I2C del SCD41
 * @param pressure_hpa Presión en hectopascales (700–1200 hPa).
 *                     Se envía como word = hPa (equivalente a Pa/100).
 * @return esp_err_t ESP_OK si el comando fue aceptado.
 */
esp_err_t scd41_set_ambient_pressure(i2c_master_dev_handle_t dev_handle, uint16_t pressure_hpa);

#ifdef __cplusplus
}
#endif
