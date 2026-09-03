#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Inicializa Wi-Fi y ESP-NOW
void network_manager_init(void);

// Envía un payload por ESP-NOW bloqueando hasta que haya confirmación de recepción o fallo
// Retorna ESP_OK si el Gateway respondió (ACK), ESP_FAIL si no.
esp_err_t network_manager_send(const uint8_t *payload, size_t len);

// Deshabilita Wi-Fi para conservar energía
void network_manager_deinit(void);

#endif
