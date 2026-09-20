# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.9.0] - 2026-09-19
### Added
- **Transporte Bidireccional Asíncrono:** Implementado un patrón de enrutamiento mediante un *Byte de Cabecera* (`0x10` Telemetría, `0x11` Diagnóstico, `0x20` GatewayAck).
- **Buzón (Mailbox) FreeRTOS:** Tras enviar telemetría, el nodo entra en una escucha bloqueante de bajo consumo durante 200ms (`network_manager_receive_cmd`). Esto permite interceptar respuestas y comandos del Gateway.
- **Hardware Self-Test Activo:** Creado el sistema de diagnóstico a nivel de silicio (secuenciador de ~11s). Se invoca remotamente vía `CMD_RUN_SELF_TEST`.
  - **SCD41:** Se activa el test `0x3639` con validación estricta de CRC8 y decodificación de registros de error.
  - **BMV080:** Se fuerza un *Cold Boot* (Hardware Reset + Firmware Reload + Sensor ID check) evadiendo bloqueos catastróficos del bus.
  - **BME688:** Validación directa vía I2C del `Chip ID` (registro `0xD0`).
- **Protobuf v2 (Nanopb):** `telemetry.proto` extendido. Nuevos mensajes `GatewayAck`, `Command`, `DiagnosticReport` y enums `NodeStatus`. Se inyecta pasivamente el campo `system_error_bitmask` en la telemetría habitual.

## [0.8.0] - 2026-09-19
### Added
- **BMV080 Number Concentration (BST-BMV080-DS000-10, §5.2.1.3.1):** El wrapper del sensor láser ahora extrae los 6 campos completos de `bmv080_output_t`: 3 de concentración de masa (µg/m³) y 3 de concentración numérica (particles/m³). Esto duplica la información de partículas disponible para el backend, habilitando cálculos de AQI por distribución de tamaño (EPA USA) y clasificación de salas limpias (ISO 14644).
- **Obstruction Detection habilitada:** `bmv080_set_parameter("do_obstruction_detection", true)`. Los flags `is_obstructed` e `is_outside_measurement_range` se propagan al payload Protobuf y al log para monitoreo de salud del sensor láser en campo.
- **Estructura `bmv080_reading_t`:** Nueva estructura de datos que encapsula 9 campos: 3 masa, 3 conteo, 2 flags de hardware y runtime del ciclo de medición.
- **API de lifecycle BMV080 separada:** `bmv080_wrapper_start()` / `bmv080_wrapper_stop()` independientes de `init()` / `deinit()`. Permite reutilizar el handle del sensor entre ciclos de Light-Sleep sin re-descargar firmware al ASIC (~1.3s de ahorro por ciclo).
- **Telemetría expandida (98 bytes):** 6 nuevos campos en `telemetry.proto` (tags 26-31): `pm1_0_count`, `pm2_5_count`, `pm10_0_count` (float), `is_laser_obstructed`, `is_pm_out_of_range` (bool), `laser_runtime` (float).

### Changed
- **Sub-bucle de integración BMV080:** Extendido de 10 a 12 ticks (11.4s), cumpliendo el requisito teórico del datasheet de `integration_time (10s) + 1.17s = 11.17s` para la primera lectura estable.
- **Ciclo de vida del láser en MODE_5_MIN:** Transición de `init()/deinit()` por ciclo a `start()/stop()`. El handle sobrevive Light-Sleep (RAM retenida), eliminando el overhead de `bmv080_open() + bmv080_reset()` (~1.3s) en cada despertar.
- **Transición Warmup→Producción:** Cambió de `bmv080_wrapper_deinit()` a `bmv080_wrapper_stop()` para preservar el handle del sensor durante la transición a MODE_5_MIN.
- **Logs BMV080 expandidos:** Formato unificado en todos los modos mostrando masa + conteo + flags: `PM1: 44.00 | PM2.5: 96.00 | PM10: 132.00 ug/m3 | #1: 774 | #2.5: 794 | #10: 795 /m3`.
- **Payload Protobuf:** Creció de 68 a **98 bytes** (39% del límite ESP-NOW de 250 bytes). Margen restante: 152 bytes.

## [0.7.0] - 2026-09-19
### Added
- **Smart Light-Sleep Production Loop (ADR-001 Implementado):** Bucle de producción continuo basado en `esp_light_sleep_start()` con timer dinámico sincronizado con BSEC `next_call`. Reemplaza completamente al Deep Sleep como modo de producción. RAM, RTOS, heap y estado BSEC se retienen entre ciclos sin serialización.
- **Event Loop Dinámico (2 Modos):** El bucle principal actúa como despachador dinámico que calcula el sleep time basándose en `MIN(next_bsec_call, next_scd41_read)`:
  - `MODE_5_SEC` (Continuo): BSEC Continuous 1Hz + SCD41 Periodic (~5s). Ticks de Light-Sleep ~1s. TX cada 5 ciclos. IAQ completo en tiempo real.
  - `MODE_5_MIN` (Batería): BSEC ULP 300s + SCD41 Single-Shot. Light-Sleep dinámico ~295s (BSEC-synced). TX por ciclo.
