#include "power_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_manager";

// Estado de la máquina almacenado en SRAM profunda
RTC_DATA_ATTR static power_wake_state_t current_wake_state = PM_STATE_WAKE_A;

// 4.85 segundos para la sincronización entre el SCD41 (5.0s) y el BMV080
#define MICROSLEEP_TIME_US 4850000ULL

// 5 minutos de ciclo maestro
#define MASTER_SLEEP_TIME_US (5ULL * 60ULL * 1000000ULL)

power_wake_state_t power_manager_get_wake_state(void) {
    return current_wake_state;
}

void power_manager_execute_sleep_cycle(void) {
    uint64_t sleep_time;

    if (current_wake_state == PM_STATE_WAKE_A) {
        // Preparar para despertar en B
        current_wake_state = PM_STATE_WAKE_B;

        ESP_LOGI(TAG, "Entering Micro-Sleep (4.85s)...");
        esp_sleep_enable_timer_wakeup(MICROSLEEP_TIME_US);
        esp_light_sleep_start();
    } else {
        // Preparar para el siguiente ciclo maestro en A
        current_wake_state = PM_STATE_WAKE_A;
        sleep_time         = MASTER_SLEEP_TIME_US;

        ESP_LOGI(TAG, "Cycle complete. Holding I2C pins HIGH and entering Master Sleep (5 min)...");
        esp_sleep_enable_timer_wakeup(300000000ULL); // 5 minutos = 300,000,000 microsegundos
        esp_deep_sleep_start();
        gpio_set_level(I2C_MASTER_SDA_IO, 1);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        gpio_hold_en(I2C_MASTER_SDA_IO);
        gpio_hold_en(I2C_MASTER_SCL_IO);
        gpio_deep_sleep_hold_en();

        esp_sleep_enable_timer_wakeup(sleep_time);
        esp_deep_sleep_start();
    }
}
