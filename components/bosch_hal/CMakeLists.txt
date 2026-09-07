idf_component_register(
    SRCS "bosch_hal.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver" "esp_rom" "esp_driver_i2c"
)