- **SCD41 Periodic Measurement API:** Nuevas funciones `scd41_start_periodic_measurement()` (0x21B1), `scd41_stop_periodic_measurement()` (0x3F86) y `scd41_get_data_ready()` (0xE4B8) para MODE_5_SEC.
- **Network Manager 3-API Pattern:** `init()` (una vez en boot), `wake()` (esp_wifi_start, ~5ms pre-TX), `sleep()` (esp_wifi_stop pre-Light-Sleep). Ahorra ~40 KB de heap por ciclo vs el patrón init/deinit anterior.
- **BSEC IAQ en todos los modos:** Eliminado `BME_MODE_RAW_FORCED`. Todos los modos de producción usan BSEC (Continuous o ULP) para datos de IAQ completo.
- **Sensor Fusion (SCD41 + BME688):** Implementada inyección en tiempo real de presión barométrica (`BSEC_OUTPUT_RAW_PRESSURE`) del BME688 hacia el SCD41 (comando `0xE000`) para compensación termodinámica del cálculo de CO2 fotoacústico.
- **Protobuf Payload Expandido (68 bytes):** Añadidos los campos `eco2`, `bvoc`, y `tvoc` a `telemetry.proto`. El nodo ahora envía una trama ultra-optimizada de 68 bytes con datos fusionados de los 3 sensores.

### Fixed
- **BSEC_E_CONFIG_FEATUREMISMATCH (-35):** Resuelto el error de incompatibilidad con la variante IAQ estándar de BSEC al reducir las suscripciones a 7 outputs (IAQ, Temp, Hum, Presión, Gas, y eCO2). Los sensores virtuales `bvoc` y `tvoc` quedan reservados en el esquema Protobuf para futura migración a la variante `Sel_IAQ`.

### Changed
- **Simplificación a 2 modos de energía:** Eliminado `PM_MODE_1_MIN`. El sistema opera solo con `MODE_5_SEC` (continuo) y `MODE_5_MIN` (batería). A futuro, los modos son seleccionables vía ESP-NOW/Bluetooth o detección del pin de carga (CHG).
- **Variables RTC → estáticas:** `RTC_DATA_ATTR` eliminado de todas las variables de producción. En Light-Sleep la RAM se retiene automáticamente. Las variables usan `static` estándar.
- **BSEC next_call nativo:** El timer de Light-Sleep se calcula desde `bsec_sensor_control().next_call` nativo (confiable con RAM retenida), en lugar del cálculo derivado `1/sample_rate` que era necesario en Deep Sleep.

### Deprecated
- **Deep Sleep como modo de producción:** Todo el código de Deep Sleep (RTC persistence, `gpio_hold_en`, `boot_counter`, FSM WAKE_A/WAKE_B, `bsec_get_state`/`bsec_set_state`, `mark_state_stale`, `reset_rtc_state`, `network_manager_deinit`) queda encapsulado bajo `#ifdef CONFIG_ENABLE_DEEP_SLEEP` para futura migración a ESP32-S3/C6 o BSEC 4.x.
- **`BME_MODE_RAW_FORCED`:** Eliminado del enum `bme_sampling_mode_t`. Todos los modos usan BSEC para IAQ.
- **`PM_MODE_1_MIN`:** Eliminado del enum `power_mode_t`.

## [0.6.0] - 2026-09-03
### Added
- **Protobuf (Nanopb):** Serialización binaria estricta (`telemetry.proto`) para todos los sensores ambientales, diagnósticos de ciclos y timestamp sin fragmentación del heap.
- **Red ESP-NOW Asíncrona:** Integración de la radio Wi-Fi en modo STA con confirmaciones explícitas de paquetes (ACK) contra la MAC del Gateway.
- **Caja Negra (Store-and-Forward):** Mecanismo de persistencia offline usando la MicroSD (bus VSPI). Solo se monta FATFS *on-demand* ante fallos de ACK, aislando eléctricamente los pines de la tarjeta al finalizar para prevenir dreno de batería.
- **Recuperación Anti Brown-out:** Sistema dinámico de purgado del historial (máximo 15 registros por despertar) al detectar que el Gateway está online, para evitar caídas de tensión eléctrica en los momentos críticos de transmisión RF continua.

