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
#include "bme688.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BME688";

// Basic Register Addresses
#define BME688_REG_STATUS 0x73
#define BME688_REG_RESET 0xE0
#define BME688_REG_CHIP_ID 0xD0
#define BME688_REG_CTRL_MEAS 0x74
#define BME688_REG_CTRL_HUM 0x72
#define BME688_REG_CTRL_GAS_1 0x71
#define BME688_REG_RES_HEAT_0 0x5A
#define BME688_REG_GAS_WAIT_0 0x64
#define BME688_REG_CONFIG 0x75

// Data Registers Start
#define BME688_REG_PRESS_MSB 0x1F

// Helper to read multiple bytes
static esp_err_t read_regs(bme688_device_t *dev, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, data, len, 1000 / portTICK_PERIOD_MS);
}

// Helper to write a single byte
static esp_err_t write_reg(bme688_device_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev->i2c_dev, buf, 2, 1000 / portTICK_PERIOD_MS);
}

/**
 * @brief Calculate target heater resistance from target temperature.
 * @note Ref: Datasheet BME688, Section 3.6.5 Gas sensor heating and measurement (Floating point)
 *
 * @param dev Pointer to device context containing calibration data
 * @param target_temp Target heater temperature in Celsius (e.g. 300.0)
 * @param amb_temp Ambient temperature in Celsius (e.g. 25.0)
 * @return uint8_t Code for heater resistance register (res_heat_x)
 */
static uint8_t calc_res_heat(bme688_device_t *dev, float target_temp, float amb_temp) {
    bme688_calib_data_t *c    = &dev->calib;
    double               var1 = ((double) c->par_g1 / 16.0) + 49.0;
    double               var2 = (((double) c->par_g2 / 32768.0) * 0.0005) + 0.00235;
    double               var3 = (double) c->par_g3 / 1024.0;
    double               var4 = var1 * (1.0 + (var2 * (double) target_temp));
    double               var5 = var4 + (var3 * (double) amb_temp);

    uint8_t res_heat = (uint8_t) (3.4 * ((var5 * (4.0 / (4.0 + (double) c->res_heat_range)) *
                                          (1.0 / (1.0 + ((double) c->res_heat_val * 0.002)))) -
                                         25));
    return res_heat;
}

