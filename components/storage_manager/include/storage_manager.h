#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include "telemetry.pb.h"

// Inicializa el bus SPI y monta FATFS, anexa el protobuf (con prefix de longitud), desmonta y libera SPI
esp_err_t storage_manager_save_offline(const EnvironmentalData *data);

// Inicializa SPI, monta FATFS, lee hasta max_items, devuelve la cantidad leída, desmonta y libera.
esp_err_t storage_manager_get_offline_batch(EnvironmentalData *batch, size_t max_items, size_t *out_count);

// Borra los primeros 'items_to_remove' registros del archivo (usando un archivo temporal)
esp_err_t storage_manager_clear_offline_batch(size_t items_to_remove);

#endif
