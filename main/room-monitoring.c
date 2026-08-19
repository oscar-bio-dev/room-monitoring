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
#include "driver/i2c_master.h"
#include "bme688.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_attr.h"
#include "esp_sleep.h"

static const char *TAG = "app_main";

// Persistencia en memoria RTC para la fase 2 (Deep Sleep)
RTC_DATA_ATTR bme688_calib_data_t rtc_calib_data;
RTC_DATA_ATTR bool rtc_calib_valid = false;

// Device handler en scope global para que la tarea no apunte a memoria destruida
static bme688_device_t bme688_dev;

#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22

// Rutina de Auto-Recuperación (Sanity Check) del Bus I2C
static void sanity_check_i2c(void) {
    ESP_LOGI(TAG, "Performing I2C bus sanity check...");
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    if (gpio_get_level(I2C_MASTER_SDA_IO) == 0) {
        ESP_LOGW(TAG, "SDA line is LOW! I2C slave is stuck. Injecting clock pulses...");
        for (int i = 0; i < 9; i++) {
            gpio_set_level(I2C_MASTER_SCL_IO, 0);
            ets_delay_us(5);
            gpio_set_level(I2C_MASTER_SCL_IO, 1);
            ets_delay_us(5);
        }
        
        // Condición STOP
        gpio_set_level(I2C_MASTER_SDA_IO, 0);
        ets_delay_us(5);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        ets_delay_us(5);
        gpio_set_level(I2C_MASTER_SDA_IO, 1);
        ets_delay_us(10);
        
        if (gpio_get_level(I2C_MASTER_SDA_IO) == 0) {
            ESP_LOGE(TAG, "Failed to recover I2C bus!");
        } else {
            ESP_LOGI(TAG, "I2C bus recovered successfully.");
        }
    } else {
        ESP_LOGI(TAG, "I2C bus sanity check passed. Lines are high.");
    }
    
    gpio_reset_pin(I2C_MASTER_SDA_IO);
    gpio_reset_pin(I2C_MASTER_SCL_IO);
}

// Tarea del sensor anclada al Core 1
static void bme688_sensor_task(void *pvParameters) {
    bme688_device_t *dev = (bme688_device_t *)pvParameters;
    bme688_data_t sensor_data;
    
    ESP_LOGI(TAG, "Waking up from Deep Sleep... Triggering forced measurement...");
    esp_err_t err = bme688_trigger_forced_measurement(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger BME688 measurement: %s", esp_err_to_name(err));
    } else {
        // El sensor BME688 tarda aprox ~150ms en completar una lectura forzada de Gas
        vTaskDelay(pdMS_TO_TICKS(150));
        
        err = bme688_read_data(dev, &sensor_data);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Temp: %.2f °C | Hum: %.2f %% | Press: %.2f hPa | Gas Res: %.0f Ohms (Valid: %d, Stab: %d)", 
                     sensor_data.temperature, sensor_data.humidity, sensor_data.pressure, 
                     sensor_data.gas_res, sensor_data.gas_valid, sensor_data.heat_stab);
            
            // Aquí en la Fase 3/4 evaluaremos el dato (Edge AI) para decidir si emitimos telemetría
            // ...
        } else {
            ESP_LOGE(TAG, "Failed to read from BME688: %s", esp_err_to_name(err));
        }
    }
    
    // Configurar el temporizador RTC para despertar en 3 segundos
    const uint64_t WAKEUP_TIME_US = 3000000ULL;
    ESP_LOGI(TAG, "Entering Deep Sleep for %llu seconds...", WAKEUP_TIME_US / 1000000ULL);
    
    esp_sleep_enable_timer_wakeup(WAKEUP_TIME_US);
    
    // Entrar en Deep Sleep (la memoria RTC retendrá rtc_calib_data)
    esp_deep_sleep_start();
}

void app_main(void)
{
    sanity_check_i2c();
    
    ESP_LOGI(TAG, "Initializing I2C Master Bus...");
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1, // Any free port
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

    ESP_LOGI(TAG, "Initializing BME688 Sensor...");
    
    // Pasamos el caché RTC si es válido, si no, NULL para que lea de fábrica
    esp_err_t err = bme688_init(bus_handle, BME688_I2C_ADDR_PRIMARY, &bme688_dev, rtc_calib_valid ? &rtc_calib_data : NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BME688 at 0x76: %s", esp_err_to_name(err));
        return;
    }

    // Guardar calibración en RTC si es la primera vez
    if (!rtc_calib_valid) {
        rtc_calib_data = bme688_dev.calib;
        rtc_calib_valid = true;
        ESP_LOGI(TAG, "Calibration data saved to RTC memory.");
    }

    // Fijamos la tarea al Core 1 (App Core) con prioridad 5
    xTaskCreatePinnedToCore(bme688_sensor_task, "BME688_Task", 4096, &bme688_dev, 5, NULL, 1);
}