esp_err_t bme688_init(i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr, bme688_device_t *dev,
                      const bme688_calib_data_t *cached_calib) {
    if (!bus_handle || !dev || (i2c_addr != BME688_I2C_ADDR_PRIMARY && i2c_addr != BME688_I2C_ADDR_SECONDARY))
        return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_addr,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK)
        return err;

    // Check Chip ID
    uint8_t chip_id = 0;
    err             = read_regs(dev, BME688_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK || chip_id != 0x61) {
        ESP_LOGE(TAG, "Failed to find BME688. Chip ID: 0x%02X", chip_id);
        return ESP_FAIL;
    }

    // Soft reset
    err = write_reg(dev, BME688_REG_RESET, 0xB6);
    if (err != ESP_OK)
        return err;
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Si tenemos calibración en la memoria RTC, la usamos y ahorramos milisegundos y batería
    if (cached_calib != NULL) {
        dev->calib = *cached_calib;
        ESP_LOGI(TAG, "Calibration data successfully loaded from RTC memory.");
        return ESP_OK;
    }

    // Read Calibration Data Block 1 (0x8A to 0xA0) and Block 2 (0xE1 to 0xF0)
    // To simplify, we will read them byte by byte or in small chunks
    uint8_t c1[24]; // 0x8A to 0xA1
    err = read_regs(dev, 0x8A, c1, 24);
    if (err != ESP_OK)
        return err;

    uint8_t c2[16]; // 0xE1 to 0xF0
    err = read_regs(dev, 0xE1, c2, 16);
    if (err != ESP_OK)
        return err;

    uint8_t c3[3];                     // 0x00, 0x02
    err = read_regs(dev, 0x00, c3, 3); // Reads 0x00 to 0x02
    if (err != ESP_OK)
        return err;

    bme688_calib_data_t *calib = &dev->calib;
    // T
    calib->par_t1 = (uint16_t) ((c2[0xEA - 0xE1] << 8) | c2[0xE9 - 0xE1]);
    calib->par_t2 = (int16_t) ((c1[0x8B - 0x8A] << 8) | c1[0x8A - 0x8A]);
    calib->par_t3 = (int8_t) (c1[0x8C - 0x8A]);

    // P
    calib->par_p1  = (uint16_t) ((c1[0x8F - 0x8A] << 8) | c1[0x8E - 0x8A]);
    calib->par_p2  = (int16_t) ((c1[0x91 - 0x8A] << 8) | c1[0x90 - 0x8A]);
    calib->par_p3  = (int8_t) (c1[0x92 - 0x8A]);
    calib->par_p4  = (int16_t) ((c1[0x95 - 0x8A] << 8) | c1[0x94 - 0x8A]);
    calib->par_p5  = (int16_t) ((c1[0x97 - 0x8A] << 8) | c1[0x96 - 0x8A]);
    calib->par_p6  = (int8_t) (c1[0x99 - 0x8A]);
    calib->par_p7  = (int8_t) (c1[0x98 - 0x8A]);
    calib->par_p8  = (int16_t) ((c1[0x9D - 0x8A] << 8) | c1[0x9C - 0x8A]);
    calib->par_p9  = (int16_t) ((c1[0x9F - 0x8A] << 8) | c1[0x9E - 0x8A]);
    calib->par_p10 = (uint8_t) (c1[0xA0 - 0x8A]);

    // H
    calib->par_h1 = (uint16_t) ((c2[0xE3 - 0xE1] << 4) | (c2[0xE2 - 0xE1] & 0x0F));
    calib->par_h2 = (uint16_t) ((c2[0xE1 - 0xE1] << 4) | (c2[0xE2 - 0xE1] >> 4));
    calib->par_h3 = (int8_t) (c2[0xE4 - 0xE1]);
    calib->par_h4 = (int8_t) (c2[0xE5 - 0xE1]);
    calib->par_h5 = (int8_t) (c2[0xE6 - 0xE1]);
    calib->par_h6 = (uint8_t) (c2[0xE7 - 0xE1]);
    calib->par_h7 = (int8_t) (c2[0xE8 - 0xE1]);

    // G
    calib->par_g1         = (int8_t) (c2[0xED - 0xE1]);
    calib->par_g2         = (int16_t) ((c2[0xEC - 0xE1] << 8) | c2[0xEB - 0xE1]);
    calib->par_g3         = (int8_t) (c2[0xEE - 0xE1]);
    calib->res_heat_range = (c3[0x02] & 0x30) >> 4;
    calib->res_heat_val   = (int8_t) c3[0x00];

    ESP_LOGI(TAG, "Calibration data successfully read.");
    return ESP_OK;
}

esp_err_t bme688_trigger_forced_measurement(bme688_device_t *dev) {
    if (!dev || !dev->i2c_dev)
        return ESP_ERR_INVALID_ARG;

    // 1. Oversampling: Humidity 1x
    esp_err_t err = write_reg(dev, BME688_REG_CTRL_HUM, 0x01); // osrs_h = 1 (1x)
    if (err != ESP_OK)
        return err;

    // 2. Set Gas Heater (Step 0): 300C for 100ms
    // Wait time: 100ms. Code = 0x59 (from datasheet example 3.5.1)
    err = write_reg(dev, BME688_REG_GAS_WAIT_0, 0x59);
    if (err != ESP_OK)
        return err;

    // Calculate and set target resistance for 300C (assuming 25C ambient as baseline)
    uint8_t res_heat = calc_res_heat(dev, 300.0, 25.0);
    err              = write_reg(dev, BME688_REG_RES_HEAT_0, res_heat);
    if (err != ESP_OK)
        return err;

    // Enable gas conversion, set heater profile index to 0
    err = write_reg(dev, BME688_REG_CTRL_GAS_1, 0x20); // run_gas = 1, nb_conv = 0
    if (err != ESP_OK)
        return err;

    // 3. Oversampling T=2x, P=16x, Mode = Forced
    // osrs_t (bits 7:5) = 010 (2) -> 2x
    // osrs_p (bits 4:2) = 101 (5) -> 16x
    // mode (bits 1:0) = 01 (1) -> Forced
    uint8_t ctrl_meas_val = (2 << 5) | (5 << 2) | 1;
    err                   = write_reg(dev, BME688_REG_CTRL_MEAS, ctrl_meas_val);

    return err;
}

