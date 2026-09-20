#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Inicializa Wi-Fi + ESP-NOW completamente (NVS, netif, peer).
 *        Llamar UNA SOLA VEZ en el arranque. Tras esta llamada, la radio
 *        queda encendida y lista para transmitir.
 */
void network_manager_init(void);

/**
 * @brief Enciende la radio Wi-Fi tras un Light-Sleep.
 *        Solo llama esp_wifi_start() + set_channel (rápido, ~5ms).
 *        ESP-NOW y el peer siguen registrados en RAM.
 */
esp_err_t network_manager_wake(void);

/**
 * @brief Apaga la radio Wi-Fi antes de entrar en Light-Sleep.
 *        Solo llama esp_wifi_stop(). No libera memoria ni peers.
 */
void network_manager_sleep(void);

/**
 * @brief Envía un payload por ESP-NOW bloqueando hasta ACK MAC o fallo.
 * @return ESP_OK si el hardware transmitió, ESP_FAIL si no.
 */
esp_err_t network_manager_send(const uint8_t *payload, size_t len);

/**
 * @brief Espera de forma asíncrona (hasta timeout_ms) la recepción de un comando por ESP-NOW.
 * @param buffer Buffer donde se copiarán los datos recibidos (ej. array de 250 bytes)
 * @param len Puntero donde se escribirá la longitud de los datos recibidos
 * @param timeout_ms Tiempo máximo de espera en milisegundos
 * @return ESP_OK si se recibió un paquete, ESP_ERR_TIMEOUT si expiró el tiempo
 */
esp_err_t network_manager_receive_cmd(uint8_t *buffer, size_t *len, uint32_t timeout_ms);

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/**
 * @brief Destructor completo: deinit ESP-NOW + wifi_stop + wifi_deinit.
 *        Solo necesario en Deep Sleep (el reboot implica re-init total).
 */
void network_manager_deinit(void);
#endif

#endif
