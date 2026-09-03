# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