esp_err_t bme688_read_data(bme688_device_t *dev, bme688_data_t *out_data) {
    if (!dev || !dev->i2c_dev || !out_data)
        return ESP_ERR_INVALID_ARG;

    uint8_t   data[15];
    esp_err_t err = read_regs(dev, BME688_REG_PRESS_MSB, data, 15);
    if (err != ESP_OK)
        return err;

    // Casteo explícito a uint32_t para prevenir overflow de enteros de 32 bits (promoción implícita en C)
    uint32_t press_adc  = ((uint32_t) data[0] << 12) | ((uint32_t) data[1] << 4) | (data[2] >> 4);
    uint32_t temp_adc   = ((uint32_t) data[3] << 12) | ((uint32_t) data[4] << 4) | (data[5] >> 4);
    uint32_t hum_adc    = ((uint32_t) data[6] << 8) | data[7];
    uint16_t gas_adc    = ((uint16_t) data[13] << 2) | (data[14] >> 6);
    uint8_t  gas_range  = data[14] & 0x0F;
    out_data->gas_valid = (data[14] & 0x20) >> 5;
    out_data->heat_stab = (data[14] & 0x10) >> 4;

    bme688_calib_data_t *c = &dev->calib;

    // --- TEMPERATURE COMPENSATION (Floating Point) ---
    // Ref: Datasheet BME688, Section 3.6.1 Temperature measurement, Table 13
    double var1_t = (((double) temp_adc / 16384.0) - ((double) c->par_t1 / 1024.0)) * (double) c->par_t2;
    double var2_t = ((((double) temp_adc / 131072.0) - ((double) c->par_t1 / 8192.0)) *
                     (((double) temp_adc / 131072.0) - ((double) c->par_t1 / 8192.0))) *
                    ((double) c->par_t3 * 16.0);
    dev->t_fine           = var1_t + var2_t;
    out_data->temperature = (float) (dev->t_fine / 5120.0);

    // --- PRESSURE COMPENSATION (Floating Point) ---
    // Ref: Datasheet BME688, Section 3.6.2 Pressure measurement, Table 14
    double var1_p     = ((double) dev->t_fine / 2.0) - 64000.0;
    double var2_p     = var1_p * var1_p * ((double) c->par_p6 / 131072.0);
    var2_p            = var2_p + (var1_p * (double) c->par_p5 * 2.0);
    var2_p            = (var2_p / 4.0) + ((double) c->par_p4 * 65536.0);
    var1_p            = ((((double) c->par_p3 * var1_p * var1_p) / 16384.0) + ((double) c->par_p2 * var1_p)) / 524288.0;
    var1_p            = (1.0 + (var1_p / 32768.0)) * (double) c->par_p1;
    double press_comp = 1048576.0 - (double) press_adc;
    if (var1_p != 0.0) {
        press_comp    = ((press_comp - (var2_p / 4096.0)) * 6250.0) / var1_p;
        var1_p        = ((double) c->par_p9 * press_comp * press_comp) / 2147483648.0;
        var2_p        = press_comp * ((double) c->par_p8 / 32768.0);
        double var3_p = (press_comp / 256.0) * (press_comp / 256.0) * (press_comp / 256.0) * (c->par_p10 / 131072.0);
        press_comp    = press_comp + (var1_p + var2_p + var3_p + ((double) c->par_p7 * 128.0)) / 16.0;
    } else {
        press_comp = 0;
    }
    out_data->pressure = (float) (press_comp / 100.0); // Convert Pa to hPa

    // --- HUMIDITY COMPENSATION (Floating Point) ---
    // Ref: Datasheet BME688, Section 3.6.3 Humidity measurement, Table 15
    double temp_comp = dev->t_fine / 5120.0;
    double var1_h    = (double) hum_adc - (((double) c->par_h1 * 16.0) + (((double) c->par_h3 / 2.0) * temp_comp));
    double var2_h =
        var1_h * (((double) c->par_h2 / 262144.0) * (1.0 + (((double) c->par_h4 / 16384.0) * temp_comp) +
                                                     (((double) c->par_h5 / 1048576.0) * temp_comp * temp_comp)));
    double var3_h      = (double) c->par_h6 / 16384.0;
    double var4_h      = (double) c->par_h7 / 2097152.0;
    double hum_comp    = var2_h + ((var3_h + (var4_h * temp_comp)) * var2_h * var2_h);
    out_data->humidity = (float) hum_comp;

    // --- GAS RESISTANCE COMPENSATION (Floating Point) ---
    // Ref: Datasheet BME688, Section 3.7.1 Gas resistance readout, Table 17
    double calc_gas_res = 0.0;
    if (out_data->gas_valid) {
        uint32_t var1_g = (uint32_t) (262144) >> gas_range;
        int32_t  var2_g = (int32_t) gas_adc - 512;
        var2_g *= 3;
        var2_g       = 4096 + var2_g;
        calc_gas_res = 1000000.0f * (float) var1_g / (float) var2_g;
    }
    out_data->gas_res = (float) calc_gas_res;

    return ESP_OK;
}
