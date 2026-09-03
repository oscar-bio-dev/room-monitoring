#include "network_manager.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "network_manager";

static EventGroupHandle_t esp_now_event_group;
#define SEND_SUCCESS_BIT BIT0
#define SEND_FAIL_BIT BIT1

// Dummy Gateway MAC para pruebas. En el futuro se provisionará.
static uint8_t gateway_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
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

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Configurar canal (debe ser el mismo que el Gateway)
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, gateway_mac, 6);
    peer_info.channel = 1;
    peer_info.encrypt = false; // Se habilitará en Fase 4

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
