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
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"

// Componentes modulares
#include "i2c_bus.h"
#include "power_manager.h"
#include "bme688_bsec_wrapper.h"
#include "scd41.h"
#include "bmv080_wrapper.h"

static const char *TAG = "app_main";

// Persistencia en memoria RTC para el estado del BSEC 3.0 (IAQ Algorithm)
RTC_DATA_ATTR uint8_t bsec_rtc_state[139];
RTC_DATA_ATTR bool    bsec_rtc_valid = false;

// Estado del Auto-Discovery (Detectará si existe el BMV080)
RTC_DATA_ATTR bool is_pro_model   = false;
RTC_DATA_ATTR bool discovery_done = false;

// Handlers de sensores I2C
static i2c_master_dev_handle_t bme688_dev = NULL;
static i2c_master_dev_handle_t scd41_dev  = NULL;
static i2c_master_dev_handle_t bmv080_dev = NULL;

static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;
    power_wake_state_t      state      = power_manager_get_wake_state();

    // Auto-Discovery I2C (Solo ocurre la primera vez tras un reinicio físico / Cold Boot)
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        discovery_done = false;
        bsec_rtc_valid = false;
    }

    if (!discovery_done) {
        ESP_LOGI(TAG, "Running I2C Auto-Discovery...");

        esp_err_t probe_scd41  = i2c_master_probe(bus_handle, 0x62, 100);
        esp_err_t probe_bmv080 = i2c_master_probe(bus_handle, 0x54, 100);

        if (probe_scd41 == ESP_OK) {
            ESP_LOGI(TAG, "SCD41 detected at 0x62");
        }

        if (probe_bmv080 == ESP_OK) {
            ESP_LOGI(TAG, "BMV080 detected at 0x54");
            is_pro_model = true;
        } else {
            is_pro_model = false;
        }

        discovery_done = true;
        ESP_LOGI(TAG, "Hardware Topology: %s Model", is_pro_model ? "PRO" : "BASE");
    }

    // 1. Adjuntar dispositivos al bus
    i2c_device_config_t scd41_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x62, .scl_speed_hz = 100000};
    i2c_master_bus_add_device(bus_handle, &scd41_cfg, &scd41_dev);

    i2c_device_config_t bme688_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x76, .scl_speed_hz = 100000};
    i2c_master_bus_add_device(bus_handle, &bme688_cfg, &bme688_dev);

    if (is_pro_model) {
        i2c_device_config_t bmv080_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x54, .scl_speed_hz = 100000};
        i2c_master_bus_add_device(bus_handle, &bmv080_cfg, &bmv080_dev);
    }

    // MÁQUINA DE ESTADOS (Micro-Sleep / Master-Sleep)
    if (state == PM_STATE_WAKE_A) {
        ESP_LOGI(TAG, "=== WAKE A: Simultaneous Trigger ===");

        // 1. Trigger SCD41 (Lento: tarda 5 segundos en procesar)
        scd41_trigger_single_shot(scd41_dev);

        // 2. Trigger Láser BMV080 (Iniciarlo)
        if (is_pro_model) {
            bmv080_wrapper_init(bmv080_dev);
        }

        // 3. Procesar BME688 con BSEC 3.0 (IAQ)
        // El wrapper internamente enciende el heater y hace un active wait cediendo el core (Zero-CPU).
        if (bme688_bsec_init(bme688_dev) == 0) {
            float   iaq, temp, hum;
            uint8_t acc;
            bme688_bsec_read_iaq(&iaq, &acc, &temp, &hum);
            ESP_LOGI(TAG, "BME688 (BSEC 3.0) -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %%", iaq, acc, temp,
                     hum);
        }

    } else {
        ESP_LOGI(TAG, "=== WAKE B: Data Collection ===");

        // 1. Leer SCD41 (El silicio ya terminó de procesar en estos 5 segs)
        scd41_data_t scd41_data;
        if (scd41_read_measurement(scd41_dev, &scd41_data) == ESP_OK) {
            ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                     scd41_data.temperature, scd41_data.humidity);
        }

        // 2. Leer BMV080 (Polling por la lectura actual de partículas)
        if (is_pro_model) {
            float pm25 = 0;
            if (bmv080_wrapper_read_pm25(&pm25) == 0) {
                // Logueo ya realizado en el callback interno del wrapper
            }
        }

        // El BME688 ya fue leído en WAKE_A ya que su heater cycle es rápido (aprox 150ms).

        // 4. Empaquetar y Transmitir Protobuf (Fase 4 - Futuro GCP/Firebase)
    }

    // Limpiar handlers para evitar memory leaks antes del sleep
    i2c_master_bus_rm_device(scd41_dev);
    i2c_master_bus_rm_device(bme688_dev);
    if (is_pro_model) {
        i2c_master_bus_rm_device(bmv080_dev);
    }

    // Ejecutar política estricta de energía y delegar a la FSM
    power_manager_execute_sleep_cycle();
}

void app_main(void) {
    // [CRÍTICO] Liberar los pines retenidos por el RTC durante el Deep Sleep.
    // Si no hacemos esto, el controlador I2C en hardware intentará cambiar el estado de los pines
    // pero estarán bloqueados físicamente por el dominio RTC, causando un "I2C software timeout".
    gpio_hold_dis(I2C_MASTER_SDA_IO);
    gpio_hold_dis(I2C_MASTER_SCL_IO);
    gpio_deep_sleep_hold_dis();

    // Si es un Cold Boot, los sensores I2C (como el SCD41) pueden mantener la línea SDA en LOW
    // durante hasta 1 segundo mientras su silicio interno arranca. Si inicializamos el bus ahora,
    // el driver I2C fallará. Debemos esperar a que liberen el bus.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Hardware cold boot. Delaying 1000ms for sensors to release I2C bus...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Inicializar la capa física del bus de forma segura y encapsulada
    i2c_master_bus_handle_t bus_handle;
    if (i2c_bus_init(&bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Critical hardware failure on I2C initialization. Rebooting...");
        esp_restart();
    }

    // Anclar la orquestación de sensores al Core 1 para proteger la telemetría futura
    xTaskCreatePinnedToCore(sensor_orchestration_task, "Sensor_Orchestrator", 4096, (void *) bus_handle, 5, NULL, 1);
}
