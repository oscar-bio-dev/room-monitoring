idf_component_register(SRCS "src/storage_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "fatfs" "vfs" "telemetry_proto" "nanopb")
