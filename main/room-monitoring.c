/*
 * Copyright (c) 2026 oscar-bio-dev
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>

#include "i2c_bus.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"

// Componentes modulares
#include "i2c_bus.h"
#include "power_manager.h"
#include "bme688_bsec_wrapper.h"
#include "bsec_datatypes.h"
#include "scd41.h"
#include "bmv080_wrapper.h"
#include "rv1805_wrapper.h"
#include "network_manager.h"
#include "storage_manager.h"
#include "telemetry.pb.h"
#include "pb_encode.h"
#include <time.h>

static const char *TAG = "app_main";

/* ────────────────────────────────────────────────────────────────────────────
 * Estado Persistente
 *
 * Light-Sleep: variables estáticas normales (RAM retenida entre ciclos).
 * Deep Sleep (legacy): RTC_DATA_ATTR para sobrevivir al reboot.
 * ──────────────────────────────────────────────────────────────────────────── */
#ifdef CONFIG_ENABLE_DEEP_SLEEP
#include "esp_attr.h"
RTC_DATA_ATTR bool           is_pro_model     = false;
RTC_DATA_ATTR bool           discovery_done   = false;
RTC_DATA_ATTR uint8_t        dynamic_bme_addr = 0x76;
RTC_DATA_ATTR uint8_t        dynamic_bmv_addr = 0x54;
RTC_DATA_ATTR uint32_t       node_sequence    = 0;
RTC_DATA_ATTR static float   rtc_iaq = 0, rtc_temp = 0, rtc_hum = 0;
RTC_DATA_ATTR static float   rtc_pressure = 0, rtc_gas_res = 0;
RTC_DATA_ATTR static float   rtc_eco2 = 0, rtc_bvoc = 0, rtc_tvoc = 0;
RTC_DATA_ATTR static uint8_t rtc_acc            = 0;
RTC_DATA_ATTR static int     debug_boot_counter = 0;
#else
static bool     is_pro_model     = false;
static bool     discovery_done   = false;
static uint8_t  dynamic_bme_addr = 0x76;
static uint8_t  dynamic_bmv_addr = 0x54;
static uint32_t node_sequence    = 0;
static float    rtc_iaq = 0, rtc_temp = 0, rtc_hum = 0;
static float    rtc_pressure = 0, rtc_gas_res = 0;
static float    rtc_eco2 = 0, rtc_bvoc = 0, rtc_tvoc = 0;
static uint8_t  rtc_acc = 0;
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Handlers de sensores I2C
 * ──────────────────────────────────────────────────────────────────────────── */
static i2c_master_dev_handle_t bme688_dev = NULL;
static i2c_master_dev_handle_t scd41_dev  = NULL;
static i2c_master_dev_handle_t bmv080_dev = NULL;
static i2c_master_dev_handle_t rv1805_dev = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────────── */
static int64_t get_current_time_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t) tv.tv_sec * 1000000000LL) + ((int64_t) tv.tv_usec * 1000LL);
}

