#include "bme688_bsec_wrapper.h"
#include "bme68x.h"
#include "bsec_interface.h"
#include "bsec_datatypes.h"
#include "bosch_hal.h"
#include "esp_log.h"
#include <sys/time.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "rv1805_wrapper.h"

#ifdef CONFIG_ENABLE_DEEP_SLEEP
#include "esp_attr.h"
#endif

static const char *TAG = "bme688_bsec";

/* ────────────────────────────────────────────────────────────────────────────
 * Estado persistente BSEC
 *
 * Deep Sleep (legacy): RTC_DATA_ATTR para sobrevivir al reboot.
 * Light-Sleep: variables estáticas normales (RAM retenida).
 * ──────────────────────────────────────────────────────────────────────────── */
#ifdef CONFIG_ENABLE_DEEP_SLEEP
RTC_DATA_ATTR static uint8_t rtc_bsec_state[BSEC_MAX_STATE_BLOB_SIZE];
RTC_DATA_ATTR static bool    rtc_bsec_state_valid         = false;
RTC_DATA_ATTR static bool    rtc_bsec_ulp_established     = false;
RTC_DATA_ATTR static int64_t rtc_bsec_next_measurement_ns = 0;
#endif

static float s_current_sample_rate = 0.0f;

/* Último next_call nativo de BSEC (para Light-Sleep timer dinámico) */
static int64_t s_last_bsec_next_call_ns = 0;

static struct bme68x_dev       bme_dev;
static i2c_master_dev_handle_t rtc_dev;
static float                   current_iaq          = 0.0f;
static uint8_t                 current_iaq_accuracy = 0;
static float                   current_temp         = 0.0f;
static float                   current_hum          = 0.0f;
static float                   current_pressure     = 0.0f;
static float                   current_gas_res      = 0.0f;
static float                   current_eco2         = 0.0f;
static float                   current_bvoc         = 0.0f;
static float                   current_tvoc         = 0.0f;

// Instancia global del BSEC 3.0
static void *bsec_instance = NULL;

