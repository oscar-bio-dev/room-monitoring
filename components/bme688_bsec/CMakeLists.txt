idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES "bosch_hal" "esp_timer"
)

# Enlazar la librería estática precompilada del algoritmo BSEC 2.0
# El usuario debe colocar libalgobsec.a compilado para ESP32 (Xtensa lx6) en la carpeta lib/
target_link_libraries(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/libalgobsec.a")
