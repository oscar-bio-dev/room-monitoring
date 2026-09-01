#include "bmv080_wrapper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char     *TAG           = "bmv080_wrapper";
static bmv080_handle_t bmv080_handle = NULL;
static float           last_pm25     = 0.0f;

// Tiempo máximo de espera para transacciones I2C
#define BMV080_I2C_TIMEOUT_MS 100

// Callback de retardo para la API de Bosch
static int8_t bmv080_delay_ms(uint32_t period) {
    vTaskDelay(pdMS_TO_TICKS(period));
    return 0; // 0 = E_COMBRIDGE_OK
}

// Callback de lectura I2C (16-bit header, 16-bit payload little endian)
static int8_t bmv080_i2c_read_16bit(bmv080_sercom_handle_t handle, uint16_t header, uint16_t *payload,
                                    uint16_t payload_length) {
    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t) handle;

    // Header shift left by 1 (R/W bit in hardware I2C handled by ESP-IDF, but API design expects this shift for
    // internal register maps)
    uint16_t header_adjusted = header << 1;
    uint8_t  tx_buf[2]       = {(uint8_t) (header_adjusted >> 8), (uint8_t) (header_adjusted & 0xFF)};

    uint16_t rx_len = payload_length * 2;
    uint8_t *rx_buf = (uint8_t *) malloc(rx_len);
    if (!rx_buf)
        return -1;

    esp_err_t err =
        i2c_master_transmit_receive(dev_handle, tx_buf, 2, rx_buf, rx_len, pdMS_TO_TICKS(BMV080_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        free(rx_buf);
        return -1;
    }

    // Convert big endian wire format to little endian payload
    for (uint16_t i = 0; i < payload_length; i++) {
        uint16_t word = (rx_buf[i * 2] << 8) | rx_buf[i * 2 + 1];
        payload[i]    = ((word << 8) | (word >> 8)) & 0xFFFF; // Swap bytes
    }

    free(rx_buf);
    return 0;
}

// Callback de escritura I2C (16-bit header, 16-bit payload little endian)
static int8_t bmv080_i2c_write_16bit(bmv080_sercom_handle_t handle, uint16_t header, const uint16_t *payload,
                                     uint16_t payload_length) {
    i2c_master_dev_handle_t dev_handle = (i2c_master_dev_handle_t) handle;

    uint16_t header_adjusted = header << 1;

    uint16_t tx_len = 2 + (payload_length * 2);
    uint8_t *tx_buf = (uint8_t *) malloc(tx_len);
    if (!tx_buf)
        return -1;

    tx_buf[0] = (uint8_t) (header_adjusted >> 8);
    tx_buf[1] = (uint8_t) (header_adjusted & 0xFF);

    for (uint16_t i = 0; i < payload_length; i++) {
        // Swap bytes from little endian to big endian for wire format
        uint16_t swapped        = ((payload[i] << 8) | (payload[i] >> 8)) & 0xFFFF;
        tx_buf[2 + (i * 2)]     = (uint8_t) (swapped >> 8);
        tx_buf[2 + (i * 2) + 1] = (uint8_t) (swapped & 0xFF);
    }

    esp_err_t err = i2c_master_transmit(dev_handle, tx_buf, tx_len, pdMS_TO_TICKS(BMV080_I2C_TIMEOUT_MS));
    free(tx_buf);

    return (err == ESP_OK) ? 0 : -1;
}

// Callback de interrupción (cuando hay datos listos)
static void bmv080_data_ready_callback(bmv080_output_t bmv080_output, void *callback_parameters) {
    if (!bmv080_output.is_obstructed) {
        last_pm25 = bmv080_output.pm2_5_mass_concentration;
        ESP_LOGI(TAG, "Láser BMV080: PM2.5 = %.2f ug/m3", last_pm25);
    } else {
        ESP_LOGW(TAG, "Láser BMV080: Sensor obstruido o sucio");
    }
}

bmv080_status_code_t bmv080_wrapper_init(i2c_master_dev_handle_t i2c_dev_handle) {
    bmv080_status_code_t rslt;

    // Inicializar el Handle del BMV080
    rslt = bmv080_open(&bmv080_handle, (bmv080_sercom_handle_t) i2c_dev_handle, bmv080_i2c_read_16bit,
                       bmv080_i2c_write_16bit, bmv080_delay_ms);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error bmv080_open: %d", rslt);
        return rslt;
    }

    // Reset de fábrica
    rslt = bmv080_reset(bmv080_handle);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error bmv080_reset: %d", rslt);
        return rslt;
    }

    // Obtener versión y sensor ID (Validación)
    char id[13] = {0};
    rslt        = bmv080_get_sensor_id(bmv080_handle, id);
    if (rslt == E_BMV080_OK) {
        ESP_LOGI(TAG, "BMV080 Detectado. ID: %s", id);
    }

    // Iniciar medición continua
    rslt = bmv080_start_continuous_measurement(bmv080_handle);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error start_continuous_measurement: %d", rslt);
        return rslt;
    }

    return E_BMV080_OK;
}

bmv080_status_code_t bmv080_wrapper_read_pm25(float *pm2_5) {
    if (!bmv080_handle)
        return E_BMV080_ERROR_NULLPTR;

    // Ejecutar el handler de interrupción manual (polling state machine)
    bmv080_status_code_t rslt = bmv080_serve_interrupt(bmv080_handle, bmv080_data_ready_callback, NULL);

    if (rslt == E_BMV080_OK && pm2_5 != NULL) {
        *pm2_5 = last_pm25;
    }

    return rslt;
}
