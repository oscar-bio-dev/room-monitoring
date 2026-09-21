#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  gateway_mac[6];          // ESP-NOW Gateway MAC
    uint32_t monitoring_interval_sec; // Sampling interval
    bool     is_provisioned;          // true if MAC exists and provisioned
} nvs_node_config_t;

/**
 * @brief Initialize NVS and load config
 */
esp_err_t config_manager_init(void);

/**
 * @brief Get the current config from RAM
 */
const nvs_node_config_t *config_manager_get(void);

/**
 * @brief Save the configuration back to NVS
 */
esp_err_t config_manager_save(const nvs_node_config_t *new_config);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
