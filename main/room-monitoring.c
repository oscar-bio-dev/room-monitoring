/*
 * Copyright (c) 2026 Oscar
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

static const char *TAG = "app_main";

// Persistencia en memoria RTC para calibración de BME688
RTC_DATA_ATTR bme688_calib_data_t rtc_calib_data;
RTC_DATA_ATTR bool                rtc_calib_valid = false;

// Estado del Auto-Discovery (Detectará si existe el BMV080)
RTC_DATA_ATTR bool is_pro_model   = false;
RTC_DATA_ATTR bool discovery_done = false;

static bme688_device_t bme688_dev;

static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;
    power_wake_state_t      state      = power_manager_get_wake_state();

    // Auto-Discovery I2C (Solo ocurre la primera vez tras un reinicio físico)
    if (!discovery_done) {
        ESP_LOGI(TAG, "Cold boot detected. Running I2C Auto-Discovery...");
        // TODO: Hacer un i2c_master_probe sobre la dirección 0x54 (BMV080)
        is_pro_model   = false; // Mock por ahora
        discovery_done = true;
        ESP_LOGI(TAG, "Hardware Topology: %s Model", is_pro_model ? "PRO" : "BASE");
    }

    // Inicializar BME688 (Base line sensor)
    esp_err_t err =
        bme688_init(bus_handle, BME688_I2C_ADDR_PRIMARY, &bme688_dev, rtc_calib_valid ? &rtc_calib_data : NULL);
    if (err == ESP_OK && !rtc_calib_valid) {
        rtc_calib_data  = bme688_dev.calib;
        rtc_calib_valid = true;
    }

    if (state == PM_STATE_WAKE_A) {
        ESP_LOGI(TAG, "=== WAKE A: Simultaneous Trigger ===");

        // 1. Trigger SCD41 (TODO)
        // 2. Trigger BMV080 Laser (TODO - if is_pro_model)

        // 3. Trigger BME688
        if (err == ESP_OK) {
            bme688_trigger_forced_measurement(&bme688_dev);
            // Cruzar datos de presión barométrica al SCD41 iría aquí
        }

    } else {
        ESP_LOGI(TAG, "=== WAKE B: Data Collection ===");

        // 1. Leer SCD41 (TODO)
        // 2. Leer BMV080 y apagar láser (TODO - if is_pro_model)

        // 3. Leer BME688
        if (err == ESP_OK) {
            bme688_data_t sensor_data;
            if (bme688_read_data(&bme688_dev, &sensor_data) == ESP_OK) {
                ESP_LOGI(TAG, "BME688 -> Temp: %.2f C | Hum: %.2f %% | Press: %.2f hPa | Gas: %.0f Ohms",
                         sensor_data.temperature, sensor_data.humidity, sensor_data.pressure, sensor_data.gas_res);
            }
        }

        // 4. Empaquetar y Transmitir Protobuf (TODO Fase 4)
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
