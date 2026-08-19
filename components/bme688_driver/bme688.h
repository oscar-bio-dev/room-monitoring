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
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// BME688 Default I2C Addresses
#define BME688_I2C_ADDR_PRIMARY   0x76
#define BME688_I2C_ADDR_SECONDARY 0x77

/**
 * @brief BME688 Calibration Data Structure
 * Holds all factory-programmed calibration coefficients.
 * @note Ref: Datasheet BME688, Chapter 5 (Global memory map and register description)
 */
typedef struct {
    uint16_t par_t1;
    int16_t  par_t2;
    int8_t   par_t3;

    uint16_t par_p1;
    int16_t  par_p2;
    int8_t   par_p3;
    int16_t  par_p4;
    int16_t  par_p5;
    int8_t   par_p6;
    int8_t   par_p7;
    int16_t  par_p8;
    int16_t  par_p9;
    uint8_t  par_p10;

    uint16_t par_h1;
    uint16_t par_h2;
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;

    int8_t   par_g1;
    int16_t  par_g2;
    int8_t   par_g3;
    uint8_t  res_heat_range;
    int8_t   res_heat_val;
} bme688_calib_data_t;

/**
 * @brief BME688 Sensor Output Data
 * @note Ref: Datasheet BME688, Section 3.7 Data readout
 */
typedef struct {
    float temperature;   // in Celsius
    float pressure;      // in hPa
    float humidity;      // in %RH
    float gas_res;       // in Ohms (Gas Resistance)
    
    uint8_t gas_valid;   // 1 if gas measurement was successful
    uint8_t heat_stab;   // 1 if heater was stable during measurement
} bme688_data_t;

/**
 * @brief Device handle context for BME688
 */
typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    bme688_calib_data_t calib;
    double t_fine; // Intermediate variable used for compensation
} bme688_device_t;

/**
 * @brief Initialize the BME688 sensor.
 * Sets up I2C device, resets the sensor, and reads calibration data.
 * @note Ref: Datasheet BME688, Section 3.5.1 Quick start
 *
 * @param bus_handle Initialized I2C master bus handle.
 * @param i2c_addr I2C address of the BME688 (0x76 or 0x77).
 * @param dev Pointer to the device context struct to initialize.
 * @param cached_calib Optional pointer to RTC cached calibration data (NULL to read from sensor).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bme688_init(i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr, bme688_device_t *dev, const bme688_calib_data_t *cached_calib);

/**
 * @brief Triggers a single "Forced Mode" measurement.
 * In forced mode, the sensor wakes up, takes one T/P/H/G measurement, and goes back to sleep.
 * @note Ref: Datasheet BME688, Section 3.4 Sensor modes (Forced mode)
 * 
 * @param dev Pointer to the initialized device context.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bme688_trigger_forced_measurement(bme688_device_t *dev);

/**
 * @brief Reads the resulting data from the sensor and applies compensation formulas.
 * Should be called after bme688_trigger_forced_measurement() and allowing enough time for conversion.
 * @note Ref: Datasheet BME688, Section 3.7 Data readout and compensation
 * 
 * @param dev Pointer to the initialized device context.
 * @param out_data Pointer to the struct where the data will be stored.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bme688_read_data(bme688_device_t *dev, bme688_data_t *out_data);