static void bsec_delay_us(uint32_t period, void *intf_ptr) {
    bosch_hal_delay_us(period, intf_ptr);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Helper: (Re)configurar suscripciones BSEC
 * ──────────────────────────────────────────────────────────────────────────── */
static int8_t configure_bsec_subscriptions(float sample_rate) {
    if (!bsec_instance)
        return -1;

    bsec_sensor_configuration_t requested_virtual_sensors[7];
    uint8_t                     n_requested_virtual_sensors = 7;

    requested_virtual_sensors[0].sensor_id   = BSEC_OUTPUT_IAQ;
    requested_virtual_sensors[0].sample_rate = sample_rate;
    requested_virtual_sensors[1].sensor_id   = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE;
    requested_virtual_sensors[1].sample_rate = sample_rate;
    requested_virtual_sensors[2].sensor_id   = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY;
    requested_virtual_sensors[2].sample_rate = sample_rate;
    requested_virtual_sensors[3].sensor_id   = BSEC_OUTPUT_RAW_GAS;
    requested_virtual_sensors[3].sample_rate = sample_rate;
    requested_virtual_sensors[4].sensor_id   = BSEC_OUTPUT_RAW_PRESSURE;
    requested_virtual_sensors[4].sample_rate = sample_rate;
    requested_virtual_sensors[5].sensor_id   = BSEC_OUTPUT_RAW_TEMPERATURE;
    requested_virtual_sensors[5].sample_rate = sample_rate;
    requested_virtual_sensors[6].sensor_id   = BSEC_OUTPUT_CO2_EQUIVALENT;
    requested_virtual_sensors[6].sample_rate = sample_rate;

    bsec_sensor_configuration_t required_sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t                     n_required_sensor_settings = BSEC_MAX_PHYSICAL_SENSOR;

    bsec_library_return_t bsec_status =
        bsec_update_subscription(bsec_instance, requested_virtual_sensors, n_requested_virtual_sensors,
                                 required_sensor_settings, &n_required_sensor_settings);

    if (bsec_status < BSEC_OK) {
        ESP_LOGE(TAG, "Suscripción BSEC fallida: %d", bsec_status);
        return -1;
    }
    if (bsec_status > BSEC_OK) {
        ESP_LOGW(TAG, "Suscripción BSEC warning/info: %d", bsec_status);
    }

    ESP_LOGI(TAG, "BSEC 3.0 suscripciones aceptadas con éxito (rate: %f)", sample_rate);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Inicialización completa del BME688 + BSEC 3.0
 * ──────────────────────────────────────────────────────────────────────────── */
int8_t bme688_bsec_init(i2c_master_dev_handle_t i2c_dev_handle, i2c_master_dev_handle_t rv_dev_handle,
                        float sample_rate) {
    int8_t rslt = BME68X_OK;
    rtc_dev     = rv_dev_handle;

    bme_dev.read     = bosch_hal_i2c_read;
    bme_dev.write    = bosch_hal_i2c_write;
    bme_dev.delay_us = bsec_delay_us;
    bme_dev.intf_ptr = (void *) i2c_dev_handle;
    bme_dev.intf     = BME68X_I2C_INTF;
    bme_dev.amb_temp = 25;

    rslt = bme68x_init(&bme_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar BME68x: %d", rslt);
        return rslt;
    }
    ESP_LOGI(TAG, "Sensor BME688 detectado (Variant ID: %lu)", (long unsigned int) bme_dev.variant_id);

    if (!bsec_instance) {
        size_t bsec_size = bsec_get_instance_size();
        bsec_instance    = malloc(bsec_size);
        if (!bsec_instance) {
            ESP_LOGE(TAG, "No hay memoria para BSEC instance (%d bytes)", (int) bsec_size);
            return -1;
        }
    }

    bsec_library_return_t bsec_status = bsec_init(bsec_instance);
    if (bsec_status < BSEC_OK) {
        ESP_LOGE(TAG, "Error inicializando BSEC 3.0: %d", bsec_status);
        free(bsec_instance);
        bsec_instance = NULL;
        return -1;
    }

#ifdef CONFIG_ENABLE_DEEP_SLEEP
    /* ── Deep Sleep (legacy): restaurar state blob desde RTC ─────────────── */
    if (rtc_bsec_state_valid && rtc_bsec_ulp_established) {
        uint8_t               work_buffer[BSEC_MAX_WORKBUFFER_SIZE];
        bsec_library_return_t res =
            bsec_set_state(bsec_instance, rtc_bsec_state, BSEC_MAX_STATE_BLOB_SIZE, work_buffer, sizeof(work_buffer));
        if (res == BSEC_OK) {
            ESP_LOGI(TAG, "BSEC state restored from RTC memory (ULP-established).");
        } else {
            ESP_LOGE(TAG, "BSEC restore failed: %d", res);
            rtc_bsec_state_valid = false;
        }
    } else if (rtc_bsec_state_valid && !rtc_bsec_ulp_established) {
        ESP_LOGW(TAG, "⚠️ BSEC state from CONTINUOUS warmup → skipping restore for clean ULP start");
    } else {
        ESP_LOGI(TAG, "BSEC: No previous state to restore (Cold Boot).");
    }
#else
    /* ── Light-Sleep: bsec_instance vive en heap, no hay nada que restaurar.
     *    Entre ciclos de Light-Sleep, el heap se retiene completamente. ──── */
    ESP_LOGI(TAG, "BSEC: Light-Sleep mode — instance lives in heap (no serialization needed).");
#endif

    s_current_sample_rate = sample_rate;
    ESP_LOGI(TAG, "BSEC configuring with sample rate: %f (period: %d s)", sample_rate, (int) (1.0f / sample_rate));
    return configure_bsec_subscriptions(sample_rate);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Reconfigura BSEC en runtime (ej. Warmup → Producción)
 * ──────────────────────────────────────────────────────────────────────────── */
int8_t bme688_bsec_set_sample_rate(float sample_rate) {
    s_current_sample_rate = sample_rate;
    ESP_LOGI(TAG, "BSEC reconfiguring sample rate to %f", sample_rate);
    return configure_bsec_subscriptions(sample_rate);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Lectura IAQ completa (BSEC-managed)
 * Retorna: 0 (nueva medición), -1 (error), -2 (BSEC no requiere medición)
 * ──────────────────────────────────────────────────────────────────────────── */
int8_t bme688_bsec_read_iaq(float *iaq, uint8_t *accuracy, float *temperature, float *humidity, float *pressure,
                            float *gas_resistance, float *eco2, float *bvoc, float *tvoc) {
    bsec_library_return_t bsec_status;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t curr_time_ns = ((int64_t) tv.tv_sec * 1000000000LL) + ((int64_t) tv.tv_usec * 1000LL);

    bsec_bme_settings_t bme_settings;

    if (!bsec_instance)
        return -1;

    bsec_status = bsec_sensor_control(bsec_instance, curr_time_ns, &bme_settings);
    if (bsec_status < BSEC_OK)
        return -1;

    /* Guardar next_call para el timer dinámico de Light-Sleep */
    s_last_bsec_next_call_ns = bme_settings.next_call;

    ESP_LOGD(TAG, "🔍 BSEC timestamp: %lld ns (%lld s) | trigger=%d | next_call=%lld ns (in +%lld s)", curr_time_ns,
             curr_time_ns / 1000000000LL, bme_settings.trigger_measurement, bme_settings.next_call,
             (bme_settings.next_call - curr_time_ns) / 1000000000LL);

    if (bme_settings.trigger_measurement) {
        struct bme68x_conf       conf;
        struct bme68x_heatr_conf heatr_conf;

        conf.os_hum  = bme_settings.humidity_oversampling;
        conf.os_temp = bme_settings.temperature_oversampling;
        conf.os_pres = bme_settings.pressure_oversampling;
        conf.filter  = BME68X_FILTER_OFF;
        conf.odr     = BME68X_ODR_NONE;
        if (bme68x_set_conf(&conf, &bme_dev) != BME68X_OK) {
            ESP_LOGE(TAG, "Failed to configure BME688");
            return -1;
        }

        heatr_conf.enable          = bme_settings.run_gas;
        heatr_conf.heatr_temp      = bme_settings.heater_temperature;
        heatr_conf.heatr_dur       = bme_settings.heater_duration;
        heatr_conf.heatr_temp_prof = &bme_settings.heater_temperature_profile[0];
        heatr_conf.heatr_dur_prof  = &bme_settings.heater_duration_profile[0];
        heatr_conf.profile_len     = bme_settings.heater_profile_len;

        ESP_LOGD(TAG, "🔧 Heater config: enable=%d temp=%d dur=%d op_mode=%d prof_len=%d", heatr_conf.enable,
                 heatr_conf.heatr_temp, heatr_conf.heatr_dur, bme_settings.op_mode, heatr_conf.profile_len);

        int8_t heatr_rslt = bme68x_set_heatr_conf(bme_settings.op_mode, &heatr_conf, &bme_dev);
        if (heatr_rslt != BME68X_OK) {
            ESP_LOGE(TAG, "Failed to configure BME688 heater (rslt=%d). Retrying with soft reset...", heatr_rslt);
            /* Retry: soft-reset el sensor y reintentar la config */
            bme68x_soft_reset(&bme_dev);
            vTaskDelay(pdMS_TO_TICKS(10));
            bme68x_init(&bme_dev);
            bme68x_set_conf(&conf, &bme_dev);
            heatr_rslt = bme68x_set_heatr_conf(bme_settings.op_mode, &heatr_conf, &bme_dev);
            if (heatr_rslt != BME68X_OK) {
                ESP_LOGE(TAG, "Heater config STILL failed after soft reset (rslt=%d)", heatr_rslt);
                return -1;
            }
            ESP_LOGI(TAG, "Heater config succeeded after soft reset");
        }

        if (bme68x_set_op_mode(bme_settings.op_mode, &bme_dev) != BME68X_OK) {
            ESP_LOGE(TAG, "Failed to start BME688 measurement");
            return -1;
        }

        uint32_t meas_dur = bme68x_get_meas_dur(bme_settings.op_mode, &conf, &bme_dev);
        meas_dur += (bme_settings.heater_duration * 1000);
        bosch_hal_delay_us(meas_dur, bme_dev.intf_ptr);

        struct bme68x_data data[3];
        uint8_t            n_fields = 0;
        if (bme68x_get_data(bme_settings.op_mode, data, &n_fields, &bme_dev) != BME68X_OK) {
            ESP_LOGE(TAG, "Failed to read BME688 measurement");
            return -1;
        }

        if (n_fields > 0) {
            bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
            uint8_t      n_inputs = 0;

            if (bme_settings.process_data & BSEC_PROCESS_TEMPERATURE) {
                inputs[n_inputs].sensor_id  = BSEC_INPUT_TEMPERATURE;
                inputs[n_inputs].signal     = data[0].temperature;
                inputs[n_inputs].time_stamp = curr_time_ns;
                n_inputs++;
            }
            if (bme_settings.process_data & BSEC_PROCESS_HUMIDITY) {
                inputs[n_inputs].sensor_id  = BSEC_INPUT_HUMIDITY;
                inputs[n_inputs].signal     = data[0].humidity;
                inputs[n_inputs].time_stamp = curr_time_ns;
                n_inputs++;
            }
            if (bme_settings.process_data & BSEC_PROCESS_PRESSURE) {
                inputs[n_inputs].sensor_id  = BSEC_INPUT_PRESSURE;
                inputs[n_inputs].signal     = data[0].pressure;
                inputs[n_inputs].time_stamp = curr_time_ns;
                n_inputs++;
            }
            if (bme_settings.process_data & BSEC_PROCESS_GAS) {
                inputs[n_inputs].sensor_id  = BSEC_INPUT_GASRESISTOR;
                inputs[n_inputs].signal     = data[0].gas_resistance;
                inputs[n_inputs].time_stamp = curr_time_ns;
                n_inputs++;
            }

            bsec_output_t outputs[BSEC_NUMBER_OUTPUTS];
            uint8_t       n_outputs = BSEC_NUMBER_OUTPUTS;

            bsec_status = bsec_do_steps(bsec_instance, inputs, n_inputs, outputs, &n_outputs);
            if (bsec_status < BSEC_OK) {
                ESP_LOGE(TAG, "BSEC processing failed: %d", bsec_status);
                return -1;
            }

            ESP_LOGI(TAG, "BSEC do_steps: n_outputs=%u | RAW T=%.1f H=%.1f P=%.0f Gas=%.0f", n_outputs,
                     data[0].temperature, data[0].humidity, data[0].pressure, data[0].gas_resistance);

            for (uint8_t i = 0; i < n_outputs; i++) {
                if (outputs[i].sensor_id == BSEC_OUTPUT_IAQ) {
                    current_iaq          = outputs[i].signal;
                    current_iaq_accuracy = outputs[i].accuracy;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE) {
                    current_temp = outputs[i].signal;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY) {
                    current_hum = outputs[i].signal;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_RAW_PRESSURE) {
                    current_pressure = outputs[i].signal / 100.0f; // Pa → hPa
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_RAW_GAS) {
                    current_gas_res = outputs[i].signal;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_CO2_EQUIVALENT) {
                    current_eco2 = outputs[i].signal;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_BREATH_VOC_EQUIVALENT) {
                    current_bvoc = outputs[i].signal;
                }
                if (outputs[i].sensor_id == BSEC_OUTPUT_TVOC_EQUIVALENT) {
                    current_tvoc = outputs[i].signal;
                }
            }

#ifdef CONFIG_ENABLE_DEEP_SLEEP
            /* ── FALLBACK CRÍTICO: Anchor Point post-Deep-Sleep ──────────
             * Cuando BSEC restaura un state blob, la primera medición sirve
             * como "anchor point" para recalibrar el filtro de Kalman.
             * bsec_do_steps() retorna n_outputs=0 (sin datos procesados),
             * pero el hardware SÍ midió (n_fields > 0). */
            if (n_outputs == 0 && n_fields > 0) {
                ESP_LOGW(TAG, "⚓ BSEC anchor point: n_outputs=0, using RAW sensor fallback");
                current_temp         = data[0].temperature;
                current_hum          = data[0].humidity;
                current_pressure     = data[0].pressure / 100.0f;
                current_gas_res      = data[0].gas_resistance;
                current_iaq          = 0.0f;
                current_iaq_accuracy = 0;
            }

            /* ── Persistir estado BSEC en RTC (incluso con n_outputs=0) ── */
            uint8_t               work_buffer[BSEC_MAX_WORKBUFFER_SIZE];
            uint32_t              actual_len = 0;
            bsec_library_return_t res = bsec_get_state(bsec_instance, 0, rtc_bsec_state, BSEC_MAX_STATE_BLOB_SIZE,
                                                       work_buffer, sizeof(work_buffer), &actual_len);
            if (res == BSEC_OK) {
                rtc_bsec_state_valid     = true;
                rtc_bsec_ulp_established = true;
                ESP_LOGI(TAG, "BSEC state persisted (ULP-established=true, n_outputs=%u)", n_outputs);
            } else {
                ESP_LOGW(TAG, "Failed to persist BSEC state: %d", res);
            }

            /* ── Calcular PRÓXIMA MEDICIÓN desde 1/sample_rate ──────────── */
            if (s_current_sample_rate > 0.0f) {
                int64_t period_ns            = (int64_t) (1.0 / (double) s_current_sample_rate * 1000000000.0);
                rtc_bsec_next_measurement_ns = curr_time_ns + period_ns;
                ESP_LOGI(TAG, "📊 BSEC next measurement: in %lld s (period=%lld s, rate=%.6f Hz)",
                         period_ns / 1000000000LL, period_ns / 1000000000LL, s_current_sample_rate);
            }
#endif
            /* Log diagnóstico: next_call nativo de BSEC */
            ESP_LOGD(TAG, "BSEC native next_call: +%lld s", (bme_settings.next_call - curr_time_ns) / 1000000000LL);
        }

        if (iaq)
            *iaq = current_iaq;
        if (accuracy)
            *accuracy = current_iaq_accuracy;
        if (temperature)
            *temperature = current_temp;
        if (humidity)
            *humidity = current_hum;
        if (pressure)
            *pressure = current_pressure;
        if (gas_resistance)
            *gas_resistance = current_gas_res;
        if (eco2)
            *eco2 = current_eco2;
        if (bvoc)
            *bvoc = current_bvoc;
        if (tvoc)
            *tvoc = current_tvoc;
        return 0;
    }

    // BSEC no requiere medición en este timestamp
    if (iaq)
        *iaq = current_iaq;
    if (accuracy)
        *accuracy = current_iaq_accuracy;
    if (temperature)
        *temperature = current_temp;
    if (humidity)
        *humidity = current_hum;
    if (pressure)
        *pressure = current_pressure;
    if (gas_resistance)
        *gas_resistance = current_gas_res;
    if (eco2)
        *eco2 = current_eco2;
    if (bvoc)
        *bvoc = current_bvoc;
    if (tvoc)
        *tvoc = current_tvoc;
    return -2;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Lectura Directa en Forced Mode (sin BSEC)
 * Para modos donde el intervalo no es compatible con LP/ULP/CONT.
 * ──────────────────────────────────────────────────────────────────────────── */
int8_t bme688_raw_forced_read(float *temperature, float *humidity, float *pressure, float *gas_resistance) {
    struct bme68x_conf conf;
    conf.os_hum  = BME68X_OS_2X;
    conf.os_temp = BME68X_OS_16X;
    conf.os_pres = BME68X_OS_1X;
    conf.filter  = BME68X_FILTER_OFF;
    conf.odr     = BME68X_ODR_NONE;

    if (bme68x_set_conf(&conf, &bme_dev) != BME68X_OK) {
        ESP_LOGE(TAG, "Raw forced: failed to configure BME688");
        return -1;
    }

    struct bme68x_heatr_conf heatr_conf;
    heatr_conf.enable     = BME68X_ENABLE;
    heatr_conf.heatr_temp = 320; // 320°C para gas
    heatr_conf.heatr_dur  = 150; // 150ms
    if (bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, &bme_dev) != BME68X_OK) {
        ESP_LOGE(TAG, "Raw forced: failed to configure heater");
        return -1;
    }

    if (bme68x_set_op_mode(BME68X_FORCED_MODE, &bme_dev) != BME68X_OK) {
        ESP_LOGE(TAG, "Raw forced: failed to start measurement");
        return -1;
    }

    uint32_t meas_dur = bme68x_get_meas_dur(BME68X_FORCED_MODE, &conf, &bme_dev);
    meas_dur += (150 * 1000); // heater duration
    bosch_hal_delay_us(meas_dur, bme_dev.intf_ptr);

    struct bme68x_data data[3];
    uint8_t            n_fields = 0;
    if (bme68x_get_data(BME68X_FORCED_MODE, data, &n_fields, &bme_dev) != BME68X_OK || n_fields == 0) {
        ESP_LOGE(TAG, "Raw forced: failed to read data");
        return -1;
    }

    if (temperature)
        *temperature = data[0].temperature;
    if (humidity)
        *humidity = data[0].humidity;
    if (pressure)
        *pressure = data[0].pressure / 100.0f; // Pa → hPa
    if (gas_resistance)
        *gas_resistance = data[0].gas_resistance;

    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Retorna el timestamp (ns) de la próxima medición BSEC.
 *
 * Light-Sleep: retorna next_call nativo de bsec_sensor_control().
 *   Con RAM retenida, BSEC mantiene su estado temporal completo y
 *   next_call refleja el verdadero próximo momento de medición.
 *
 * Deep Sleep (legacy): retorna measurement_timestamp + 1/sample_rate.
 *   Porque next_call nativo es siempre +3s (polling interno inútil).
 * ──────────────────────────────────────────────────────────────────────────── */
int64_t bme688_bsec_get_next_call_ns(void) {
#ifdef CONFIG_ENABLE_DEEP_SLEEP
    return rtc_bsec_next_measurement_ns;
#else
    return s_last_bsec_next_call_ns;
#endif
}

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/* ────────────────────────────────────────────────────────────────────────────
 * Deep Sleep legacy: marca state blob como stale y resetea RTC state.
 * ──────────────────────────────────────────────────────────────────────────── */
void bme688_bsec_mark_state_stale(void) {
    rtc_bsec_ulp_established = false;
    ESP_LOGW(TAG, "BSEC state marked STALE (CONTINUOUS → ULP transition). "
                  "Next init will start fresh.");
}

void bme688_bsec_reset_rtc_state(void) {
    rtc_bsec_state_valid         = false;
    rtc_bsec_ulp_established     = false;
    rtc_bsec_next_measurement_ns = 0;
    memset(rtc_bsec_state, 0, sizeof(rtc_bsec_state));
    ESP_LOGW(TAG, "🔄 BSEC RTC state RESET (cold boot / fresh flash)");
}
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Self-Test: Validación de Hardware (Activa)
 * ──────────────────────────────────────────────────────────────────────────── */
esp_err_t bme688_bsec_self_test(i2c_master_dev_handle_t dev_handle, bool *test_passed) {
    if (!dev_handle || !test_passed) {
        if (test_passed)
            *test_passed = false;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "BME688 Self-Test Triggered. Reading Chip ID...");

    uint8_t reg_addr = BME68X_REG_CHIP_ID;
    uint8_t chip_id  = 0;

    esp_err_t err = bosch_hal_i2c_read(reg_addr, &chip_id, 1, (void *) dev_handle);

    if (err != 0) { // bosch_hal_i2c_read returns BME68X_OK (0) on success
        ESP_LOGE(TAG, "BME688 Self-Test Failed: I2C error %d", err);
        *test_passed = false;
        return ESP_FAIL;
    }

    if (chip_id == BME68X_CHIP_ID) {
        ESP_LOGI(TAG, "BME688 Self-Test Passed. Chip ID matches (0x%02X)", chip_id);
        *test_passed = true;
    } else {
        ESP_LOGE(TAG, "BME688 Self-Test Failed! Invalid Chip ID: 0x%02X (Expected 0x%02X)", chip_id, BME68X_CHIP_ID);
        *test_passed = false;
    }

    return ESP_OK;
}
