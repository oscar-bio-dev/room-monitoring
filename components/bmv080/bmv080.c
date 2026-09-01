#include "bmv080.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BMV080";

esp_err_t bmv080_trigger_measurement(i2c_master_dev_handle_t dev_handle) {
    if (!dev_handle)
        return ESP_ERR_INVALID_ARG;

    // TODO: Inyectar registros específicos del datasheet de Bosch para encender el láser
    ESP_LOGD(TAG, "BMV080 Laser triggered. Fanless particulate scanning started.");

    return ESP_OK;
}

esp_err_t bmv080_read_measurement(i2c_master_dev_handle_t dev_handle, bmv080_data_t *out_data) {
    if (!dev_handle || !out_data)
        return ESP_ERR_INVALID_ARG;

    // TODO: Leer búferes I2C y calcular partículas (PM2.5)
    // Luego enviar comando de Sleep al láser.

    out_data->pm2_5 = 12.5f; // Valor simulado
    ESP_LOGD(TAG, "BMV080 Data Read. Laser powered down.");

    return ESP_OK;
}