static esp_err_t retry_scd41_trigger(void) {
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        err = scd41_trigger_single_shot(scd41_dev);
        if (err == ESP_OK)
            return ESP_OK;
        ESP_LOGW(TAG, "SCD41 trigger failed (%s), attempt %u/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

static esp_err_t retry_scd41_read(scd41_data_t *data) {
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        err = scd41_read_measurement(scd41_dev, data);
        if (err == ESP_OK)
            return ESP_OK;
        ESP_LOGW(TAG, "SCD41 read failed (%s), attempt %u/3", esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Empaquetado Protobuf + Transmisión ESP-NOW + Store & Forward
 *
 * NOTA: La radio Wi-Fi DEBE estar encendida antes de llamar esta función.
 *       El caller es responsable de network_manager_wake/sleep.
 * ──────────────────────────────────────────────────────────────────────────── */
static void transmit_telemetry(const scd41_data_t *scd41_data, const bmv080_reading_t *bmv080_data) {
    TelemetryPayload data = TelemetryPayload_init_zero;

    // Versioning
    data.protocol_version     = 1;
    data.has_protocol_version = true;
    data.schema_version       = 1;
    data.has_schema_version   = true;

    // Secuencia
    data.node_sequence     = node_sequence;
    data.has_node_sequence = true;

    // Timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    data.measured_at_ms     = ((uint64_t) tv.tv_sec * 1000ULL) + ((uint64_t) tv.tv_usec / 1000ULL);
    data.has_measured_at_ms = true;

    // BME688 — Validación de rango físico (Anti Poison-Pill)
    if (rtc_temp > -40.0f && rtc_temp < 85.0f) {
        data.temperature     = rtc_temp;
        data.has_temperature = true;
    }
    if (rtc_hum > 0.0f && rtc_hum <= 100.0f) {
        data.humidity     = rtc_hum;
        data.has_humidity = true;
    }
    if (rtc_pressure > 300.0f) {
        data.pressure     = rtc_pressure;
        data.has_pressure = true;
    } else if (rtc_pressure != 0.0f) {
        ESP_LOGW(TAG, "Presión ignorada (%.2f hPa) - Previniendo Poison Pill", rtc_pressure);
    }
    if (rtc_gas_res > 10.0f) {
        data.gas_resistance     = rtc_gas_res;
        data.has_gas_resistance = true;
    }
    data.iaq     = rtc_iaq;
    data.has_iaq = true;

    // SCD41
    if (scd41_data && scd41_data->co2 > 0) {
        data.co2     = scd41_data->co2;
        data.has_co2 = true;
    }

    // BSEC Virtual Sensors (eCO2, bVOC, TVOC)
    if (rtc_eco2 > 0.0f) {
        data.eco2     = rtc_eco2;
        data.has_eco2 = true;
    }
    if (rtc_bvoc > 0.0f) {
        data.bvoc     = rtc_bvoc;
        data.has_bvoc = true;
    }
    if (rtc_tvoc > 0.0f) {
        data.tvoc     = rtc_tvoc;
        data.has_tvoc = true;
    }

    // BMV080 — Masa + Conteo + Flags
    if (is_pro_model && bmv080_data && (bmv080_data->pm1_mass > 0 || bmv080_data->pm2_5_mass > 0)) {
        data.pm1_0      = bmv080_data->pm1_mass;
        data.has_pm1_0  = true;
        data.pm2_5      = bmv080_data->pm2_5_mass;
        data.has_pm2_5  = true;
        data.pm10_0     = bmv080_data->pm10_mass;
        data.has_pm10_0 = true;

        data.pm1_0_count      = bmv080_data->pm1_count;
        data.has_pm1_0_count  = true;
        data.pm2_5_count      = bmv080_data->pm2_5_count;
        data.has_pm2_5_count  = true;
        data.pm10_0_count     = bmv080_data->pm10_count;
        data.has_pm10_0_count = true;

        data.is_laser_obstructed     = bmv080_data->is_obstructed;
        data.has_is_laser_obstructed = true;
        data.is_pm_out_of_range      = bmv080_data->is_outside_range;
        data.has_is_pm_out_of_range  = true;
        data.laser_runtime           = bmv080_data->runtime_sec;
        data.has_laser_runtime       = true;
    }

    // Diagnóstico
    data.sleep_cycles     = node_sequence;
    data.has_sleep_cycles = true;

    // Bandera de Calibración
    data.is_calibrating     = power_manager_is_calibrating() || (rtc_acc == 0);
    data.has_is_calibrating = true;

    // TX
    uint8_t      buffer[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (pb_encode(&stream, TelemetryPayload_fields, &data)) {
        if (network_manager_send(buffer, stream.bytes_written) == ESP_OK) {
            ESP_LOGI(TAG, "📡 Telemetría enviada (%d bytes)", (int) stream.bytes_written);

            // Ventana de Recepción Downlink (50ms)
            vTaskDelay(pdMS_TO_TICKS(50));

            // Store & Forward
            TelemetryPayload batch[15];
            size_t           count = 0;
            if (storage_manager_get_offline_batch(batch, 15, &count) == ESP_OK && count > 0) {
                ESP_LOGI(TAG, "Enviando %d registros offline...", count);
                size_t success_count = 0;
                for (size_t i = 0; i < count; i++) {
                    pb_ostream_t off_stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
                    if (pb_encode(&off_stream, TelemetryPayload_fields, &batch[i])) {
                        if (network_manager_send(buffer, off_stream.bytes_written) == ESP_OK) {
                            success_count++;
                        } else {
                            break;
                        }
                    }
                }
                if (success_count > 0) {
                    storage_manager_clear_offline_batch(success_count);
                }
            }
        } else {
            ESP_LOGW(TAG, "⚠️ ESP-NOW falló. Guardando en Caja Negra...");
            storage_manager_save_offline(&data);
        }
    } else {
        ESP_LOGE(TAG, "Error empaquetando Protobuf");
    }
    node_sequence++;
}

/* ════════════════════════════════════════════════════════════════════════════
 * FASE 1: Ráfaga de Calibración (12 Pulsos de ~5s)
 *
 * Sub-bucle de 1 segundo que alimenta BSEC a 1 Hz (BSEC_SAMPLE_RATE_CONT).
 * Cada 5 ticks: lee SCD41, lee BMV080, empaqueta Protobuf y transmite.
 *
 * Matemática:
 *   - BSEC Continuous Mode espera ser llamado cada 1 segundo exacto.
 *   - SCD41 single_shot necesita 5 segundos de procesamiento químico.
 *   - BMV080 necesita ~2s de warm-up para datos limpios.
 *   - 12 pulsos × 5 ticks × 1s = 60 segundos de calentamiento total.
 * ════════════════════════════════════════════════════════════════════════════ */
static void run_warmup_phase(bool bme_initialized) {
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "🔥 FASE 1: Ráfaga de Calibración (%d pulsos)", PM_WARMUP_TOTAL_CYCLES);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    const node_config_t *cfg = power_manager_get_config();

    while (cfg->is_calibrating) {
        uint8_t pulse_num = PM_WARMUP_TOTAL_CYCLES - cfg->warmup_cycles_remaining + 1;

        // Trigger SCD41 al inicio de cada ventana de 5s
        if (retry_scd41_trigger() != ESP_OK) {
            ESP_LOGE(TAG, "SCD41 trigger failed");
        }

        // Sub-bucle de 5 ticks de 1 segundo (alimentando BSEC a 1 Hz)
        for (int tick = 0; tick < 5; tick++) {
            // 1. Purgar buffer BMV080 PRIMERO (su clock-stretching bloquea el bus I2C)
            if (is_pro_model) {
                bmv080_wrapper_read_data(NULL, NULL, NULL);
                vTaskDelay(pdMS_TO_TICKS(50)); // Cooldown I2C: el BMV080 hace clock-stretching largo
            }

            // 2. Alimentar BSEC (BME688) después de que el bus se haya liberado
            if (bme_initialized) {
                int8_t bsec_result = bme688_bsec_read_iaq(&rtc_iaq, &rtc_acc, &rtc_temp, &rtc_hum, &rtc_pressure,
                                                          &rtc_gas_res, &rtc_eco2, &rtc_bvoc, &rtc_tvoc);
                if (bsec_result == 0) {
                    ESP_LOGD(TAG, "BSEC tick %d/5: T=%.1f H=%.1f P=%.1f IAQ=%.1f", tick + 1, rtc_temp, rtc_hum,
                             rtc_pressure, rtc_iaq);
                }
            }

            // 3. Light-Sleep de ~900ms (ajustado: 1000ms - 50ms cooldown - ~50ms medición)
            esp_sleep_enable_timer_wakeup(900000ULL);
            esp_light_sleep_start();
        }

        // Leer SCD41 (ya pasaron los 5s)
        scd41_data_t scd41_data = {0};
        if (retry_scd41_read(&scd41_data) == ESP_OK) {
            ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                     scd41_data.temperature, scd41_data.humidity);
        }

        // Leer BMV080 (lectura completa: masa + conteo + flags)
        bmv080_reading_t bmv080_data = {0};
        if (is_pro_model) {
            int bmv_rslt = bmv080_wrapper_read_full(&bmv080_data);
            if (bmv_rslt == 0 && (bmv080_data.pm1_mass > 0 || bmv080_data.pm2_5_mass > 0)) {
                ESP_LOGI(TAG,
                         "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3 | "
                         "#1: %.0f | #2.5: %.0f | #10: %.0f /m3",
                         bmv080_data.pm1_mass, bmv080_data.pm2_5_mass, bmv080_data.pm10_mass, bmv080_data.pm1_count,
                         bmv080_data.pm2_5_count, bmv080_data.pm10_count);
            }
            if (bmv080_data.is_obstructed) {
                ESP_LOGW(TAG, "BMV080  ⚠️ OBSTRUCTED — lente sucia");
            }
        }

        // Log
        ESP_LOGI(
            TAG,
            "BME688  -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %% | P: %.1f hPa | Gas: %.0f Ω | eCO2: %.0f ppm",
            rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res, rtc_eco2);
        ESP_LOGI(TAG, "🔥 Calibration pulse %d/%d", pulse_num, PM_WARMUP_TOTAL_CYCLES);

        // Encender Wi-Fi → Transmitir → Apagar Wi-Fi
        network_manager_wake();
        transmit_telemetry(&scd41_data, &bmv080_data);
        network_manager_sleep();

        // Tick del warmup
        power_manager_tick_warmup();
    }

    // Transición: reconfigurar BSEC de Continuous (1 Hz) → ULP (300s) para producción
    if (bme_initialized && cfg->bme_mode == BME_MODE_BSEC_ULP) {
        ESP_LOGI(TAG, "🔄 Transitioning BSEC: Continuous (1 Hz) → ULP (300s)");
        bme688_bsec_set_sample_rate(BSEC_SAMPLE_RATE_ULP);
#ifdef CONFIG_ENABLE_DEEP_SLEEP
        bme688_bsec_mark_state_stale();
        float   _iaq, _t, _h, _p, _g;
        uint8_t _a;
        bme688_bsec_read_iaq(&_iaq, &_a, &_t, &_h, &_p, &_g, NULL, NULL, NULL);
#endif
    }

    // Detener medición BMV080 (handle sobrevive Light-Sleep para reusar en producción)
    if (is_pro_model) {
        bmv080_wrapper_stop();
    }

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "✅ FASE 1 COMPLETADA. Entrando en Fase 2 (Modo %d)", cfg->mode);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
}

#ifndef CONFIG_ENABLE_DEEP_SLEEP
/* ════════════════════════════════════════════════════════════════════════════
 * FASE 2: Producción Smart Light-Sleep — Event Loop Dinámico
 *
 * MODE_5_MIN (Batería / ULP):
 *   WAKE → Trigger SCD41 (single-shot) → Light-Sleep(5s) →
 *   WAKE → Read SCD41 + BSEC(ULP) + BMV080 → Wi-Fi Wake → TX →
 *   Wi-Fi Sleep → Light-Sleep(~295s dinámico) → [REPEAT]
 *
 * MODE_5_SEC (Continuo / Calibración):
 *   SCD41 en Periodic mode (mide cada 5s internamente).
 *   BSEC Continuous (1Hz) alimentado cada ~1s.
 *   Cada 5 ticks: leer SCD41 + BMV080 → TX → [REPEAT]
 * ════════════════════════════════════════════════════════════════════════════ */
static void run_production_cycle(bool bme_initialized) {
    const node_config_t *cfg = power_manager_get_config();

    if (cfg->mode == PM_MODE_5_SEC) {
        /* ═══════════════════════════════════════════════════════════════
         * MODE_5_SEC: Bucle Continuo con BSEC 1Hz + SCD41 Periodic
         * ═══════════════════════════════════════════════════════════════ */
        ESP_LOGI(TAG, "═══ Production: MODE_5_SEC (Continuous) ═══");

        // Iniciar SCD41 en modo periódico (mide cada ~5s internamente)
        scd41_start_periodic_measurement(scd41_dev);

        // Iniciar BMV080 una vez (permanece encendido)
        bool bmv080_active = false;
        if (is_pro_model) {
            if (bmv080_wrapper_start() == E_BMV080_OK) {
                bmv080_active = true;
                ESP_LOGI(TAG, "BMV080  ✅ Láser encendido (continuo)");
            }
        }

        uint8_t scd41_tick_counter = 0;

        while (1) {
            // 1. Alimentar BSEC (1Hz tick)
            if (bme_initialized) {
                int8_t r = bme688_bsec_read_iaq(&rtc_iaq, &rtc_acc, &rtc_temp, &rtc_hum, &rtc_pressure, &rtc_gas_res,
                                                &rtc_eco2, &rtc_bvoc, &rtc_tvoc);
                if (r == 0) {
                    ESP_LOGD(TAG, "BSEC tick: IAQ=%.1f Acc=%d T=%.1f", rtc_iaq, rtc_acc, rtc_temp);
                }
            }

            // 2. Purgar FIFO del BMV080
            bmv080_reading_t bmv080_data = {0};
            if (bmv080_active) {
                bmv080_wrapper_read_full(&bmv080_data);
            }

            scd41_tick_counter++;

            // 3. Cada 5 ticks (~5s): leer SCD41 + transmitir
            if (scd41_tick_counter >= 5) {
                scd41_tick_counter = 0;

                // Verificar si SCD41 tiene datos listos
                scd41_data_t scd41_data  = {0};
                bool         scd41_ready = false;
                scd41_get_data_ready(scd41_dev, &scd41_ready);
                if (scd41_ready) {
                    // Sensor Fusion: inyectar presión barométrica BME688 → SCD41
                    if (rtc_pressure > 300.0f && rtc_pressure < 1200.0f) {
                        scd41_set_ambient_pressure(scd41_dev, (uint16_t) rtc_pressure);
                    }
                    retry_scd41_read(&scd41_data);
                    ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                             scd41_data.temperature, scd41_data.humidity);
                }

                // Log completo
                ESP_LOGI(TAG,
                         "BME688  -> IAQ: %.1f (Acc: %d) | T: %.2f C | H: %.2f %% | P: %.1f hPa | Gas: %.0f Ω | eCO2: "
                         "%.0f ppm",
                         rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res, rtc_eco2);
                if (bmv080_active && (bmv080_data.pm1_mass > 0 || bmv080_data.pm2_5_mass > 0)) {
                    ESP_LOGI(TAG,
                             "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3 | "
                             "#1: %.0f | #2.5: %.0f | #10: %.0f /m3",
                             bmv080_data.pm1_mass, bmv080_data.pm2_5_mass, bmv080_data.pm10_mass, bmv080_data.pm1_count,
                             bmv080_data.pm2_5_count, bmv080_data.pm10_count);
                }
                if (bmv080_data.is_obstructed) {
                    ESP_LOGW(TAG, "BMV080  ⚠️ OBSTRUCTED — lente sucia");
                }
                ESP_LOGI(TAG, "Runtime: heap=%u bytes", (unsigned int) esp_get_free_heap_size());

                // Wi-Fi Wake → TX → Wi-Fi Sleep
                network_manager_wake();
                transmit_telemetry(&scd41_data, &bmv080_data);
                network_manager_sleep();
            }

            // 4. Light-Sleep dinámico hasta el próximo BSEC tick
            int64_t next_bsec_ns = bme688_bsec_get_next_call_ns();
            int64_t now_ns       = get_current_time_ns();
            int64_t sleep_us     = (next_bsec_ns - now_ns) / 1000;

            if (sleep_us < 50000LL)
                sleep_us = 50000LL; // Mínimo 50ms para evitar busy-loop
            if (sleep_us > 1500000LL)
                sleep_us = 1500000LL; // Máximo 1.5s para Continuous 1Hz

            esp_sleep_enable_timer_wakeup((uint64_t) sleep_us);
            esp_light_sleep_start();
        }

    } else {
        /* ═══════════════════════════════════════════════════════════════
         * MODE_5_MIN: Ciclo de Batería con BSEC ULP + SCD41 Single-Shot
         * ═══════════════════════════════════════════════════════════════ */
        ESP_LOGI(TAG, "═══ Production: MODE_5_MIN (Battery ULP) ═══");

        while (1) {
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
            ESP_LOGI(TAG, "📡 Production Cycle [ULP 5min]");
            ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

            /* ── PASO 1: BME688/BSEC PRIMERO (bus I2C limpio) ────────── */
            if (bme_initialized) {
                int8_t r = bme688_bsec_read_iaq(&rtc_iaq, &rtc_acc, &rtc_temp, &rtc_hum, &rtc_pressure, &rtc_gas_res,
                                                &rtc_eco2, &rtc_bvoc, &rtc_tvoc);
                if (r == 0) {
                    ESP_LOGI(TAG,
                             "BME688  ✅ BSEC ULP | IAQ: %.1f (Acc: %d) | T: %.1f | H: %.1f | P: %.1f hPa | Gas: "
                             "%.0f Ω | eCO2: %.0f ppm",
                             rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res, rtc_eco2);
                } else if (r == -2) {
                    ESP_LOGI(TAG, "BME688  ⏳ No trigger (cached: IAQ=%.1f T=%.1f)", rtc_iaq, rtc_temp);
                } else {
                    ESP_LOGE(TAG, "BME688  ❌ BSEC error");
                }
            }

            /* ── PASO 2: Cooldown I2C (200ms) ────────────────────────── */
            vTaskDelay(pdMS_TO_TICKS(200));

            /* ── PASO 3: Sensor Fusion + Trigger SCD41 Single-Shot ──── */
            // Inyectar presión barométrica BME688 → SCD41 (Sensor Fusion)
            if (rtc_pressure > 300.0f && rtc_pressure < 1200.0f) {
                scd41_set_ambient_pressure(scd41_dev, (uint16_t) rtc_pressure);
            }
            if (retry_scd41_trigger() != ESP_OK) {
                ESP_LOGE(TAG, "SCD41 trigger failed");
            }

            /* ── PASO 4: Start BMV080 Measurement ──────────────────── */
            bool bmv080_active = false;
            if (is_pro_model) {
                if (bmv080_wrapper_start() == E_BMV080_OK) {
                    ESP_LOGI(TAG, "BMV080  ✅ Láser encendido");
                    bmv080_active = true;
                } else {
                    ESP_LOGW(TAG, "BMV080  ❌ Start failed this cycle");
                }
            }

            /* Sub-bucle: Light-Sleep + BMV080 FIFO drain (~12s ≥ integration_time + 1.17s) */
            int integration_ticks = (is_pro_model && bmv080_active) ? 12 : 5;
            ESP_LOGI(TAG, "⏳ Integration sub-loop: %d × 1s Light-Sleep", integration_ticks);

            for (int tick = 0; tick < integration_ticks; tick++) {
                if (bmv080_active) {
                    bmv080_wrapper_read_data(NULL, NULL, NULL);
                }
                esp_sleep_enable_timer_wakeup(950000ULL);
                esp_light_sleep_start();
            }

            /* ── PASO 5: Recolección de Datos ────────────────────────── */
            scd41_data_t scd41_data = {0};
            if (retry_scd41_read(&scd41_data) == ESP_OK) {
                ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                         scd41_data.temperature, scd41_data.humidity);
            } else {
                ESP_LOGW(TAG, "SCD41   -> Read failed, using zeros");
                scd41_data.co2 = 0;
            }

            bmv080_reading_t bmv080_data = {0};
            if (bmv080_active) {
                int bmv_rslt = bmv080_wrapper_read_full(&bmv080_data);
                if (bmv_rslt == 0 && (bmv080_data.pm1_mass > 0 || bmv080_data.pm2_5_mass > 0)) {
                    ESP_LOGI(TAG,
                             "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3 | "
                             "#1: %.0f | #2.5: %.0f | #10: %.0f /m3",
                             bmv080_data.pm1_mass, bmv080_data.pm2_5_mass, bmv080_data.pm10_mass, bmv080_data.pm1_count,
                             bmv080_data.pm2_5_count, bmv080_data.pm10_count);
                }
                if (bmv080_data.is_obstructed) {
                    ESP_LOGW(TAG, "BMV080  ⚠️ OBSTRUCTED — lente sucia");
                }
                bmv080_wrapper_stop(); // Láser a sleep (< 30 µA), handle retenido en RAM
            }

            /* ── PASO 6: Log + Transmisión ───────────────────────────── */
            ESP_LOGI(TAG,
                     "BME688  -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %% | P: %.1f hPa | Gas: %.0f Ω | eCO2: "
                     "%.0f ppm",
                     rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res, rtc_eco2);
            ESP_LOGI(TAG, "Runtime: heap=%u bytes, stack=%u bytes", (unsigned int) esp_get_free_heap_size(),
                     (unsigned int) (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

            // Wi-Fi Wake → TX → Wi-Fi Sleep
            network_manager_wake();
            transmit_telemetry(&scd41_data, &bmv080_data);
            network_manager_sleep();

            /* ── PASO 7: Smart Light-Sleep Dinámico (~295s) ──────────── */
            int64_t next_bsec_ns = bme688_bsec_get_next_call_ns();
            int64_t now_ns       = get_current_time_ns();
            int64_t sleep_us     = (next_bsec_ns - now_ns) / 1000;

            /* Sanity bounds: mínimo 10s, máximo 310s */
            if (sleep_us < 10000000LL)
                sleep_us = 10000000LL;
            if (sleep_us > 310000000LL)
                sleep_us = 310000000LL;

            ESP_LOGI(TAG, "💤 Smart Light-Sleep (%lld s) [BSEC-synced, next_call in %lld s]", sleep_us / 1000000LL,
                     (next_bsec_ns - now_ns) / 1000000000LL);

            esp_sleep_enable_timer_wakeup((uint64_t) sleep_us);
            esp_light_sleep_start();
        }
    }
}

#else  /* CONFIG_ENABLE_DEEP_SLEEP */
/* ════════════════════════════════════════════════════════════════════════════
 * FASE 2: Producción — Legacy Deep Sleep (v2.0 encapsulado)
 * ════════════════════════════════════════════════════════════════════════════ */
static void run_production_cycle(bool bme_initialized) {
    const node_config_t *cfg = power_manager_get_config();

    while (1) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
        ESP_LOGI(TAG, "📡 Production Cycle [Mode %d] (Deep Sleep Legacy)", cfg->mode);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

        struct timeval tv;
        gettimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Timestamp: %lu s", (unsigned long) tv.tv_sec);

        if (bme_initialized) {
            if (cfg->bme_mode == BME_MODE_BSEC_ULP) {
                float   tmp_iaq, tmp_t, tmp_h, tmp_p, tmp_g;
                uint8_t tmp_a;
                int8_t  r = bme688_bsec_read_iaq(&tmp_iaq, &tmp_a, &tmp_t, &tmp_h, &tmp_p, &tmp_g, &rtc_eco2, &rtc_bvoc,
                                                 &rtc_tvoc);
                if (r == 0) {
                    rtc_iaq      = tmp_iaq;
                    rtc_acc      = tmp_a;
                    rtc_temp     = tmp_t;
                    rtc_hum      = tmp_h;
                    rtc_pressure = tmp_p;
                    rtc_gas_res  = tmp_g;
                } else if (r == -2) {
                    ESP_LOGI(TAG, "BME688  ⏳ No trigger (cached)");
                }
            } else {
                rtc_iaq = 0;
                rtc_acc = 0;
                bme688_raw_forced_read(&rtc_temp, &rtc_hum, &rtc_pressure, &rtc_gas_res);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        if (retry_scd41_trigger() != ESP_OK) {
            ESP_LOGE(TAG, "SCD41 trigger failed");
        }

        bool bmv080_active = false;
        if (is_pro_model) {
            if (bmv080_wrapper_init(bmv080_dev) == E_BMV080_OK) {
                if (bmv080_wrapper_start() == E_BMV080_OK) {
                    bmv080_active = true;
                }
            }
        }

        int integration_ticks = (is_pro_model && bmv080_active) ? 12 : 5;
        for (int tick = 0; tick < integration_ticks; tick++) {
            if (bmv080_active) {
                bmv080_wrapper_read_data(NULL, NULL, NULL);
            }
            esp_sleep_enable_timer_wakeup(950000ULL);
            esp_light_sleep_start();
        }

        scd41_data_t scd41_data = {0};
        retry_scd41_read(&scd41_data);

        bmv080_reading_t bmv080_data = {0};
        if (bmv080_active) {
            bmv080_wrapper_read_full(&bmv080_data);
            bmv080_wrapper_deinit(); // Deep Sleep: handle won't survive, must close
        }

        network_manager_init();
        transmit_telemetry(&scd41_data, &bmv080_data);
        network_manager_deinit();

        if (cfg->mode == PM_MODE_5_SEC) {
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
        } else {
            int64_t next_call_ns = bme688_bsec_get_next_call_ns();
            int64_t now_ns       = get_current_time_ns();
            int64_t deep_sleep_us;

            if (next_call_ns > 0 && cfg->bme_mode == BME_MODE_BSEC_ULP) {
                deep_sleep_us = ((next_call_ns - now_ns) / 1000) - 6000000LL;
                if (deep_sleep_us < 1000000LL)
                    deep_sleep_us = 1000000LL;
            } else {
                deep_sleep_us = 285000000LL;
            }

            ESP_LOGI(TAG, "💤 Deep-Sleep (%lld s)", deep_sleep_us / 1000000LL);

            gpio_set_level(I2C_MASTER_SDA_IO, 1);
            gpio_set_level(I2C_MASTER_SCL_IO, 1);
            gpio_hold_en(I2C_MASTER_SDA_IO);
            gpio_hold_en(I2C_MASTER_SCL_IO);
            gpio_deep_sleep_hold_en();

            esp_sleep_enable_timer_wakeup((uint64_t) deep_sleep_us);
            esp_deep_sleep_start();
        }
    }
}
#endif /* CONFIG_ENABLE_DEEP_SLEEP */

/* ────────────────────────────────────────────────────────────────────────────
 * Orquestador Principal de Sensores (Pinned to Core 1)
 * ──────────────────────────────────────────────────────────────────────────── */
static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;
    const node_config_t    *cfg        = power_manager_get_config();

    /* ── Auto-Discovery I2C (Solo en Cold Boot) ───────────────────────── */
#ifdef CONFIG_ENABLE_DEEP_SLEEP
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        discovery_done = false;
        bme688_bsec_reset_rtc_state();
    }
#endif

    if (!discovery_done) {
        ESP_LOGI(TAG, "Running I2C Auto-Discovery Scanner...");
        is_pro_model = false;

        for (uint8_t addr = 1; addr < 127; addr++) {
            if (i2c_master_probe(bus_handle, addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "=> I2C Device at 0x%02X", addr);
                if (addr == 0x62) {
                    ESP_LOGI(TAG, "   (SCD41 CO2 Sensor)");
                } else if (addr == 0x76 || addr == 0x77) {
                    ESP_LOGI(TAG, "   (BME688 IAQ Sensor)");
                    dynamic_bme_addr = addr;
                } else if (addr >= 0x54 && addr <= 0x57) {
                    ESP_LOGI(TAG, "   (BMV080 Particulate Laser)");
                    dynamic_bmv_addr = addr;
                    is_pro_model     = true;
                }
            }
        }
        discovery_done = true;
        ESP_LOGI(TAG, "Hardware Topology: %s Model", is_pro_model ? "PRO" : "BASE");
    }

    /* ── Adjuntar dispositivos al bus I2C ─────────────────────────────── */
    i2c_device_config_t scd41_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x62, .scl_speed_hz = 100000};
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &scd41_cfg, &scd41_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SCD41: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    i2c_device_config_t bme688_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = dynamic_bme_addr, .scl_speed_hz = 100000};
    err = i2c_master_bus_add_device(bus_handle, &bme688_cfg, &bme688_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME688: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    if (is_pro_model) {
        i2c_device_config_t bmv080_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = dynamic_bmv_addr, .scl_speed_hz = 100000};
        err = i2c_master_bus_add_device(bus_handle, &bmv080_cfg, &bmv080_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add BMV080: %s", esp_err_to_name(err));
            is_pro_model = false;
        }
    }

    /* ── RTC Hardware: Sincronizar reloj en CADA arranque ─────────────── */
    err = rv1805_wrapper_init(bus_handle, &rv1805_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RV1805 init failed! Timekeeping degraded.");
    } else {
        int64_t rv_time = 0;
        if (rv1805_get_time_ns(rv1805_dev, &rv_time) == ESP_OK) {
            struct timeval tv;
            tv.tv_sec  = rv_time / 1000000000ULL;
            tv.tv_usec = (rv_time % 1000000000ULL) / 1000;
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "POSIX Time synced from RV-1805.");
        }
    }

    /* ── Inicializar BMV080 (solo durante warmup) ─────────────────────── */
    if (is_pro_model && cfg->is_calibrating) {
        if (bmv080_wrapper_init(bmv080_dev) == E_BMV080_OK) {
            if (bmv080_wrapper_start() == E_BMV080_OK) {
                ESP_LOGI(TAG, "BMV080 Láser encendido (Warmup)");
                vTaskDelay(pdMS_TO_TICKS(250));
            } else {
                ESP_LOGE(TAG, "BMV080 start failed");
            }
        } else {
            ESP_LOGE(TAG, "BMV080 init failed; disabling particulate");
            is_pro_model = false;
        }
    }

    /* ── Inicializar BME688 + BSEC ────────────────────────────────────── */
    float init_sample_rate;
    if (cfg->is_calibrating) {
        init_sample_rate = BSEC_SAMPLE_RATE_CONT; // 1.0f → 1 Hz
    } else if (cfg->bme_mode == BME_MODE_BSEC_ULP) {
        init_sample_rate = BSEC_SAMPLE_RATE_ULP; // 0.0033f → 300s
    } else {
        init_sample_rate = BSEC_SAMPLE_RATE_CONT; // MODE_5_SEC: 1 Hz
    }

    bool bme_initialized = false;
    for (uint8_t attempt = 1; attempt <= 3 && !bme_initialized; attempt++) {
        bme_initialized = (bme688_bsec_init(bme688_dev, rv1805_dev, init_sample_rate) == 0);
        if (!bme_initialized) {
            ESP_LOGW(TAG, "BME688 init failed, attempt %u/3", attempt);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!bme_initialized) {
        ESP_LOGE(TAG, "BME688 init failed; IAQ disabled");
    }

    /* ── Inicializar Red (UNA SOLA VEZ en Light-Sleep) ────────────────── */
#ifndef CONFIG_ENABLE_DEEP_SLEEP
    network_manager_init();
    /* Apagar radio inmediatamente — se enciende solo para TX */
    network_manager_sleep();
#endif

    /* ═══════════════════════════════════════════════════════════════════
     * DESPACHO DE FASES
     * ═══════════════════════════════════════════════════════════════════ */
    if (cfg->is_calibrating) {
        run_warmup_phase(bme_initialized);
    }

    // Fase 2: Producción (bucle infinito)
    run_production_cycle(bme_initialized);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Punto de Entrada
 * ──────────────────────────────────────────────────────────────────────────── */
void app_main(void) {
#ifdef CONFIG_ENABLE_DEEP_SLEEP
    debug_boot_counter++;
    ESP_LOGW(TAG, "🔢 DEBUG boot_counter=%d | Wake cause: %d, reset reason: %d, free heap: %u bytes",
             debug_boot_counter, esp_sleep_get_wakeup_cause(), esp_reset_reason(),
             (unsigned int) esp_get_free_heap_size());

    gpio_hold_dis(I2C_MASTER_SDA_IO);
    gpio_hold_dis(I2C_MASTER_SCL_IO);
    gpio_deep_sleep_hold_dis();
#else
    ESP_LOGI(TAG, "🚀 Room-Monitoring v1.x (Smart Light-Sleep) | free heap: %u bytes",
             (unsigned int) esp_get_free_heap_size());
#endif

    ESP_LOGI(TAG, "Delaying 1000ms for sensors to release I2C bus...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    i2c_master_bus_handle_t bus_handle;
    if (i2c_bus_init(&bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed. Rebooting...");
        esp_restart();
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(sensor_orchestration_task, "Sensor_Orchestrator", 16384,
                                                      (void *) bus_handle, 5, NULL, 1);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        esp_restart();
    }
}
