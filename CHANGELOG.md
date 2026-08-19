# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
