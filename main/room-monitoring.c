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
#include "esp_attr.h"
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
 * Estado Persistente en RTC SRAM (Sobrevive al Deep Sleep)
 * ──────────────────────────────────────────────────────────────────────────── */
RTC_DATA_ATTR bool     is_pro_model     = false;
RTC_DATA_ATTR bool     discovery_done   = false;
RTC_DATA_ATTR uint8_t  dynamic_bme_addr = 0x76;
RTC_DATA_ATTR uint8_t  dynamic_bmv_addr = 0x54;
RTC_DATA_ATTR uint32_t node_sequence    = 0;

// Cache de última lectura BSEC (persiste entre Deep Sleep cycles)
RTC_DATA_ATTR static float   rtc_iaq = 0, rtc_temp = 0, rtc_hum = 0;
RTC_DATA_ATTR static float   rtc_pressure = 0, rtc_gas_res = 0;
RTC_DATA_ATTR static uint8_t rtc_acc = 0;

/* ── Trampas de Depuración (BORRAR en producción) ─────────────────────── */
RTC_DATA_ATTR static int debug_boot_counter = 0;

/* ────────────────────────────────────────────────────────────────────────────
 * Handlers de sensores I2C (no sobreviven al Deep Sleep)
 * ──────────────────────────────────────────────────────────────────────────── */
static i2c_master_dev_handle_t bme688_dev = NULL;
static i2c_master_dev_handle_t scd41_dev  = NULL;
static i2c_master_dev_handle_t bmv080_dev = NULL;
static i2c_master_dev_handle_t rv1805_dev = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers de resiliencia I2C
 * ──────────────────────────────────────────────────────────────────────────── */
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
 * ──────────────────────────────────────────────────────────────────────────── */
static void transmit_telemetry(const scd41_data_t *scd41_data, float pm1, float pm25, float pm10) {
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

    // BMV080
    if (is_pro_model && (pm1 > 0 || pm25 > 0)) {
        data.pm1_0      = pm1;
        data.has_pm1_0  = true;
        data.pm2_5      = pm25;
        data.has_pm2_5  = true;
        data.pm10_0     = pm10;
        data.has_pm10_0 = true;
    }

    // Diagnóstico
    data.sleep_cycles     = node_sequence;
    data.has_sleep_cycles = true;

    // Bandera de Calibración: señal al Backend que IAQ no es confiable.
    // Se activa en 2 casos:
    //   1. Warmup Fase 1 (12 pulsos iniciales)
    //   2. Anchor Point post-Deep-Sleep (BSEC retorna n_outputs=0,
    //      accuracy=0, pero T/H/P/Gas son RAW válidos del hardware)
    data.is_calibrating     = power_manager_is_calibrating() || (rtc_acc == 0);
    data.has_is_calibrating = true;

    // TX
    network_manager_init();
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
    network_manager_deinit();
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
                int8_t bsec_result =
                    bme688_bsec_read_iaq(&rtc_iaq, &rtc_acc, &rtc_temp, &rtc_hum, &rtc_pressure, &rtc_gas_res);
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

        // Leer BMV080
        float pm1 = 0, pm25 = 0, pm10 = 0;
        if (is_pro_model) {
            int bmv_rslt = bmv080_wrapper_read_data(&pm1, &pm25, &pm10);
            if (bmv_rslt == 0 && (pm1 > 0 || pm25 > 0)) {
                ESP_LOGI(TAG, "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3", pm1, pm25, pm10);
            }
        }

        // Log
        ESP_LOGI(TAG, "BME688  -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %% | P: %.1f hPa | Gas: %.0f Ω",
                 rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res);
        ESP_LOGI(TAG, "🔥 Calibration pulse %d/%d", pulse_num, PM_WARMUP_TOTAL_CYCLES);

        // Transmitir
        transmit_telemetry(&scd41_data, pm1, pm25, pm10);

        // Tick del warmup
        power_manager_tick_warmup();
    }

    // Transición: reconfigurar BSEC de Continuous (1 Hz) → ULP (300s) si MODE_5_MIN
    // CRÍTICO: Debemos hacer una última lectura BSEC para que el state blob refleje
    // la nueva suscripción ULP. Sin esto, el blob persistido en RTC sigue siendo de
    // CONTINUOUS mode y BSEC se desincroniza tras el Deep Sleep.
    if (bme_initialized && cfg->bme_mode == BME_MODE_BSEC_ULP) {
        ESP_LOGI(TAG, "🔄 Transitioning BSEC: Continuous (1 Hz) → ULP (300s)");
        bme688_bsec_set_sample_rate(BSEC_SAMPLE_RATE_ULP);
        // Marcar el state blob como "stale" para que el primer ciclo ULP
        // post-Deep-Sleep NO restaure un blob de CONTINUOUS (causa n_outputs=0 infinito)
        bme688_bsec_mark_state_stale();
        // Forzar una medición final para persistir el state blob con ULP
        float   _iaq, _t, _h, _p, _g;
        uint8_t _a;
        bme688_bsec_read_iaq(&_iaq, &_a, &_t, &_h, &_p, &_g);
    }

    // Apagar BMV080 antes de entrar en producción
    if (is_pro_model) {
        bmv080_wrapper_deinit();
    }

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "✅ FASE 1 COMPLETADA. Entrando en Fase 2 (Modo %d)", cfg->mode);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
}

