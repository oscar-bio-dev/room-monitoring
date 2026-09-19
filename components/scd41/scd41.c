#include "scd41.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SCD41";

static uint8_t scd41_crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }

    return crc;
}

esp_err_t scd41_trigger_single_shot(i2c_master_dev_handle_t dev_handle) {
    if (!dev_handle)
        return ESP_ERR_INVALID_ARG;

    // Comando 0x219D (Measure Single Shot)
    uint8_t   cmd[2] = {0x21, 0x9D};
    esp_err_t err    = i2c_master_transmit(dev_handle, cmd, 2, 1000 / portTICK_PERIOD_MS);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "SCD41 Single-Shot Triggered");
    } else {
        ESP_LOGE(TAG, "Failed to trigger SCD41: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t scd41_read_measurement(i2c_master_dev_handle_t dev_handle, scd41_data_t *out_data) {
    if (!dev_handle || !out_data)
        return ESP_ERR_INVALID_ARG;

    // Comando 0xEC05 (Read Measurement)
    uint8_t cmd[2]    = {0xEC, 0x05};
    uint8_t rx_buf[9] = {0}; // 3 words (16-bit) + 3 CRC bytes

    esp_err_t err = i2c_master_transmit_receive(dev_handle, cmd, 2, rx_buf, 9, 1000 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SCD41 data: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t word = 0; word < 3; word++) {
        const size_t offset = word * 3;
        if (scd41_crc8(&rx_buf[offset], 2) != rx_buf[offset + 2]) {
            ESP_LOGE(TAG, "Invalid CRC in SCD41 measurement word %u", (unsigned int) word);
            return ESP_ERR_INVALID_CRC;
        }
    }

    // Decodificar CO2
    out_data->co2 = (rx_buf[0] << 8) | rx_buf[1];

    // Decodificar Temperatura (Fórmula Datasheet Sensirion)
    uint16_t temp_raw     = (rx_buf[3] << 8) | rx_buf[4];
    out_data->temperature = -45.0f + 175.0f * (float) temp_raw / 65536.0f;

    // Decodificar Humedad
    uint16_t hum_raw   = (rx_buf[6] << 8) | rx_buf[7];
    out_data->humidity = 100.0f * (float) hum_raw / 65536.0f;

    return ESP_OK;
}

esp_err_t scd41_start_periodic_measurement(i2c_master_dev_handle_t dev_handle) {
    if (!dev_handle)
        return ESP_ERR_INVALID_ARG;

    // Comando 0x21B1 (Start Periodic Measurement)
    // El SCD41 medirá cada ~5 segundos internamente.
    uint8_t   cmd[2] = {0x21, 0xB1};
    esp_err_t err    = i2c_master_transmit(dev_handle, cmd, 2, 1000 / portTICK_PERIOD_MS);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SCD41 Periodic Measurement started (~5s interval)");
    } else {
        ESP_LOGE(TAG, "Failed to start periodic measurement: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t scd41_stop_periodic_measurement(i2c_master_dev_handle_t dev_handle) {
    if (!dev_handle)
        return ESP_ERR_INVALID_ARG;

    // Comando 0x3F86 (Stop Periodic Measurement)
    uint8_t   cmd[2] = {0x3F, 0x86};
    esp_err_t err    = i2c_master_transmit(dev_handle, cmd, 2, 1000 / portTICK_PERIOD_MS);

    if (err == ESP_OK) {
        // Datasheet: esperar 500ms antes de enviar otro comando
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "SCD41 Periodic Measurement stopped");
    } else {
        ESP_LOGE(TAG, "Failed to stop periodic measurement: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t scd41_get_data_ready(i2c_master_dev_handle_t dev_handle, bool *ready) {
    if (!dev_handle || !ready)
        return ESP_ERR_INVALID_ARG;

    // Comando 0xE4B8 (Get Data Ready Status)
    uint8_t cmd[2]    = {0xE4, 0xB8};
    uint8_t rx_buf[3] = {0}; // 1 word (16-bit) + 1 CRC byte

    esp_err_t err = i2c_master_transmit_receive(dev_handle, cmd, 2, rx_buf, 3, 1000 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get data ready status: %s", esp_err_to_name(err));
        *ready = false;
        return err;
    }

    if (scd41_crc8(rx_buf, 2) != rx_buf[2]) {
        ESP_LOGE(TAG, "Invalid CRC in data ready status");
        *ready = false;
        return ESP_ERR_INVALID_CRC;
    }

    // Datasheet: si los 11 bits inferiores del word son distintos de 0, hay datos listos
    uint16_t status = (rx_buf[0] << 8) | rx_buf[1];
    *ready          = (status & 0x07FF) != 0;

    return ESP_OK;
}
