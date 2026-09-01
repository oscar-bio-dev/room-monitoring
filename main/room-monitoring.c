/*
 * Copyright (c) 2026 oscar-bio-dev
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"

// Componentes modulares
#include "i2c_bus.h"
#include "power_manager.h"
#include "bme688.h"
#include "scd41.h"
#include "bmv080.h"

static const char *TAG = "app_main";

// Persistencia en memoria RTC para calibración de BME688
RTC_DATA_ATTR bme688_calib_data_t rtc_calib_data;
RTC_DATA_ATTR bool                rtc_calib_valid = false;

// Estado del Auto-Discovery (Detectará si existe el BMV080)
RTC_DATA_ATTR bool is_pro_model   = false;
RTC_DATA_ATTR bool discovery_done = false;

// Handlers de sensores
static bme688_device_t         bme688_dev;
static i2c_master_dev_handle_t scd41_dev  = NULL;
static i2c_master_dev_handle_t bmv080_dev = NULL;

static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;
    power_wake_state_t      state      = power_manager_get_wake_state();

    // Auto-Discovery I2C (Solo ocurre la primera vez tras un reinicio físico / Cold Boot)
    if (!discovery_done) {
        ESP_LOGI(TAG, "Cold boot detected. Waiting 1000ms for hardware (SCD41/BMV) to boot...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Running I2C Auto-Discovery...");

        esp_err_t probe_scd41  = i2c_master_probe(bus_handle, SCD41_I2C_ADDR, 100);
        esp_err_t probe_bmv080 = i2c_master_probe(bus_handle, BMV080_I2C_ADDR, 100);

        if (probe_scd41 == ESP_OK) {
            ESP_LOGI(TAG, "SCD41 detected at 0x%02x", SCD41_I2C_ADDR);
        }

        if (probe_bmv080 == ESP_OK) {
            ESP_LOGI(TAG, "BMV080 detected at 0x%02x", BMV080_I2C_ADDR);
            is_pro_model = true;
        } else {
            is_pro_model = false;
        }

        discovery_done = true;
        ESP_LOGI(TAG, "Hardware Topology: %s Model", is_pro_model ? "PRO" : "BASE");
    }

    // Adjuntar los dispositivos al bus maestro I2C
    i2c_device_config_t scd41_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SCD41_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    i2c_master_bus_add_device(bus_handle, &scd41_cfg, &scd41_dev);

    if (is_pro_model) {
        i2c_device_config_t bmv080_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BMV080_I2C_ADDR,
            .scl_speed_hz    = 100000,
        };
        i2c_master_bus_add_device(bus_handle, &bmv080_cfg, &bmv080_dev);
    }

    // Inicializar BME688 (Base line sensor)
    esp_err_t err =
        bme688_init(bus_handle, BME688_I2C_ADDR_PRIMARY, &bme688_dev, rtc_calib_valid ? &rtc_calib_data : NULL);
    if (err == ESP_OK && !rtc_calib_valid) {
        rtc_calib_data  = bme688_dev.calib;
        rtc_calib_valid = true;
    }

    // MÁQUINA DE ESTADOS
    if (state == PM_STATE_WAKE_A) {
        ESP_LOGI(TAG, "=== WAKE A: Simultaneous Trigger ===");

        // 1. Trigger SCD41
        scd41_trigger_single_shot(scd41_dev);

        // 2. Trigger BMV080 Laser (Solo modelo Pro)
        if (is_pro_model) {
            bmv080_trigger_measurement(bmv080_dev);
        }

        // 3. Trigger BME688
        if (err == ESP_OK) {
            bme688_trigger_forced_measurement(&bme688_dev);
        }

    } else {
        ESP_LOGI(TAG, "=== WAKE B: Data Collection ===");

        // 1. Leer SCD41
        scd41_data_t scd41_data;
        if (scd41_read_measurement(scd41_dev, &scd41_data) == ESP_OK) {
            ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                     scd41_data.temperature, scd41_data.humidity);
        }

        // 2. Leer BMV080 y apagar láser
        if (is_pro_model) {
            bmv080_data_t bmv080_data;
            if (bmv080_read_measurement(bmv080_dev, &bmv080_data) == ESP_OK) {
                ESP_LOGI(TAG, "BMV080  -> PM2.5: %.2f ug/m3", bmv080_data.pm2_5);
            }
        }

        // 3. Leer BME688
        if (err == ESP_OK) {
            bme688_data_t sensor_data;
            if (bme688_read_data(&bme688_dev, &sensor_data) == ESP_OK) {
                ESP_LOGI(TAG, "BME688  -> Temp: %.2f C | Hum: %.2f %% | Press: %.2f hPa | Gas: %.0f Ohms",
                         sensor_data.temperature, sensor_data.humidity, sensor_data.pressure, sensor_data.gas_res);
            }
        }

        // 4. Empaquetar y Transmitir Protobuf (Fase 4)
    }

    // Limpiar handlers para evitar memory leaks antes del sleep
    i2c_master_bus_rm_device(scd41_dev);
    if (is_pro_model) {
        i2c_master_bus_rm_device(bmv080_dev);
    }

    // Ejecutar política estricta de energía y delegar a la FSM
    power_manager_execute_sleep_cycle();
}

void app_main(void) {
    // Inicializar la capa física del bus de forma segura y encapsulada
    i2c_master_bus_handle_t bus_handle;
    if (i2c_bus_init(&bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Critical hardware failure on I2C initialization. Rebooting...");
        esp_restart();
    }

    // Anclar la orquestación de sensores al Core 1 para proteger la telemetría futura
    xTaskCreatePinnedToCore(sensor_orchestration_task, "Sensor_Orchestrator", 4096, (void *) bus_handle, 5, NULL, 1);
}
