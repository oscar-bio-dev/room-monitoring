#include "bmv080_wrapper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bmv080_wrapper";
// Handle del SDK de Bosch
static bmv080_handle_t bmv080_handle = NULL;
// Handle del Bus I2C de ESP-IDF (cacheado para evadir bugs de contexto del SDK)
static i2c_master_dev_handle_t s_bmv080_i2c_dev = NULL;
static float                   last_pm25        = 0.0f;

// Tiempo máximo de espera para transacciones I2C
// Durante la descarga de firmware, el sensor hace clock-stretching para grabar en memoria.
// 100ms (10 ticks) demostró ser insuficiente (I2C software timeout a los 10 ticks exactos).
#define BMV080_I2C_TIMEOUT_MS 1000
#define BMV080_MAX_TRANSFER_WORDS 512

// Función de retardo (callback para Bosch SDK)
static int8_t bmv080_delay_ms(uint32_t period) {
    if (period == 0)
        return 0;
    uint32_t ticks = pdMS_TO_TICKS(period);
    if (ticks > 0) {
        vTaskDelay(ticks);
    } else {
        esp_rom_delay_us(period * 1000);
    }
    return 0; // 0 = E_COMBRIDGE_OK
}

// Wrapper I2C Read para el SDK de Bosch
static int8_t bmv080_i2c_read_16bit(bmv080_sercom_handle_t handle, uint16_t header, uint16_t *payload,
                                    uint16_t payload_length) {
    (void) handle;
    // IGNORAR el parámetro 'handle' que provee el SDK.
    // bmv080_serve_interrupt tiene un bug conocido donde pasa el contexto propio en vez del intf_ptr.
    i2c_master_dev_handle_t dev_handle = s_bmv080_i2c_dev;

    if (!dev_handle || (payload_length > 0 && !payload)) {
        ESP_LOGE(TAG, "FATAL: s_bmv080_i2c_dev IS NULL!");
        return -1;
    }
    if (payload_length > BMV080_MAX_TRANSFER_WORDS) {
        ESP_LOGE(TAG, "BMV080 read exceeds maximum payload: %u words", payload_length);
        return -1;
    }

    header = header << 1;

    uint8_t tx_buf[2] = {(uint8_t) (header >> 8), (uint8_t) (header & 0xFF)};

    ESP_LOGD(TAG, "Calling i2c_master_transmit...");
    esp_err_t err = i2c_master_transmit(dev_handle, tx_buf, 2, pdMS_TO_TICKS(BMV080_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_transmit failed: %s", esp_err_to_name(err));
        return -1;
    }

    if (payload_length == 0) {
        return 0;
    }

    uint16_t rx_len = payload_length * 2;
    uint8_t *rx_buf = (uint8_t *) malloc(rx_len);
    if (!rx_buf)
        return -1;

    // Receive payload (STOP/START condition respected)
    err = i2c_master_receive(dev_handle, rx_buf, rx_len, pdMS_TO_TICKS(BMV080_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE("BMV080_I2C", "Receive payload NACK!");
        free(rx_buf);
        return -1;
    }

    // Convert big endian wire format to native uint16_t
    for (uint16_t i = 0; i < payload_length; i++) {
        payload[i] = (rx_buf[i * 2] << 8) | rx_buf[i * 2 + 1];
    }

    free(rx_buf);

    // Bosch ASIC needs a tiny breath between back-to-back fast reads
    esp_rom_delay_us(2000); // 2ms busy-wait to guarantee delay regardless of RTOS tick rate

    return 0;
}

// Wrapper I2C Write para el SDK de Bosch
static int8_t bmv080_i2c_write_16bit(bmv080_sercom_handle_t handle, uint16_t header, const uint16_t *payload,
                                     uint16_t payload_length) {
    (void) handle;
    // IGNORAR el parámetro 'handle' (ver bmv080_i2c_read_16bit)
    i2c_master_dev_handle_t dev_handle = s_bmv080_i2c_dev;

    if (!dev_handle || (payload_length > 0 && !payload)) {
        ESP_LOGE(TAG, "Invalid BMV080 write context or payload");
        return -1;
    }
    if (payload_length > BMV080_MAX_TRANSFER_WORDS) {
        ESP_LOGE(TAG, "BMV080 write exceeds maximum payload: %u words", payload_length);
        return -1;
    }

    header = header << 1;

    uint16_t tx_len = 2 + (payload_length * 2);
    uint8_t *tx_buf = (uint8_t *) malloc(tx_len);
    if (!tx_buf)
        return -1;

    // Header MSB first
    tx_buf[0] = (uint8_t) (header >> 8);
    tx_buf[1] = (uint8_t) (header & 0xFF);

    for (uint16_t i = 0; i < payload_length; i++) {
        // Send MSB first for each native word
        tx_buf[2 + (i * 2)]     = (uint8_t) (payload[i] >> 8);
        tx_buf[2 + (i * 2) + 1] = (uint8_t) (payload[i] & 0xFF);
    }

    esp_err_t err = i2c_master_transmit(dev_handle, tx_buf, tx_len, pdMS_TO_TICKS(BMV080_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE("BMV080_I2C", "Write NACK! header=0x%04X", header);
    }

    free(tx_buf);

    // Bosch ASIC needs a tiny breath after writes
    esp_rom_delay_us(2000); // 2ms busy-wait to guarantee delay

    return (err == ESP_OK) ? 0 : -1;
}

// Callback de interrupción (cuando hay datos listos)
static void bmv080_data_ready_callback(bmv080_output_t bmv080_output, void *callback_parameters) {
    (void) callback_parameters;
    if (!bmv080_output.is_obstructed) {
        last_pm25 = bmv080_output.pm2_5_mass_concentration;
        ESP_LOGI(TAG, "Láser BMV080: PM2.5 = %.2f ug/m3", last_pm25);
    } else {
        ESP_LOGW(TAG, "Láser BMV080: Sensor obstruido o sucio");
    }
}

bmv080_status_code_t bmv080_wrapper_init(i2c_master_dev_handle_t i2c_dev_handle) {
    bmv080_status_code_t rslt = E_BMV080_ERROR_NULLPTR;

    if (!i2c_dev_handle) {
        ESP_LOGE(TAG, "Cannot initialize BMV080 without an I2C device handle");
        return E_BMV080_ERROR_NULLPTR;
    }

    // Guardar el handle I2C globalmente para los callbacks
    s_bmv080_i2c_dev = i2c_dev_handle;

    // Inicializar el Handle del BMV080 con reintentos en caso de NACK tras Deep Sleep
    for (int retry = 0; retry < 3; retry++) {
        bmv080_handle = NULL; // Prevenir error 180 (NULLPTR) en el SDK de Bosch
        rslt          = bmv080_open(&bmv080_handle, (bmv080_sercom_handle_t) i2c_dev_handle, bmv080_i2c_read_16bit,
                                    bmv080_i2c_write_16bit, bmv080_delay_ms);
        if (rslt == E_BMV080_OK) {
            break;
        }
        ESP_LOGW(TAG, "bmv080_open NACK/Error %d. Retrying %d/3...", rslt, retry + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error bmv080_open FATAL: %d", rslt);
        s_bmv080_i2c_dev = NULL;
        return rslt;
    }

    // Reset de fábrica
    rslt = bmv080_reset(bmv080_handle);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error bmv080_reset: %d", rslt);
        bmv080_close(&bmv080_handle);
        bmv080_handle    = NULL;
        s_bmv080_i2c_dev = NULL;
        return rslt;
    }

    // Obtener versión y sensor ID (Validación)
    char id[13] = {0};
    rslt        = bmv080_get_sensor_id(bmv080_handle, id);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error reading BMV080 sensor ID: %d", rslt);
        bmv080_close(&bmv080_handle);
        bmv080_handle    = NULL;
        s_bmv080_i2c_dev = NULL;
        return rslt;
    }
    ESP_LOGI(TAG, "BMV080 Detectado. ID: %s", id);

    // Iniciar medición continua
    rslt = bmv080_start_continuous_measurement(bmv080_handle);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error start_continuous_measurement: %d", rslt);
        bmv080_close(&bmv080_handle);
        bmv080_handle    = NULL;
        s_bmv080_i2c_dev = NULL;
        return rslt;
    }

    return E_BMV080_OK;
}

int bmv080_wrapper_read_pm25(float *pm25_out) {
    if (!bmv080_handle || !pm25_out)
        return -1;

    // Ejecutar el handler de interrupción manual (polling state machine)
    bmv080_status_code_t rslt = bmv080_serve_interrupt(bmv080_handle, bmv080_data_ready_callback, NULL);

    if (rslt == E_BMV080_OK)
        *pm25_out = last_pm25;

    return (rslt == E_BMV080_OK) ? 0 : -1;
}

void bmv080_wrapper_deinit(void) {
    if (bmv080_handle) {
        bmv080_stop_measurement(bmv080_handle);
        bmv080_close(&bmv080_handle);
        bmv080_handle = NULL;
        ESP_LOGI(TAG, "BMV080 measurement stopped and handle closed.");
    }
    s_bmv080_i2c_dev = NULL;
}
