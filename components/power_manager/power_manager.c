/*
 * power_manager.c — Máquina de Estados Híbrida (Light-Sleep / Deep-Sleep)
 *
 * Fase 1 (INITIAL_WARMUP): 12 ciclos de ~5s usando sub-bucle de Light-Sleep 1s
 *         para alimentar BSEC a 1 Hz (Continuous Mode).
 * Fase 2 (ULP_CYCLE): Deep-Sleep dinámico según modo seleccionado por el usuario.
 *
 * Copyright (c) 2026 oscar-bio-dev. Apache-2.0.
 */
#include "power_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_manager";

/* ────────────────────────────────────────────────────────────────────────────
 * Estado Persistente en RTC SRAM (Sobrevive al Deep Sleep)
 * ──────────────────────────────────────────────────────────────────────────── */
RTC_DATA_ATTR static power_wake_state_t current_wake_state = PM_STATE_WAKE_A;
RTC_DATA_ATTR static node_config_t      rtc_config         = {
                 .mode                    = PM_MODE_5_MIN,
                 .bme_mode                = BME_MODE_BSEC_CONTINUOUS,
                 .warmup_cycles_remaining = PM_WARMUP_TOTAL_CYCLES,
                 .is_calibrating          = true,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Tabla de tiempos de Deep-Sleep por modo (microsegundos)
 *
 * El ciclo total del usuario es:
 *   Deep-Sleep + WAKE_A (~50ms) + Light-Sleep (4.95s) + WAKE_B (~200ms) ≈ TARGET
 *
 * MODE_5_SEC:  Sin Deep-Sleep (Light-Sleep puro de 4.95s por ciclo)
 * MODE_1_MIN:  Deep-Sleep 55s → total ≈ 60s
 * MODE_5_MIN:  Deep-Sleep 295s → total ≈ 300s (match perfecto BSEC ULP)
 * ──────────────────────────────────────────────────────────────────────────── */
#define DEEP_SLEEP_5SEC_US 0ULL                  // No se usa Deep-Sleep
#define DEEP_SLEEP_1MIN_US (55ULL * 1000000ULL)  // 55 segundos
#define DEEP_SLEEP_5MIN_US (295ULL * 1000000ULL) // 295 segundos

static const uint64_t deep_sleep_table[] = {
    [PM_MODE_5_SEC] = DEEP_SLEEP_5SEC_US,
    [PM_MODE_1_MIN] = DEEP_SLEEP_1MIN_US,
    [PM_MODE_5_MIN] = DEEP_SLEEP_5MIN_US,
};

/* ────────────────────────────────────────────────────────────────────────────
 * API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

power_wake_state_t power_manager_get_wake_state(void) {
    return current_wake_state;
}

const node_config_t *power_manager_get_config(void) {
    return &rtc_config;
}

void power_manager_set_mode(power_mode_t mode) {
    if (mode > PM_MODE_5_MIN) {
        ESP_LOGW(TAG, "Invalid power mode %d, defaulting to PM_MODE_5_MIN", mode);
        mode = PM_MODE_5_MIN;
    }
    rtc_config.mode = mode;

    // Determinar automáticamente el modo de muestreo del BME688
    if (mode == PM_MODE_5_MIN) {
        rtc_config.bme_mode = BME_MODE_BSEC_ULP;
    } else {
        rtc_config.bme_mode = BME_MODE_RAW_FORCED;
    }

    ESP_LOGI(TAG, "Power mode changed to %d (BME mode: %d)", mode, rtc_config.bme_mode);
}

void power_manager_tick_warmup(void) {
    if (!rtc_config.is_calibrating) {
        return;
    }
    if (rtc_config.warmup_cycles_remaining > 0) {
        rtc_config.warmup_cycles_remaining--;
    }
    if (rtc_config.warmup_cycles_remaining == 0) {
        rtc_config.is_calibrating = false;

        // Transicionar al modo BME688 apropiado para la Fase 2
        if (rtc_config.mode == PM_MODE_5_MIN) {
            rtc_config.bme_mode = BME_MODE_BSEC_ULP;
        } else {
            rtc_config.bme_mode = BME_MODE_RAW_FORCED;
        }

        ESP_LOGI(TAG, "🔥 Warmup complete (%d cycles). Transitioning to Phase 2 (Power: %d, BME: %d).",
                 PM_WARMUP_TOTAL_CYCLES, rtc_config.mode, rtc_config.bme_mode);
    } else {
        ESP_LOGI(TAG, "🔥 Warmup pulse %d/%d", PM_WARMUP_TOTAL_CYCLES - rtc_config.warmup_cycles_remaining,
                 PM_WARMUP_TOTAL_CYCLES);
    }
}

bool power_manager_is_calibrating(void) {
    return rtc_config.is_calibrating;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Ejecución del Ciclo de Sueño (Core de la FSM)
 * ──────────────────────────────────────────────────────────────────────────── */
void power_manager_execute_sleep_cycle(void) {
    if (current_wake_state == PM_STATE_WAKE_A) {
        /* ── WAKE_A → Light-Sleep (4.95s) ─────────────────────────────────
         * El SCD41 ya fue disparado. El BMV080 ya fue encendido.
         * Dormimos con CPU apagada pero RAM/I2C/FreeRTOS intactos.
         * Al despertar, los 5 segundos del SCD41 habrán pasado. */
        current_wake_state = PM_STATE_WAKE_B;
        ESP_LOGI(TAG, "⏳ Entering Light-Sleep (4.95s) for SCD41 integration...");
        esp_sleep_enable_timer_wakeup(PM_MICROSLEEP_US);
        esp_light_sleep_start();

    } else {
        /* ── WAKE_B → Deep-Sleep o Light-Sleep ──────────────────────────── */
        current_wake_state = PM_STATE_WAKE_A;

        /* ── Fase 1 (Calibración): Light-Sleep cortísimo ─────────────────
         * Durante los 12 pulsos usamos Light-Sleep para preservar el
         * estado de BSEC en RAM estática (evitamos Deep-Sleep). */
        if (rtc_config.is_calibrating) {
            ESP_LOGI(TAG, "⏳ Calibration: Light-Sleep (~50ms) before next pulse...");
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
            return;
        }

        /* ── Fase 2: Modo de energía del usuario ────────────────────────── */
        uint64_t sleep_us = deep_sleep_table[rtc_config.mode];

        if (rtc_config.mode == PM_MODE_5_SEC) {
            /* MODE_5_SEC: Light-Sleep puro para mantener contexto. */
            ESP_LOGI(TAG, "⏳ MODE_5_SEC: Light-Sleep (~50ms) before next cycle...");
            esp_sleep_enable_timer_wakeup(50000ULL);
            esp_light_sleep_start();
        } else {
            /* MODE_1_MIN / MODE_5_MIN: Deep-Sleep real */
            ESP_LOGI(TAG, "💤 Entering Deep-Sleep (%.0f s) [Mode %d]...", (double) sleep_us / 1000000.0,
                     rtc_config.mode);

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
