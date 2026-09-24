#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the BLE Provisioning loop blocking for up to timeout_sec
 *
 * @param timeout_sec Number of seconds to advertise before giving up.
 * @return true if provisioned successfully, false if timed out.
 */
bool run_ble_provisioning_loop_blocking(uint32_t timeout_sec);

#ifdef __cplusplus
}
#endif

#endif // BLE_MANAGER_H