## [0.5.0] - 2026-09-03
### Added
- **RTC Híbrido Zero-CPU:** Sincronización transparente entre el reloj de hardware Qwiic RV-1805 (Cold Boot) y el reloj interno `gettimeofday()` a través de los ciclos de Deep Sleep.
- Validación CRC-8 de todas las palabras de medición SCD41 y reintentos limitados para sus operaciones I2C.
- Soporte completo para métricas PM1.0 y PM10.0 extraídas desde la librería nativa del sensor BMV080, además del PM2.5.
- Métricas de diagnóstico de despertar, reinicio, heap y margen mínimo de stack.

### Changed
- El micro-sleep de 4,85 segundos usa Light Sleep para conservar el contexto de sensores.
- Las operaciones de BME688 y BMV080 propagan fallos, validan argumentos y limitan transferencias del SDK BMV080.
- Se retiró el controlador BMV080 simulado no integrado; el firmware usa exclusivamente el wrapper del SDK oficial.

### Fixed
- **Colisión de Bus I2C (Software Timeout):** Se resolvieron las caídas del bus compartidas entre BME688, SCD41 y BMV080 agregando un tiempo mecánico de estabilización (250ms tras arranque del láser y 50ms post-disparo de CO2) evitando fallos de Clock-Stretching.
- **Desbordamiento FIFO (BMV080):** Corrección drástica del error frecuente de `Sensor obstruido o sucio`. Se incrementó el sondeo de 1000ms a 100ms durante el calentamiento de 15s y se inyectó una rutina de purgado de 15 lecturas tras despertar del Micro-Sleep.
- **Amnesia de Tiempo (BSEC 3.0):** Se integró exitosamente `CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y` junto con la inicialización RTC RV-1805, proveyendo un uptime irrompible para los algoritmos del BME688.

## [0.4.0] - 2026-09-01
### Added
- Integración completa de sensores **SCD41** (CO2) y **BMV080** (PM2.5).
- Soporte para **BSEC 3.0** con preservación de estado ULP en memoria RTC.
- Aislamiento eléctrico completo en Deep Sleep reteniendo `SDA` y `SCL` en `HIGH` usando `gpio_hold_en` para prevenir corrupciones I2C (Clock Glitching).
- Emisión de **I2C General Call Reset** (0x00 -> 0x06) en la inicialización del bus para resetear por hardware a esclavos Bosch.

### Fixed
- **Amnesia de Auto-Escáner I2C:** Se declararon las variables de dirección I2C dinámica (`dynamic_bmv_addr`, `dynamic_bme_addr`) bajo `RTC_DATA_ATTR`. Esto evita que el ESP32 restablezca las direcciones a sus valores por defecto al despertar del Deep Sleep, solucionando rechazos fantasma (`NACK`).
- **SDK Bosch Pointer Crash:** Implementación de sanitización manual (`bmv080_handle = NULL`) antes de reintentos I2C, eludiendo el error interno `180 (E_BMV080_ERROR_NULLPTR)` del driver oficial de Bosch.

## [0.3.0] - 2026-09-01
### Added
- Componente `power_manager`: Máquina de estados de Doble Despertar (4.85s) con aislamiento físico `gpio_hold_en()`.
- Componente `i2c_bus`: Abstracción del hardware con inyección de 9 pulsos para Auto-Recuperación (Bit-Banging).
- Configuración estricta de `.clang-format` basada en el estilo de Espressif (C/C++).

### Changed
- **Refactor Arquitectónico:** Se demolió el monolito `room-monitoring.c` a favor de una arquitectura limpia por componentes (Desacoplamiento).
- Orquestador adaptado para el futuro Auto-Discovery de topología de hardware (Modelo Base vs Pro).

## [0.2.0] - 2026-08-19
### Added
- Deep Sleep Agresivo (Ultra-Low Power) y Micro-Wakeups (RTC Timer).
- Inyección Fast-Path de datos de calibración de gas retenidos en memoria SRAM (`RTC_DATA_ATTR`).

### Fixed
- **Ciclo Térmico (Wake & Heat):** Incremento de la ventana de espera del calentador MOX de 150ms a 200ms para garantizar estabilización térmica (`heat_stab == 1`) antes de la lectura ADC.

## [0.1.0] - 2026-08-19
### Added
- Inicialización del repositorio.
- Estructura de carpetas basada en componentes de ESP-IDF.
- Archivos base: `.gitignore`, `LICENSE`, `CHANGELOG.md`, y `README.md`.
- Implementación de la Fase 1: Driver básico I2C asíncrono para BME688, Core Pinning (Core 1) y mecanismo de recuperación Sanity Check.
