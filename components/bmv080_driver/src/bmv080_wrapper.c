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

/* ── Última lectura completa (actualizada por el callback) ──────────────── */
static bmv080_reading_t s_last_reading = {0};

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

/* ────────────────────────────────────────────────────────────────────────────
 * Callback: captura TODOS los campos de bmv080_output_t
 * Fuente: BST-BMV080-DS000-10, §5.2.1.3.1
 * ──────────────────────────────────────────────────────────────────────────── */
static void bmv080_data_ready_callback(bmv080_output_t bmv080_output, void *callback_parameters) {
    (void) callback_parameters;

    /* Masa (µg/m³) */
    s_last_reading.pm1_mass   = bmv080_output.pm1_mass_concentration;
    s_last_reading.pm2_5_mass = bmv080_output.pm2_5_mass_concentration;
    s_last_reading.pm10_mass  = bmv080_output.pm10_mass_concentration;

    /* Conteo (particles/m³) */
    s_last_reading.pm1_count   = bmv080_output.pm1_number_concentration;
    s_last_reading.pm2_5_count = bmv080_output.pm2_5_number_concentration;
    s_last_reading.pm10_count  = bmv080_output.pm10_number_concentration;

    /* Estado de hardware */
    s_last_reading.is_obstructed    = bmv080_output.is_obstructed;
    s_last_reading.is_outside_range = bmv080_output.is_outside_measurement_range;
    s_last_reading.runtime_sec      = bmv080_output.runtime_in_sec;

    if (!bmv080_output.is_obstructed) {
        ESP_LOGI(TAG,
                 "Láser BMV080: PM1=%.2f | PM2.5=%.2f | PM10=%.2f (ug/m3) | "
                 "#1=%.0f | #2.5=%.0f | #10=%.0f (/m3)",
                 s_last_reading.pm1_mass, s_last_reading.pm2_5_mass, s_last_reading.pm10_mass, s_last_reading.pm1_count,
                 s_last_reading.pm2_5_count, s_last_reading.pm10_count);
    } else {
        ESP_LOGW(TAG, "⚠️ Láser BMV080: Sensor OBSTRUIDO (lente sucia o bloqueada)");
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Init: open + reset + configure (Cold Boot only)
 * ──────────────────────────────────────────────────────────────────────────── */
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

    // Habilitar detección de obstrucción (lente sucia/bloqueada)
    // BST-BMV080-DS000-10, §5.2.6.2: "do_obstruction_detection" = bool
    bool obstruction_on = true;
    rslt                = bmv080_set_parameter(bmv080_handle, "do_obstruction_detection", &obstruction_on);
    if (rslt != E_BMV080_OK) {
        ESP_LOGW(TAG, "No se pudo habilitar detección de obstrucción: %d (continuando)", rslt);
    } else {
        ESP_LOGI(TAG, "BMV080 obstruction detection enabled");
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

    // Limpiar última lectura
    memset(&s_last_reading, 0, sizeof(s_last_reading));

    return E_BMV080_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Start: iniciar medición continua (usar tras init o tras Light-Sleep)
 * ──────────────────────────────────────────────────────────────────────────── */
bmv080_status_code_t bmv080_wrapper_start(void) {
    if (!bmv080_handle) {
        ESP_LOGE(TAG, "Cannot start: bmv080_handle is NULL (call init first)");
        return E_BMV080_ERROR_NULLPTR;
    }

    bmv080_status_code_t rslt = bmv080_start_continuous_measurement(bmv080_handle);
    if (rslt != E_BMV080_OK) {
        ESP_LOGE(TAG, "Error start_continuous_measurement: %d", rslt);
    }
    return rslt;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Stop: detener medición (láser a sleep < 30 µA, handle sobrevive)
 * ──────────────────────────────────────────────────────────────────────────── */
void bmv080_wrapper_stop(void) {
    if (bmv080_handle) {
        bmv080_stop_measurement(bmv080_handle);
        ESP_LOGI(TAG, "BMV080 measurement stopped (handle retained for Light-Sleep).");
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Read (backward-compatible): solo masa
 * ──────────────────────────────────────────────────────────────────────────── */
int bmv080_wrapper_read_data(float *pm1_out, float *pm25_out, float *pm10_out) {
    if (!bmv080_handle) {
        return -1;
    }

    // Ejecutar el handler de interrupción manual (polling state machine)
    bmv080_status_code_t rslt = bmv080_serve_interrupt(bmv080_handle, bmv080_data_ready_callback, NULL);

    if (rslt == E_BMV080_OK) {
        if (pm1_out)
            *pm1_out = s_last_reading.pm1_mass;
        if (pm25_out)
            *pm25_out = s_last_reading.pm2_5_mass;
        if (pm10_out)
            *pm10_out = s_last_reading.pm10_mass;
    }

    return (rslt == E_BMV080_OK) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Read Full: masa + conteo + estado de hardware
 * ──────────────────────────────────────────────────────────────────────────── */
int bmv080_wrapper_read_full(bmv080_reading_t *reading) {
    if (!bmv080_handle || !reading) {
        return -1;
    }

    bmv080_status_code_t rslt = bmv080_serve_interrupt(bmv080_handle, bmv080_data_ready_callback, NULL);

    if (rslt == E_BMV080_OK) {
        *reading = s_last_reading;
    }

    return (rslt == E_BMV080_OK) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Deinit: cierre completo (solo para shutdown o error fatal)
 * ──────────────────────────────────────────────────────────────────────────── */
void bmv080_wrapper_deinit(void) {
    if (bmv080_handle) {
        bmv080_stop_measurement(bmv080_handle);
        bmv080_close(&bmv080_handle);
        bmv080_handle = NULL;
        ESP_LOGI(TAG, "BMV080 measurement stopped and handle closed.");
    }
    s_bmv080_i2c_dev = NULL;
}
