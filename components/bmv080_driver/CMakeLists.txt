idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES "bosch_hal"
)

# El SDK de Bosch para el BMV080 es un binario cerrado. Enlazamos las librerías precompiladas.
target_link_libraries(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/lib_bmv080.a")
target_link_libraries(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/lib_postProcessor.a")
