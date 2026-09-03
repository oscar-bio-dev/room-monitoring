idf_component_register(SRCS "src/network_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_wifi" "esp_netif" "telemetry_proto" "nanopb")
