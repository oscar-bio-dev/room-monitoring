/*
 * power_manager.c — Gestión de Energía Smart Light-Sleep
 *
 * Fase 1 (WARMUP): 12 ciclos de ~5s usando sub-bucle de Light-Sleep 1s
 *         para alimentar BSEC a 1 Hz (Continuous Mode).
 * Fase 2 (PRODUCTION): Light-Sleep dinámico según modo seleccionado.
 *   - MODE_5_SEC: BSEC Continuous 1Hz + SCD41 Periodic (~5s ciclos)
 *   - MODE_5_MIN: BSEC ULP 300s + SCD41 Single-Shot (~300s ciclos)
 *
 * Deep Sleep legacy (v2.0) encapsulado bajo CONFIG_ENABLE_DEEP_SLEEP.
 *
 * Copyright (c) 2026 oscar-bio-dev. Apache-2.0.
 */
#include "power_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_ENABLE_DEEP_SLEEP
#include "driver/gpio.h"
#include "i2c_bus.h"
#endif

static const char *TAG = "power_manager";

/* ────────────────────────────────────────────────────────────────────────────
 * Estado del Nodo
 *
 * Light-Sleep: variables estáticas normales (RAM retenida entre ciclos).
 * Deep Sleep (legacy): RTC_DATA_ATTR para sobrevivir al reboot.
 * ──────────────────────────────────────────────────────────────────────────── */
#ifdef CONFIG_ENABLE_DEEP_SLEEP
RTC_DATA_ATTR static power_wake_state_t current_wake_state = PM_STATE_WAKE_A;
RTC_DATA_ATTR static node_config_t      s_config           = {
#else
static node_config_t s_config = {
#endif
    .mode                    = PM_MODE_5_MIN,
    .bme_mode                = BME_MODE_BSEC_CONTINUOUS,
    .warmup_cycles_remaining = PM_WARMUP_TOTAL_CYCLES,
    .is_calibrating          = true,
};

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/* ────────────────────────────────────────────────────────────────────────────
 * Tabla de tiempos de Deep-Sleep por modo (legacy, microsegundos)
 * ──────────────────────────────────────────────────────────────────────────── */
#define DEEP_SLEEP_5SEC_US 0ULL                  // No se usa Deep-Sleep
#define DEEP_SLEEP_5MIN_US (295ULL * 1000000ULL) // 295 segundos

static const uint64_t deep_sleep_table[] = {
    [PM_MODE_5_SEC] = DEEP_SLEEP_5SEC_US,
    [PM_MODE_5_MIN] = DEEP_SLEEP_5MIN_US,
};
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

#ifdef CONFIG_ENABLE_DEEP_SLEEP
power_wake_state_t power_manager_get_wake_state(void) {
    return current_wake_state;
}
#endif

const node_config_t *power_manager_get_config(void) {
    return &s_config;
}

void power_manager_set_mode(power_mode_t mode) {
    if (mode > PM_MODE_5_MIN) {
        ESP_LOGW(TAG, "Invalid power mode %d, defaulting to PM_MODE_5_MIN", mode);
        mode = PM_MODE_5_MIN;
    }
    s_config.mode = mode;

    // Determinar automáticamente el modo de muestreo del BME688
    if (mode == PM_MODE_5_MIN) {
        s_config.bme_mode = BME_MODE_BSEC_ULP;
    } else {
        // MODE_5_SEC: BSEC Continuous (1 Hz) para IAQ completo
        s_config.bme_mode = BME_MODE_BSEC_CONTINUOUS;
    }

    ESP_LOGI(TAG, "Power mode changed to %d (BME mode: %d)", mode, s_config.bme_mode);
}

void power_manager_tick_warmup(void) {
    if (!s_config.is_calibrating) {
        return;
    }
    if (s_config.warmup_cycles_remaining > 0) {
        s_config.warmup_cycles_remaining--;
    }
    if (s_config.warmup_cycles_remaining == 0) {
        s_config.is_calibrating = false;

        // Transicionar al modo BME688 apropiado para la Fase 2
        if (s_config.mode == PM_MODE_5_MIN) {
            s_config.bme_mode = BME_MODE_BSEC_ULP;
        } else {
            s_config.bme_mode = BME_MODE_BSEC_CONTINUOUS;
        }

        ESP_LOGI(TAG, "🔥 Warmup complete (%d cycles). Transitioning to Phase 2 (Power: %d, BME: %d).",
                 PM_WARMUP_TOTAL_CYCLES, s_config.mode, s_config.bme_mode);
    } else {
        ESP_LOGI(TAG, "🔥 Warmup pulse %d/%d", PM_WARMUP_TOTAL_CYCLES - s_config.warmup_cycles_remaining,
                 PM_WARMUP_TOTAL_CYCLES);
    }
}

bool power_manager_is_calibrating(void) {
    return s_config.is_calibrating;
}

#ifdef CONFIG_ENABLE_DEEP_SLEEP
/* ────────────────────────────────────────────────────────────────────────────
 * Ejecución del Ciclo de Sueño — Legacy Deep Sleep FSM (v2.0)
 * ──────────────────────────────────────────────────────────────────────────── */
void power_manager_execute_sleep_cycle(void) {
    if (current_wake_state == PM_STATE_WAKE_A) {
        current_wake_state = PM_STATE_WAKE_B;
        ESP_LOGI(TAG, "⏳ Entering Light-Sleep (4.95s) for SCD41 integration...");
        esp_sleep_enable_timer_wakeup(PM_MICROSLEEP_US);
        esp_light_sleep_start();

    } else {
        current_wake_state = PM_STATE_WAKE_A;

        if (s_config.is_calibrating) {
            ESP_LOGI(TAG, "⏳ Calibration: Light-Sleep (~50ms) before next pulse...");
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
            return;
        }

        uint64_t sleep_us = deep_sleep_table[s_config.mode];

        if (s_config.mode == PM_MODE_5_SEC) {
            ESP_LOGI(TAG, "⏳ MODE_5_SEC: Light-Sleep (~50ms) before next cycle...");
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
        } else {
            ESP_LOGI(TAG, "💤 Entering Deep-Sleep (%.0f s) [Mode %d]...", (double) sleep_us / 1000000.0, s_config.mode);

            gpio_set_level(I2C_MASTER_SDA_IO, 1);
            gpio_set_level(I2C_MASTER_SCL_IO, 1);
            gpio_hold_en(I2C_MASTER_SDA_IO);
            gpio_hold_en(I2C_MASTER_SCL_IO);
            gpio_deep_sleep_hold_en();

            esp_sleep_enable_timer_wakeup(sleep_us);
            esp_deep_sleep_start();
        }
    }
}
#endif
