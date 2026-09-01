#include "power_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "i2c_bus.h"

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

static void hold_hardware_pins(void) {
    // Aislar el dominio RTC para prevenir corrientes parásitas
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Mantener la línea I2C alimentada para que el SCD41/BMV080 no colapsen en su ciclo
    gpio_hold_en(I2C_MASTER_SDA_IO);
    gpio_hold_en(I2C_MASTER_SCL_IO);
    gpio_deep_sleep_hold_en();
}

void power_manager_execute_sleep_cycle(void) {
    uint64_t sleep_time;

    if (current_wake_state == PM_STATE_WAKE_A) {
        // Preparar para despertar en B
        current_wake_state = PM_STATE_WAKE_B;
        sleep_time         = MICROSLEEP_TIME_US;

        ESP_LOGI(TAG, "Entering Micro-Sleep (4.85s). Holding pins...");
        hold_hardware_pins();
    } else {
        // Preparar para el siguiente ciclo maestro en A
        current_wake_state = PM_STATE_WAKE_A;
        sleep_time         = MASTER_SLEEP_TIME_US;

        ESP_LOGI(TAG, "Cycle complete. Releasing pins and entering Master Sleep (5 min)...");
        gpio_hold_dis(I2C_MASTER_SDA_IO);
        gpio_hold_dis(I2C_MASTER_SCL_IO);
        gpio_deep_sleep_hold_dis();
    }

    esp_sleep_enable_timer_wakeup(sleep_time);
    esp_deep_sleep_start();
}