/* ════════════════════════════════════════════════════════════════════════════
 * FASE 2: Ciclo de Producción — Flujo Secuencial
 *
 * Arquitectura simplificada sin máquina de estados WAKE_A/WAKE_B.
 * El sub-bucle de integración (10×1s Light-Sleep con polling BMV080) garantiza
 * que el láser tenga los ~10 segundos que necesita desde un cold-init para
 * producir lecturas no-cero (confirmado empíricamente en la Fase 1).
 *
 * Timing por modo (total ≈ BSEC target):
 *   MODE_5_MIN: ~5s activo + 10s integración + 285s Deep = 300s (ULP match)
 *   MODE_1_MIN: ~5s activo + 10s integración +  45s Deep =  60s
 *   MODE_5_SEC: ~5s activo +  5s integración +  0s       =  10s (Light-Sleep)
 * ════════════════════════════════════════════════════════════════════════════ */
static void run_production_cycle(bool bme_initialized) {
    const node_config_t *cfg = power_manager_get_config();

    while (1) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
        ESP_LOGI(TAG, "📡 Production Cycle [Mode %d]", cfg->mode);
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

        struct timeval tv;
        gettimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Timestamp: %lu s", (unsigned long) tv.tv_sec);

        /* ── PASO 1: BME688 PRIMERO (bus I2C 100%% limpio) ────────────
         * El BMV080 hace clock-stretching que corrompe el bus.
         * BSEC debe acceder al BME688 ANTES de encender el láser. */
        if (bme_initialized) {
            if (cfg->bme_mode == BME_MODE_BSEC_ULP) {
                float   tmp_iaq, tmp_t, tmp_h, tmp_p, tmp_g;
                uint8_t tmp_a;
                int8_t  r = bme688_bsec_read_iaq(&tmp_iaq, &tmp_a, &tmp_t, &tmp_h, &tmp_p, &tmp_g);
                if (r == 0) {
                    rtc_iaq      = tmp_iaq;
                    rtc_acc      = tmp_a;
                    rtc_temp     = tmp_t;
                    rtc_hum      = tmp_h;
                    rtc_pressure = tmp_p;
                    rtc_gas_res  = tmp_g;
                    ESP_LOGI(
                        TAG,
                        "BME688  ✅ BSEC ULP | IAQ: %.1f (Acc: %d) | T: %.1f | H: %.1f | P: %.1f hPa | Gas: %.0f Ω",
                        rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res);
                } else if (r == -2) {
                    ESP_LOGI(TAG, "BME688  ⏳ No trigger (RTC cached: IAQ=%.1f T=%.1f)", rtc_iaq, rtc_temp);
                } else {
                    ESP_LOGE(TAG, "BME688  ❌ BSEC error");
                }
            } else {
                // RAW_FORCED: lectura directa sin BSEC (MODE_5_SEC / MODE_1_MIN)
                rtc_iaq = 0;
                rtc_acc = 0;
                if (bme688_raw_forced_read(&rtc_temp, &rtc_hum, &rtc_pressure, &rtc_gas_res) == 0) {
                    ESP_LOGI(TAG, "BME688  ✅ Raw Forced | T: %.1f | H: %.1f | Gas: %.0f", rtc_temp, rtc_hum,
                             rtc_gas_res);
                } else {
                    ESP_LOGE(TAG, "BME688  ❌ Raw Forced error");
                }
            }
        }

        /* ── PASO 2: Cooldown I2C (200ms) ─────────────────────────── */
        vTaskDelay(pdMS_TO_TICKS(200));

        /* ── PASO 3: Trigger SCD41 (async, necesita 5s) ───────────── */
        if (retry_scd41_trigger() != ESP_OK) {
            ESP_LOGE(TAG, "SCD41 trigger failed");
        }

        /* ── PASO 4: Init BMV080 (cold start post-Deep-Sleep) ─────── */
        bool bmv080_active = false;
        if (is_pro_model) {
            if (bmv080_wrapper_init(bmv080_dev) == E_BMV080_OK) {
                ESP_LOGI(TAG, "BMV080  ✅ Láser encendido");
                bmv080_active = true;
            } else {
                ESP_LOGW(TAG, "BMV080  ❌ Init failed this cycle");
            }
        }

        /* ── PASO 5: Sub-bucle de Integración (Light-Sleep + Polling)
         *
         * Idéntico al sub-bucle de la Fase 1 pero sin BSEC.
         * Cada tick: despierta → drena FIFO BMV080 → duerme 950ms
         *
         * PRO model: 10 ticks = ~10s (BMV080 necesita ~10s para datos válidos)
         * BASE model: 5 ticks = ~5s  (solo SCD41 single_shot, sin BMV080)
         * ──────────────────────────────────────────────────────────── */
        int integration_ticks = (is_pro_model && bmv080_active) ? 10 : 5;
        ESP_LOGI(TAG, "⏳ Integration sub-loop: %d × 1s Light-Sleep", integration_ticks);

        for (int tick = 0; tick < integration_ticks; tick++) {
            if (bmv080_active) {
                bmv080_wrapper_read_data(NULL, NULL, NULL); // Drain FIFO, keep sensor alive
            }
            esp_sleep_enable_timer_wakeup(950000ULL); // 950ms
            esp_light_sleep_start();
        }

        /* ── PASO 6: Recolección de Datos ─────────────────────────── */
        scd41_data_t scd41_data = {0};
        if (retry_scd41_read(&scd41_data) == ESP_OK) {
            ESP_LOGI(TAG, "SCD41   -> CO2: %u ppm | Temp: %.2f C | Hum: %.2f %%", scd41_data.co2,
                     scd41_data.temperature, scd41_data.humidity);
        } else {
            ESP_LOGW(TAG, "SCD41   -> Read failed, using zeros");
            scd41_data.co2 = 0;
        }

        float pm1 = 0, pm25 = 0, pm10 = 0;
        if (bmv080_active) {
            // Una última lectura con datos de salida (el FIFO se drenó en el sub-bucle)
            int bmv_rslt = bmv080_wrapper_read_data(&pm1, &pm25, &pm10);
            if (bmv_rslt == 0 && (pm1 > 0 || pm25 > 0)) {
                ESP_LOGI(TAG, "BMV080  -> PM1: %.2f | PM2.5: %.2f | PM10: %.2f ug/m3", pm1, pm25, pm10);
            } else {
                ESP_LOGW(TAG, "BMV080  -> Sin datos válidos (%d) PM1=%.2f PM2.5=%.2f", bmv_rslt, pm1, pm25);
            }
            bmv080_wrapper_deinit();
        }

        /* ── PASO 7: Log + Transmisión ────────────────────────────── */
        ESP_LOGI(TAG, "BME688  -> IAQ: %.1f (Acc: %d) | Temp: %.2f C | Hum: %.2f %% | P: %.1f hPa | Gas: %.0f Ω",
                 rtc_iaq, rtc_acc, rtc_temp, rtc_hum, rtc_pressure, rtc_gas_res);
        ESP_LOGI(TAG, "Runtime: heap=%u bytes, stack=%u bytes", (unsigned int) esp_get_free_heap_size(),
                 (unsigned int) (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

        transmit_telemetry(&scd41_data, pm1, pm25, pm10);

        /* ── PASO 8: Deep-Sleep (o Light-Sleep para MODE_5_SEC) ───── */
        if (cfg->mode == PM_MODE_5_SEC) {
            /* MODE_5_SEC: Sin Deep-Sleep. Light-Sleep mínimo antes del próximo ciclo. */
            ESP_LOGI(TAG, "⏳ MODE_5_SEC: Light-Sleep (50ms) → next cycle");
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
        } else {
            /* MODE_1_MIN / MODE_5_MIN: Deep-Sleep real.
             * Cálculo DINÁMICO basado en bsec_sensor_control().next_call
             * obtenido DESPUÉS de bsec_do_steps() (refleja el timing ULP
             * real: ~300s, no el pre-medición de ~3s). */
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            int64_t now_ns       = ((int64_t) tv_now.tv_sec * 1000000000LL) + ((int64_t) tv_now.tv_usec * 1000LL);
            int64_t next_call_ns = bme688_bsec_get_next_call_ns();

            int64_t deep_sleep_us;
            if (next_call_ns > 0 && cfg->bme_mode == BME_MODE_BSEC_ULP) {
                /* Cálculo dinámico: next_call - now - 6s overhead (boot+I2C+SCD41) */
                int64_t time_to_sleep_ns = next_call_ns - now_ns;
                deep_sleep_us            = (time_to_sleep_ns / 1000) - 6000000LL;

                if (deep_sleep_us < 1000000LL) {
                    deep_sleep_us = 1000000LL; // Mínimo 1s para evitar crash
                }
                ESP_LOGI(TAG, "💤 Deep-Sleep (%lld s) [BSEC-synced, next_call in %lld s] boot_counter=%d",
                         deep_sleep_us / 1000000LL, time_to_sleep_ns / 1000000000LL, debug_boot_counter);
            } else {
                /* Fallback estático: MODE_1_MIN o BSEC no activo */
                deep_sleep_us = (cfg->mode == PM_MODE_1_MIN) ? 45000000LL : 285000000LL;
                ESP_LOGI(TAG, "💤 Deep-Sleep (%lld s) [Mode %d, static] boot_counter=%d", deep_sleep_us / 1000000LL,
                         cfg->mode, debug_boot_counter);
            }

            /* Hold I2C lines high during deep sleep to prevent sensor bus lockup */
            gpio_set_level(I2C_MASTER_SDA_IO, 1);
            gpio_set_level(I2C_MASTER_SCL_IO, 1);
            gpio_hold_en(I2C_MASTER_SDA_IO);
            gpio_hold_en(I2C_MASTER_SCL_IO);
            gpio_deep_sleep_hold_en();

            esp_sleep_enable_timer_wakeup((uint64_t) deep_sleep_us);
            esp_deep_sleep_start();
            // ← No regresa. El ESP32 reboota desde app_main().
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Orquestador Principal de Sensores (Pinned to Core 1)
 * ──────────────────────────────────────────────────────────────────────────── */
static void sensor_orchestration_task(void *pvParameters) {
    i2c_master_bus_handle_t bus_handle = (i2c_master_bus_handle_t) pvParameters;
    const node_config_t    *cfg        = power_manager_get_config();

    /* ── Auto-Discovery I2C (Solo en Cold Boot) ───────────────────────── */
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        discovery_done = false;
        /* Invalidar TODO el estado BSEC en RTC para evitar flags stale
         * de firmware anterior (ej: rtc_bsec_ulp_established=true de un
         * debug test previo que corrompería la lógica de bootstrap). */
        bme688_bsec_reset_rtc_state();
    }

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

    /* ── Inicializar BMV080 (solo durante warmup; en Fase 2 se enciende por ciclo) */
    if (is_pro_model && cfg->is_calibrating) {
        if (bmv080_wrapper_init(bmv080_dev) == E_BMV080_OK) {
            ESP_LOGI(TAG, "BMV080 Láser encendido (Warmup)");
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            ESP_LOGE(TAG, "BMV080 init failed; disabling particulate");
            is_pro_model = false;
        }
    }

    /* ── Inicializar BME688 + BSEC ────────────────────────────────────── */
    // Fase 1: Continuous Mode (1 Hz) para convergencia rápida
    // Fase 2 MODE_5_MIN: ULP (300s)
    // Fase 2 MODE_5_SEC / MODE_1_MIN: Raw Forced (sin BSEC)
    float init_sample_rate;
    if (cfg->is_calibrating) {
        init_sample_rate = BSEC_SAMPLE_RATE_CONT; // 1.0f → 1 Hz
    } else if (cfg->bme_mode == BME_MODE_BSEC_ULP) {
        init_sample_rate = BSEC_SAMPLE_RATE_ULP; // 0.0033f → 300s
    } else {
        init_sample_rate = BSEC_SAMPLE_RATE_LP; // Fallback: no se usará (Raw Forced)
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

    /* ═══════════════════════════════════════════════════════════════════
     * DESPACHO DE FASES
     * ═══════════════════════════════════════════════════════════════════ */
    if (cfg->is_calibrating) {
        run_warmup_phase(bme_initialized);
    }

    // Fase 2: Producción (bucle infinito con Deep Sleep)
    run_production_cycle(bme_initialized);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Punto de Entrada
 * ──────────────────────────────────────────────────────────────────────────── */
void app_main(void) {
    debug_boot_counter++;
    ESP_LOGW(TAG, "🔢 DEBUG boot_counter=%d | Wake cause: %d, reset reason: %d, free heap: %u bytes",
             debug_boot_counter, esp_sleep_get_wakeup_cause(), esp_reset_reason(),
             (unsigned int) esp_get_free_heap_size());

    gpio_hold_dis(I2C_MASTER_SDA_IO);
    gpio_hold_dis(I2C_MASTER_SCL_IO);
    gpio_deep_sleep_hold_dis();

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
