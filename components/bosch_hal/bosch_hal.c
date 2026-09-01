#include "bosch_hal.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "bosch_hal";

// Tiempo máximo de espera para transacciones I2C asíncronas
#define BOSCH_HAL_I2C_TIMEOUT_MS 100

int8_t bosch_hal_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    if (!intf_ptr || !reg_data) {
        return -1;
    }

    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t) intf_ptr;

    // Utilizamos la API de ESP-IDF v5 para transmisión atómica
    esp_err_t err =
        i2c_master_transmit_receive(dev_handle, &reg_addr, 1, reg_data, len, pdMS_TO_TICKS(BOSCH_HAL_I2C_TIMEOUT_MS));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C Read Error at reg 0x%02x: %s", reg_addr, esp_err_to_name(err));
        return -1; // Bosch API espera 0 en éxito (BME68X_OK / BMV080_OK)
    }

    return 0;
}

int8_t bosch_hal_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    if (!intf_ptr || !reg_data) {
        return -1;
    }

    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t) intf_ptr;

    // Las APIs de Bosch requieren transmisión continua (Registro + Payload).
    // Usamos un búfer estático VLA-like en stack para evitar malloc() en rutas calientes.
    uint8_t buf[32];
    if (len > sizeof(buf) - 1) {
        ESP_LOGE(TAG, "I2C Write Error: Payload too large for static HAL buffer (%lu)", len);
        return -1;
    }

    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);

    esp_err_t err = i2c_master_transmit(dev_handle, buf, len + 1, pdMS_TO_TICKS(BOSCH_HAL_I2C_TIMEOUT_MS));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C Write Error at reg 0x%02x: %s", reg_addr, esp_err_to_name(err));
        return -1;
    }

    return 0;
}

void bosch_hal_delay_us(uint32_t period_us, void *intf_ptr) {
    // Cumplimiento estricto del AGENTS.md (Bloqueos activos MUST evitarse).
    if (period_us >= 1000) {
        // >= 1 milisegundo: Ceder núcleo de vuelta al scheduler RTOS
        vTaskDelay(pdMS_TO_TICKS(period_us / 1000));
    } else {
        // < 1 milisegundo: Bloqueo de ROM (El yield RTOS no tiene precisión microsegundo)
        esp_rom_delay_us(period_us);
    }
}
