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
#include "esp_system.h"

// Componentes modulares
#include "i2c_bus.h"
#include "power_manager.h"
#include "bme688_bsec_wrapper.h"
#include "scd41.h"
#include "bmv080_wrapper.h"

static const char *TAG = "app_main";

// Estado del Auto-Discovery (Detectará si existe el BMV080)
RTC_DATA_ATTR bool    is_pro_model     = false;
RTC_DATA_ATTR bool    discovery_done   = false;
RTC_DATA_ATTR uint8_t dynamic_bme_addr = 0x76;
RTC_DATA_ATTR uint8_t dynamic_bmv_addr = 0x54;

// Handlers de sensores I2C
static i2c_master_dev_handle_t bme688_dev = NULL;
static i2c_master_dev_handle_t scd41_dev  = NULL;
static i2c_master_dev_handle_t bmv080_dev = NULL;

static esp_err_t retry_scd41_trigger(void) {
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        err = scd41_trigger_single_shot(scd41_dev);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SCD41 trigger failed (%s), attempt %u/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

static esp_err_t retry_scd41_read(scd41_data_t *data) {
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        err = scd41_read_measurement(scd41_dev, data);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SCD41 read failed (%s), attempt %u/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;

    // Auto-Discovery I2C (Solo ocurre la primera vez tras un reinicio físico / Cold Boot)
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        discovery_done = false;
    }

    if (!discovery_done) {
        ESP_LOGI(TAG, "Running I2C Auto-Discovery Scanner...");

        is_pro_model   = false;
        bool bme_found = false;

        // Escanear todas las direcciones posibles (1 a 127)
        for (uint8_t addr = 1; addr < 127; addr++) {
            if (i2c_master_probe(bus_handle, addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "=> I2C Device detected at address 0x%02X", addr);

                if (addr == 0x62) {
                    ESP_LOGI(TAG, "   (SCD41 CO2 Sensor)");
                } else if (addr == 0x76 || addr == 0x77) {
                    ESP_LOGI(TAG, "   (BME688 IAQ Sensor)");
                    dynamic_bme_addr = addr;
                    bme_found        = true;
                } else if (addr >= 0x54 && addr <= 0x57) {
                    ESP_LOGI(TAG, "   (BMV080 Particulate Laser)");
                    dynamic_bmv_addr = addr;
                    is_pro_model     = true;
                }
            }
        }

        discovery_done = true;
        ESP_LOGI(TAG, "Hardware Topology: %s Model", is_pro_model ? "PRO" : "BASE");
    }

    // MÁQUINA DE ESTADOS (Micro-Sleep / Master-Sleep)
    static RTC_DATA_ATTR float   rtc_iaq = 0, rtc_temp = 0, rtc_hum = 0;
    static RTC_DATA_ATTR uint8_t rtc_acc = 0;

    // 1. Adjuntar dispositivos al bus (persisten a través del Light Sleep)
    i2c_device_config_t scd41_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x62, .scl_speed_hz = 100000};
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &scd41_cfg, &scd41_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SCD41 device: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    i2c_device_config_t bme688_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = dynamic_bme_addr, .scl_speed_hz = 100000};
    err = i2c_master_bus_add_device(bus_handle, &bme688_cfg, &bme688_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME688 device: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    if (is_pro_model) {
        i2c_device_config_t bmv080_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = dynamic_bmv_addr, .scl_speed_hz = 100000};
        err = i2c_master_bus_add_device(bus_handle, &bmv080_cfg, &bmv080_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add BMV080 device: %s", esp_err_to_name(err));
            is_pro_model = false;
        }
    }

    // MÁQUINA DE ESTADOS (Micro-Sleep / Master-Sleep)

    if (is_pro_model) {
        if (bmv080_wrapper_init(bmv080_dev) == E_BMV080_OK) {
            ESP_LOGI(TAG, "BMV080 Láser encendido (Continuous mode)");
        } else {
            ESP_LOGE(TAG, "BMV080 initialization failed; disabling particulate measurements");
            is_pro_model = false;
        }
    }

    bool bme_initialized = false;
    for (uint8_t attempt = 1; attempt <= 3 && !bme_initialized; attempt++) {
        bme_initialized = (bme688_bsec_init(bme688_dev) == 0);
        if (!bme_initialized) {
            ESP_LOGW(TAG, "BME688 initialization failed, attempt %u/3", attempt);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!bme_initialized) {
        ESP_LOGE(TAG, "BME688 initialization failed; IAQ measurements disabled");
    }

    while (1) {
        power_wake_state_t state = power_manager_get_wake_state();

        if (state == PM_STATE_WAKE_A) {
            ESP_LOGI(TAG, "=== WAKE A: Simultaneous Trigger ===");

            // 1. Trigger SCD41 (Lento: tarda 5 segundos en procesar)
            if (retry_scd41_trigger() != ESP_OK) {
                ESP_LOGE(TAG, "SCD41 trigger failed after retries");
            }

            // 2. Procesar BME688 con BSEC 3.0 (IAQ)
            if (bme_initialized) {
                if (bme688_bsec_read_iaq(&rtc_iaq, &rtc_acc, &rtc_temp, &rtc_hum) == 0) {
                    ESP_LOGI(TAG, "BME688 Procesado correctamente (Resultados guardados para WAKE_B)");
                } else {
                    ESP_LOGE(TAG, "BME688 IAQ processing failed");
                }
            }

        } else {
            ESP_LOGI(TAG, "=== WAKE B: Data Collection ===");

            // 1. Leer SCD41
            scd41_data_t scd41_data;
            if (retry_scd41_read(&scd41_data) == ESP_OK) {
                ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                         scd41_data.temperature, scd41_data.humidity);
            } else {
                ESP_LOGE(TAG, "SCD41 measurement unavailable");
            }

            // 2. Leer BMV080
            if (is_pro_model) {
                float pm1 = 0, pm25 = 0, pm10 = 0;
                int   bmv_rslt = bmv080_wrapper_read_data(&pm1, &pm25, &pm10);
                if (bmv_rslt == 0) {
                    ESP_LOGI(TAG, "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3", pm1, pm25, pm10);
                } else {
                    ESP_LOGE(TAG, "BMV080  -> Error leyendo datos: código %d", bmv_rslt);
                }
                bmv080_wrapper_deinit(); // AHORRO DE BATERÍA
            }

            // 3. Mostrar BME688 (Ya leído en WAKE_A)
            ESP_LOGI(TAG, "BME688  -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %%", rtc_iaq, rtc_acc, rtc_temp,
                     rtc_hum);
            ESP_LOGI(TAG, "Runtime health: free heap=%u bytes, task stack minimum=%u bytes",
                     (unsigned int) esp_get_free_heap_size(),
                     (unsigned int) (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        }

        // Ejecutar política estricta de energía y delegar a la FSM
        power_manager_execute_sleep_cycle();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Wake cause: %d, reset reason: %d, free heap: %u bytes", esp_sleep_get_wakeup_cause(),
             esp_reset_reason(), (unsigned int) esp_get_free_heap_size());
    // [CRÍTICO] Liberar los pines retenidos por el RTC durante el Deep Sleep.
    // Si no hacemos esto, el controlador I2C en hardware intentará cambiar el estado de los pines
    // pero estarán bloqueados físicamente por el dominio RTC, causando un "I2C software timeout".
    gpio_hold_dis(I2C_MASTER_SDA_IO);
    gpio_hold_dis(I2C_MASTER_SCL_IO);
    gpio_deep_sleep_hold_dis();

    // Si es un Cold Boot, los sensores I2C (como el SCD41) pueden mantener la línea SDA en LOW
    // durante hasta 1 segundo mientras su silicio interno arranca. Si inicializamos el bus ahora,
    // el driver I2C fallará. Debemos esperar a que liberen el bus.
    ESP_LOGI(TAG, "Hardware cold boot. Delaying 1000ms for sensors to release I2C bus...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Inicializar la capa física del bus de forma segura y encapsulada
    i2c_master_bus_handle_t bus_handle;
    if (i2c_bus_init(&bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Critical hardware failure on I2C initialization. Rebooting...");
        esp_restart();
    }

    // Anclar la orquestación de sensores al Core 1 para proteger la telemetría futura
    // Aumentamos el stack de 4096 a 16384 bytes, ya que bmv080_serve_interrupt (Bosch SDK)
    // probablemente esté asignando buffers gigantes en el stack, causando un Stack Overflow
    // silente que aplasta la memoria del i2c_master_bus.
    BaseType_t task_created = xTaskCreatePinnedToCore(sensor_orchestration_task, "Sensor_Orchestrator", 16384,
                                                      (void *) bus_handle, 5, NULL, 1);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor orchestration task");
        esp_restart();
    }
}
