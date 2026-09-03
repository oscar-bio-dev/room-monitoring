idf_component_register(
    SRCS "room-monitoring.c"
    INCLUDE_DIRS "."
    REQUIRES bme688_bsec bmv080_driver driver esp_hw_support i2c_bus power_manager scd41 rv1805_rtc network_manager storage_manager telemetry_proto nanopb
)
