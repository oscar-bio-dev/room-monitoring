# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
