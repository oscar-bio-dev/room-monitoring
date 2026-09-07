#include "network_manager.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "network_manager";

static EventGroupHandle_t esp_now_event_group;
#define SEND_SUCCESS_BIT BIT0
#define SEND_FAIL_BIT BIT1

static uint8_t gateway_mac[6] = {0};

/**
 * @brief Convierte un string MAC "AA:BB:CC:DD:EE:FF" a un arreglo uint8_t[6].
 */
static void parse_mac_string(const char *mac_str, uint8_t *mac_out) {
    unsigned int tmp[6];
    if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac_out[i] = (uint8_t) tmp[i];
        }
    } else {
        ESP_LOGE(TAG, "Invalid MAC format: '%s'. Expected AA:BB:CC:DD:EE:FF", mac_str);
        memset(mac_out, 0xFF, 6);
    }
}

static void esp_now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        xEventGroupSetBits(esp_now_event_group, SEND_SUCCESS_BIT);
    } else {
        xEventGroupSetBits(esp_now_event_group, SEND_FAIL_BIT);
    }
}

void network_manager_init(void) {
    if (!esp_now_event_group) {
        esp_now_event_group = xEventGroupCreate();
    }

    /* Parsear la MAC del Gateway desde Kconfig */
    parse_mac_string(CONFIG_ESPNOW_GATEWAY_MAC, gateway_mac);
    ESP_LOGI(TAG, "Gateway MAC: %02X:%02X:%02X:%02X:%02X:%02X", gateway_mac[0], gateway_mac[1], gateway_mac[2],
             gateway_mac[3], gateway_mac[4], gateway_mac[5]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Configurar canal (debe ser el mismo que el Gateway Companion C6) */
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));

#if CONFIG_ESPNOW_ENCRYPT
    /* Configurar la Primary Master Key (PMK) a nivel global */
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *) CONFIG_ESPNOW_PMK));
    ESP_LOGI(TAG, "ESP-NOW PMK configured (encrypted mode)");
#endif

    /* Registrar el peer (Gateway C6) */
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, gateway_mac, 6);
    peer_info.channel = CONFIG_ESPNOW_CHANNEL;

#if CONFIG_ESPNOW_ENCRYPT
    peer_info.encrypt = true;
    memcpy(peer_info.lmk, CONFIG_ESPNOW_LMK, 16);
    ESP_LOGI(TAG, "ESP-NOW peer registered with CCMP-128 encryption");
#else
    peer_info.encrypt = false;
    ESP_LOGW(TAG, "ESP-NOW peer registered WITHOUT encryption (lab mode)");
#endif

    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ESP-NOW peer");
    }
}

esp_err_t network_manager_send(const uint8_t *payload, size_t len) {
    xEventGroupClearBits(esp_now_event_group, SEND_SUCCESS_BIT | SEND_FAIL_BIT);

    esp_err_t err = esp_now_send(gateway_mac, payload, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW send error: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(esp_now_event_group, SEND_SUCCESS_BIT | SEND_FAIL_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(1000));

    if (bits & SEND_SUCCESS_BIT) {
        return ESP_OK;
    } else {
        return ESP_FAIL;
    }
}

void network_manager_deinit(void) {
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
}
