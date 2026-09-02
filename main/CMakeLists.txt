idf_component_register(
    SRCS "room-monitoring.c"
    INCLUDE_DIRS "."
    REQUIRES bme688_bsec bmv080_driver driver esp_hw_support i2c_bus power_manager scd41
)
