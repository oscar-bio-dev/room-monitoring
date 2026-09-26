#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#ifdef CONFIG_NVS_ENCRYPTION
#include "nvs_sec_provider.h"
#endif
#include <string.h>

static const char       *TAG = "config_manager";
static nvs_node_config_t current_config;

static const char *NVS_NAMESPACE = "node_cfg";
static const char *KEY_MAC       = "gw_mac";
static const char *KEY_INTERVAL  = "interval";

esp_err_t config_manager_init(void) {
    esp_err_t err;
#ifdef CONFIG_NVS_ENCRYPTION
    nvs_sec_cfg_t cfg;
    err = nvs_flash_read_security_cfg(NULL, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "NVS key partition empty, generating keys");
        err = nvs_flash_generate_keys(NULL, &cfg);
        if (err != ESP_OK)
            return err;
    }
    err = nvs_flash_secure_init(&cfg);
#else
    err = nvs_flash_init();
#endif

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
#ifdef CONFIG_NVS_ENCRYPTION
        err = nvs_flash_secure_init(&cfg);
#else
        err = nvs_flash_init();
#endif
    }
    ESP_ERROR_CHECK(err);

    // Set defaults
    memset(&current_config, 0, sizeof(current_config));
    current_config.monitoring_interval_sec = 300; // default 5 min
    current_config.is_provisioned          = false;

    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t mac_size = sizeof(current_config.gateway_mac);
        if (nvs_get_blob(nvs_handle, KEY_MAC, current_config.gateway_mac, &mac_size) == ESP_OK) {
            current_config.is_provisioned = true; // Assuming if MAC is stored, we are provisioned
        }

        uint32_t interval = 0;
        if (nvs_get_u32(nvs_handle, KEY_INTERVAL, &interval) == ESP_OK) {
            current_config.monitoring_interval_sec = interval;
        }

        nvs_close(nvs_handle);
    } else {
        ESP_LOGI(TAG, "NVS space empty or uninitialized. Using defaults.");
    }

    ESP_LOGI(TAG, "Config Loaded: Provisioned=%d, Interval=%u sec, MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             current_config.is_provisioned, (unsigned int) current_config.monitoring_interval_sec,
             current_config.gateway_mac[0], current_config.gateway_mac[1], current_config.gateway_mac[2],
             current_config.gateway_mac[3], current_config.gateway_mac[4], current_config.gateway_mac[5]);

    return ESP_OK;
}

const nvs_node_config_t *config_manager_get(void) {
    return &current_config;
}

esp_err_t config_manager_save(const nvs_node_config_t *new_config) {
    if (!new_config)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return err;
    }

    err = nvs_set_blob(nvs_handle, KEY_MAC, new_config->gateway_mac, sizeof(new_config->gateway_mac));
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, KEY_INTERVAL, new_config->monitoring_interval_sec);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        memcpy(&current_config, new_config, sizeof(nvs_node_config_t));
        current_config.is_provisioned = true;
        ESP_LOGI(TAG, "Configuration saved successfully.");
    } else {
        ESP_LOGE(TAG, "Error saving configuration: %s", esp_err_to_name(err));
    }

    return err;
}
