#include "network_manager.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "network_manager";

static EventGroupHandle_t esp_now_event_group;
#define SEND_SUCCESS_BIT BIT0
#define SEND_FAIL_BIT BIT1

typedef struct {
    uint8_t data[250];
    size_t  len;
} rx_packet_t;

static QueueHandle_t rx_command_queue = NULL;

static uint8_t gateway_mac[6] = {0};
static bool    s_initialized  = false;

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

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        xEventGroupSetBits(esp_now_event_group, SEND_SUCCESS_BIT);
    } else {
        xEventGroupSetBits(esp_now_event_group, SEND_FAIL_BIT);
    }
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len > 0 && len <= 250 && rx_command_queue != NULL) {
        rx_packet_t pkt;
        pkt.len = len;
        memcpy(pkt.data, data, len);
        xQueueSend(rx_command_queue, &pkt, 0);
    }
}

void network_manager_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized, skipping");
        return;
    }

    if (!esp_now_event_group) {
        esp_now_event_group = xEventGroupCreate();
    }

    if (!rx_command_queue) {
        rx_command_queue = xQueueCreate(5, sizeof(rx_packet_t));
    }

    /* Parsear la MAC del Gateway desde Kconfig */
    parse_mac_string(CONFIG_ESPNOW_GATEWAY_MAC, gateway_mac);
    ESP_LOGI(TAG, "Gateway MAC: %02X:%02X:%02X:%02X:%02X:%02X", gateway_mac[0], gateway_mac[1], gateway_mac[2],
             gateway_mac[3], gateway_mac[4], gateway_mac[5]);

    /* Inicializar NVS (Requerido por esp_wifi_init para calibración PHY) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Inicializar el stack TCP/IP y el event loop (requerido por esp_wifi_start para no lanzar errores) */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_ret);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Configurar canal (debe ser el mismo que el Gateway Companion C6) */
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));

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

    s_initialized = true;
    ESP_LOGI(TAG, "Network manager initialized (Wi-Fi + ESP-NOW ready)");
}

esp_err_t network_manager_wake(void) {
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi wake failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Re-establecer canal (se pierde tras wifi_stop/start) */
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_LOGD(TAG, "Wi-Fi radio awake (channel %d)", CONFIG_ESPNOW_CHANNEL);
    return ESP_OK;
}

void network_manager_sleep(void) {
    esp_wifi_stop();
    ESP_LOGD(TAG, "Wi-Fi radio stopped (Light-Sleep safe)");
}

esp_err_t network_manager_send(const uint8_t *payload, size_t len) {
    xEventGroupClearBits(esp_now_event_group, SEND_SUCCESS_BIT | SEND_FAIL_BIT);

    if (rx_command_queue) {
        xQueueReset(rx_command_queue);
    }

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

esp_err_t network_manager_receive_cmd(uint8_t *buffer, size_t *len, uint32_t timeout_ms) {
    if (!rx_command_queue || !buffer || !len)
        return ESP_ERR_INVALID_ARG;

    rx_packet_t pkt;
    if (xQueueReceive(rx_command_queue, &pkt, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *len = pkt.len;
        memcpy(buffer, pkt.data, pkt.len);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

#ifdef CONFIG_ENABLE_DEEP_SLEEP
void network_manager_deinit(void) {
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialized = false;
}
#endif
