#include "bme688_bsec_wrapper.h"
#include "bme68x.h"
#include "bsec_interface.h"
#include "bsec_datatypes.h"
#include "bosch_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme688_bsec";

static struct bme68x_dev bme_dev;
static float             current_iaq          = 0.0f;
static uint8_t           current_iaq_accuracy = 0;
static float             current_temp         = 0.0f;
static float             current_hum          = 0.0f;

// Instancia global del BSEC 3.0
static void *bsec_instance = NULL;

// Wrapper simple del delay para BSEC en microsegundos
static void bsec_delay_us(uint32_t period, void *intf_ptr) {
    bosch_hal_delay_us(period, intf_ptr);
}

int8_t bme688_bsec_init(i2c_master_dev_handle_t i2c_dev_handle) {
    int8_t rslt = BME68X_OK;

    // 1. Configurar HAL nativo en el struct del BME68x
    bme_dev.read     = bosch_hal_i2c_read;
    bme_dev.write    = bosch_hal_i2c_write;
    bme_dev.delay_us = bsec_delay_us;
    bme_dev.intf_ptr = (void *) i2c_dev_handle;
    bme_dev.intf     = BME68X_I2C_INTF;
    bme_dev.amb_temp = 25; // Default ambient temp

    rslt = bme68x_init(&bme_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar BME68x: %d", rslt);
        return rslt;
    }

    ESP_LOGI(TAG, "Sensor BME688 detectado (Variant ID: %lu)", (long unsigned int) bme_dev.variant_id);

    // 2. Asignar e inicializar la instancia de la librería BSEC 3.0
    if (!bsec_instance) {
        size_t bsec_size = bsec_get_instance_size();
        bsec_instance    = malloc(bsec_size);
        if (!bsec_instance) {
            ESP_LOGE(TAG, "No hay memoria para BSEC instance (%d bytes)", (int) bsec_size);
            return -1;
        }
    }

    bsec_library_return_t bsec_status = bsec_init(bsec_instance);
    if (bsec_status != BSEC_OK) {
        ESP_LOGE(TAG, "Error inicializando BSEC 3.0: %d", bsec_status);
        return -1;
    }

    // 3. Suscripciones BSEC (Las salidas que queremos que el algoritmo calcule)
    bsec_sensor_configuration_t requested_virtual_sensors[4];
    uint8_t                     n_requested_virtual_sensors = 4;

    // IAQ (Índice Calidad del Aire)
    requested_virtual_sensors[0].sensor_id   = BSEC_OUTPUT_IAQ;
    requested_virtual_sensors[0].sample_rate = BSEC_SAMPLE_RATE_ULP; // Ultra Low Power (300s)

    // Temperatura compensada
    requested_virtual_sensors[1].sensor_id   = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE;
    requested_virtual_sensors[1].sample_rate = BSEC_SAMPLE_RATE_ULP;

    // Humedad compensada
    requested_virtual_sensors[2].sensor_id   = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY;
    requested_virtual_sensors[2].sample_rate = BSEC_SAMPLE_RATE_ULP;

    // Gas puro (resistencia) opcional
    requested_virtual_sensors[3].sensor_id   = BSEC_OUTPUT_RAW_GAS;
    requested_virtual_sensors[3].sample_rate = BSEC_SAMPLE_RATE_ULP;

    bsec_sensor_configuration_t required_sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t                     n_required_sensor_settings = BSEC_MAX_PHYSICAL_SENSOR;

    bsec_status = bsec_update_subscription(bsec_instance, requested_virtual_sensors, n_requested_virtual_sensors,
                                           required_sensor_settings, &n_required_sensor_settings);

    if (bsec_status != BSEC_OK) {
        ESP_LOGE(TAG, "Suscripción BSEC fallida: %d", bsec_status);
        return -1;
    }

    ESP_LOGI(TAG, "BSEC 3.0 suscripciones aceptadas con éxito");
    return 0;
}

int8_t bme688_bsec_read_iaq(float *iaq, uint8_t *accuracy, float *temperature, float *humidity) {
    bsec_library_return_t bsec_status;

    // Obtener la configuración que el algoritmo BSEC requiere que el sensor físico tenga para esta marca de tiempo
    // (timestamp)
    int64_t             curr_time_ns = esp_timer_get_time() * 1000;
    bsec_bme_settings_t bme_settings;

    if (!bsec_instance)
        return -1;

    bsec_status = bsec_sensor_control(bsec_instance, curr_time_ns, &bme_settings);
    if (bsec_status != BSEC_OK)
        return -1;

    // Si el algoritmo solicita que midamos...
    if (bme_settings.trigger_measurement) {
        struct bme68x_conf       conf;
        struct bme68x_heatr_conf heatr_conf;

        conf.os_hum  = bme_settings.humidity_oversampling;
        conf.os_temp = bme_settings.temperature_oversampling;
        conf.os_pres = bme_settings.pressure_oversampling;
        conf.filter  = BME68X_FILTER_OFF;
        conf.odr     = BME68X_ODR_NONE;
        bme68x_set_conf(&conf, &bme_dev);

        heatr_conf.enable          = bme_settings.run_gas;
        heatr_conf.heatr_temp      = bme_settings.heater_temperature;
        heatr_conf.heatr_dur       = bme_settings.heater_duration;
        heatr_conf.heatr_temp_prof = &bme_settings.heater_temperature_profile[0];
        heatr_conf.heatr_dur_prof  = &bme_settings.heater_duration_profile[0];
        heatr_conf.profile_len     = bme_settings.heater_profile_len;
        bme68x_set_heatr_conf(bme_settings.op_mode, &heatr_conf, &bme_dev);

        bme68x_set_op_mode(bme_settings.op_mode, &bme_dev);

        // Polling para esperar la medición (zero-cpu delay loop manual para BSEC)
        uint32_t meas_dur = bme68x_get_meas_dur(bme_settings.op_mode, &conf, &bme_dev);
        // Add heater duration
        meas_dur += (bme_settings.heater_duration * 1000); // bme_settings contains heater_duration in ms
        bosch_hal_delay_us(meas_dur, bme_dev.intf_ptr);

        struct bme68x_data data[3];
        uint8_t            n_fields = 0;
        bme68x_get_data(bme_settings.op_mode, data, &n_fields, &bme_dev);

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

            // Inyectar al BSEC 3.0 para procesar IAQ
            bsec_do_steps(bsec_instance, inputs, n_inputs, outputs, &n_outputs);

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
            }
        }
    }

    if (iaq)
        *iaq = current_iaq;
    if (accuracy)
        *accuracy = current_iaq_accuracy;
    if (temperature)
        *temperature = current_temp;
    if (humidity)
        *humidity = current_hum;

    return 0;
}
