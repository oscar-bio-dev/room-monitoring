#include "i2c_bus.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2c_bus";

// Rutina de Auto-Recuperación (Sanity Check)
static void sanity_check_i2c(void) {
    ESP_LOGI(TAG, "Performing I2C bus sanity check...");
    gpio_config_t io_conf = {.intr_type    = GPIO_INTR_DISABLE,
                             .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
                             .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
                             .pull_down_en = 0,
                             .pull_up_en   = 1};
    gpio_config(&io_conf);

    // [CRÍTICO] El registro de salida por defecto es 0. En modo Open-Drain, esto tira la línea a GND.
    // Debemos escribir 1 explícitamente para dejar que la línea flote ALTA impulsada por las pull-ups.
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    gpio_set_level(I2C_MASTER_SCL_IO, 1);

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Injecting 9 clock pulses and STOP condition to reset slave state machines...");
    for (int i = 0; i < 9; i++) {
        gpio_set_level(I2C_MASTER_SCL_IO, 0);
        ets_delay_us(5);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        ets_delay_us(5);
    }

    // Condición STOP
    gpio_set_level(I2C_MASTER_SDA_IO, 0);
    ets_delay_us(5);
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    ets_delay_us(5);
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    ets_delay_us(10);

    if (gpio_get_level(I2C_MASTER_SDA_IO) == 0) {
        ESP_LOGE(TAG, "Failed to recover I2C bus! SDA is still LOW.");
    } else {
        ESP_LOGI(TAG, "I2C bus sanity check passed. Lines are high.");
    }

    gpio_reset_pin(I2C_MASTER_SDA_IO);
    gpio_reset_pin(I2C_MASTER_SCL_IO);
}

esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_handle) {
    if (!bus_handle)
        return ESP_ERR_INVALID_ARG;

    // Ejecutar bit-banging preventivo antes de inyectar el periférico por hardware
    sanity_check_i2c();

    ESP_LOGI(TAG, "Initializing I2C Master Bus...");
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = -1, // Any free port
        .scl_io_num                   = I2C_MASTER_SCL_IO,
        .sda_io_num                   = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&i2c_mst_config, bus_handle);
}
