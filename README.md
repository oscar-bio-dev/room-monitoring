# 🧪 Room-Monitoring BME688 (ESP-IDF)

![ESP-IDF v5.3.5](https://img.shields.io/badge/ESP--IDF-v5.3.5-red.svg)
![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)
![Build](https://img.shields.io/badge/Build-Passing-brightgreen.svg)

**Arquitectura Aeroespacial (ESP32 + BME688)**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3-red.svg)](https://docs.espressif.com/)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

Este proyecto exprime al máximo las capacidades de silicio del ESP32 (Zero-CPU) y el sensor ambiental Bosch BME688, conectando telemetría de ultra-bajo consumo a una infraestructura en Google Cloud Platform (GCP) con backend en Rust y frontend distribuido vía Firebase (`https://oscar-bio.dev`).

## 📑 Tabla de Contenidos
- [Descripción General](#descripción-general)
- [Arquitectura de Hardware y Software](#arquitectura-de-hardware-y-software)
- [Requisitos](#requisitos)
- [Instalación y Uso](#instalación-y-uso)
- [Roadmap de Desarrollo](#🗺️-roadmap-de-desarrollo)

## Descripción General
El sistema implementa patrones de grado aeroespacial para el monitoreo de ambientes:
- **Zero-CPU:** Uso extensivo del Coprocesador de Ultra-Bajo Consumo (ULP FSM) y tareas aisladas por hardware.
- **Tolerancia a Fallos:** Rutinas de auto-recuperación (Sanity Checks) para evitar *latch-ups* en los buses físicos I2C.
- **Eficiencia Energética:** Uso del registro de memoria RTC (Slow Memory) para cachear datos de calibración y evitar el arranque en frío constante durante los ciclos de Deep Sleep.

## 🛠️ Requisitos de Hardware y Software
- **Hardware:** SparkFun IoT RedBoard ESP32 (o cualquier SoC ESP32-WROOM).
- **Sensor:** Bosch BME688 conectado al bus I2C (SDA: 21, SCL: 22).
- **Framework:** [ESP-IDF v5.3.5](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html).

## 🚀 Compilación y Flasheo (ESP-IDF)
El proyecto hace uso de la herramienta de compilación nativa de Espressif (`idf.py`). Para compilar y monitorizar la placa:

```bash
idf.py set-target esp32
idf.py build
idf.py -p (PUERTO_SERIAL) flash monitor
```

---

## 🎯 Arquitectura del Proyecto
- **Core 0 (Pro Core):** Reservado exclusivamente para Telemetría Edge-to-Cloud y el stack WiFi/ESP-NOW.
- **Core 1 (App Core):** Tareas nativas ancladas por FreeRTOS dedicadas al control del I2C, rutinas de inicialización y recuperación.
- **Payloads Eficientes:** Uso de Protocol Buffers para condensar mediciones y evitar saturación del ancho de banda y tiempo de antena.

## Requisitos
- **Hardware:** SparkFun IoT RedBoard ESP32 (ESP32 WROOM) o cualquier SoC ESP32 compatible. Sensor Bosch BME688.
- **Software:** ESP-IDF v5.3+ configurado.

## Instalación y Uso

1. **Configurar el entorno:**
```bash
. $HOME/esp/esp-idf/export.sh
```

2. **Compilar, flashear y monitorear:**
```bash
idf.py build flash monitor
```

---

## 🗺️ Roadmap de Desarrollo

### Fase 1: Fundamentos Críticos (Capa Física y RTOS)
- [x] **Arquitectura Base:** Scaffolding de un proyecto profesional nativo en ESP-IDF (CMake, componentes, tests).
- [x] **Driver HAL BME688:** Migración *Bare-Metal* del código del sensor a la estructura de componentes.
- [x] **I2C Asíncrono Non-Blocking:** Implementar transacciones I2C por hardware optimizadas.
- [ ] **I2C Multiplexado:** Expandir el bus I2C a 400kHz para soportar los sensores de referencia: **SCD41** (`0x62`, NDIR CO2) y **BMV080** (`0x54`, PM Scanner óptico).

### Fase 2: Sensor Fusion y Energía (Máquina de Doble Despertar)
- [x] **Micro-Wakeups y Fast-Path:** Retención de datos en memoria estática `RTC_DATA_ATTR` para evitar la inicialización térmica completa del BME688 en cada despertar.
- [ ] **Compensación Barométrica Cruzada:** Inyectar la lectura de presión atmosférica del BME688 en el registro NDIR del SCD41 previo al disparo para máxima precisión.
- [ ] **Máquina de Doble Despertar (10s):** Orquestar el Deep Sleep en dos etapas (Despertar A: Iniciar óptica y láser -> Sleep 10s -> Despertar B: Cosechar mediciones de alta precisión).
- [ ] **Retención de Estado Físico:** Implementar aislamiento `gpio_hold_en()` sobre los pines del bus para mantener la integridad de energía de la instrumentación óptica durante los 10s de sueño profundo.
- [ ] **Deep Sleep Dinámico y Motor Predictivo:** Calcular Tasa de Cambio ($\Delta/\Delta t$) para gobernar el tiempo de sueño maestro (ej. Baseline 5m, Warning 30s, Emergency 5s).

### Fase 3: Inteligencia Embebida y Edge AI
- [ ] **Integración BSEC 2.0:** Incorporar el binario oficial de Bosch para el cálculo estandarizado de IAQ (Índice de Calidad de Aire), bVOC, y $CO_{2}\text{eq}$.
- [ ] **Gas Scanner (Perfil Térmico):** Escalar a ráfagas del calefactor MOX (200 °C - 400 °C en 10 pasos) utilizando los registros `res_heat_x` y `gas_wait_x`.
- [ ] **Inferencia TinyML (Bare-Metal):** Entrenar un Perceptrón Multicapa (MLP) cuantizado en INT8 nativo en C. Tras un trigger del ULP, la CPU principal clasifica firmas químicas específicas (ej. humo vs. humedad) antes de transmitir, ahorrando ancho de banda.

### Fase 4: Telemetría Edge-to-Cloud (GCP & Firebase)
- [ ] **Contrato de Datos Estricto (Protobuf):** Serializar la telemetría del BME688 usando Protocol Buffers en el nodo (ESP32) para compresión extrema.
- [ ] **Transmisión de Ultra Baja Latencia (ESP-NOW):** El nodo transmite el Protobuf en ráfagas de milisegundos y vuelve a Deep Sleep.
- [ ] **Gateway Unificado (ESP32 + Ethernet):** Nodo central (WT32-ETH01 / Nano ESP32 + W5500) operando con segregación de núcleos:
    - **Core 1:** Escucha pasiva de radio 2.4GHz capturando paquetes ESP-NOW.
    - **Core 0:** Aceleración criptográfica por hardware (TLS 1.2/1.3 + JWT) y publicación a Google Cloud Pub/Sub vía Ethernet nativo (lwIP).
- [ ] **Backend de Alto Rendimiento (Rust):** Microservicio en Rust (Tokio/Axum) suscrito a Pub/Sub, encargado de la validación y almacenamiento.
- [ ] **Stream de Datos al Frontend (SSE):** Endpoint Server-Sent Events (SSE) para transmitir datos unidireccionales al cliente WebAssembly (Firebase) manteniendo la pureza de Rust.
