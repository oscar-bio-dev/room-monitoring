idf_component_register(SRCS "power_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_hw_support driver esp_timer i2c_bus)
