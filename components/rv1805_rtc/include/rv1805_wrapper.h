#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RV1805_I2C_ADDR 0x69

/**
 * @brief Initialize RV1805 I2C handle
 *
 * @param bus_handle The master I2C bus handle
 * @param rv_dev_handle Pointer to store the created device handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rv1805_wrapper_init(i2c_master_bus_handle_t bus_handle, i2c_master_dev_handle_t *rv_dev_handle);

/**
 * @brief Get monotonic time from RV-1805 in nanoseconds
 * This value is robust against deep sleep resets and perfectly suited for BSEC 3.0
 *
 * @param rv_dev_handle The RV-1805 device handle
 * @param time_ns Pointer to store the calculated nanoseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t rv1805_get_time_ns(i2c_master_dev_handle_t rv_dev_handle, int64_t *time_ns);

#ifdef __cplusplus
}
#endif
