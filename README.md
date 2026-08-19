# 🚀 Room-Monitoring: Arquitectura Aeroespacial (ESP32 + BME688)

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

## Arquitectura de Hardware y Software
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
- [x] **Aislamiento de Núcleos (FreeRTOS):** Asignar manejo de hardware (I2C, timers) al Core 1 (App Core) y confinar el stack de red y telemetría al Core 0 (Pro Core).
- [x] **I2C Asíncrono Non-Blocking:** Migrar el driver actual a transacciones I2C por hardware mediante interrupciones en ESP-IDF v5, liberando por completo los ciclos de CPU durante los 150ms de lectura.
- [x] **Mecanismo de Auto-Recuperación (Watchdog & I2C):** Rutina *Sanity Check* para detectar bloqueos en el bus (SDA/SCL latch-up) e inyectar 9 ciclos de reloj manuales por GPIO para reiniciar el estado del bus de forma autónoma.
- [x] **Persistencia RTC:** Guardar el vector de estado base de calibración del BME688 (Baseline) en la memoria RTC (Slow Memory) del ESP32 para sobrevivir a eventos de Deep Sleep y reinicios duros sin perder calibración.

### Fase 2: Filosofía Zero-CPU (Micro-Wakeups y Deep Sleep)
- [x] **Deep Sleep Agresivo:** Dormir el procesador Xtensa principal el 99% del tiempo para operar en regímenes de ultra-bajo consumo (~150µA promedio).
- [x] **Micro-Wakeups (RTC Timer):** Despertar al SoC intermitentemente usando el temporizador RTC, reteniendo la línea base de calibración de gas del BME688 en memoria estática `RTC_DATA_ATTR`.
- [x] **Inyección Fast-Path:** Interceptar el flujo de arranque para omitir las rutinas de inicialización I2C pesadas si los datos ya están cacheados en el RTC, logrando ráfagas de CPU activas extremadamente cortas (< 200ms).

### Fase 3: Inteligencia Embebida y Edge AI
- [ ] **Integración BSEC 2.0:** Incorporar el binario oficial de Bosch para el cálculo estandarizado de IAQ (Índice de Calidad de Aire), bVOC, y $CO_{2}\text{eq}$.
- [ ] **Gas Scanner (Perfil Térmico):** Escalar a ráfagas del calefactor MOX (200 °C - 400 °C en 10 pasos) utilizando los registros `res_heat_x` y `gas_wait_x`.
- [ ] **Inferencia TinyML (Bare-Metal):** Entrenar un Perceptrón Multicapa (MLP) cuantizado en INT8 nativo en C. Tras un trigger del ULP, la CPU principal clasifica firmas químicas específicas (ej. humo vs. humedad) antes de transmitir, ahorrando ancho de banda.

### Fase 4: Telemetría Edge-to-Cloud (GCP & Firebase)
- [ ] **Payloads Eficientes (Protobuf/CBOR):** Serializar las matrices de datos usando Protocol Buffers o CBOR para comprimir la transmisión al máximo.
- [ ] **Enlace RF (ESP-NOW a Gateway):** Enviar telemetría en micro-ráfagas vía ESP-NOW hacia un ESP32 Gateway interno (conectado a corriente), minimizando el tiempo de antena del nodo batería.
- [ ] **Ingesta Cloud:** El Gateway inyecta los datos a Google Cloud IoT / PubSub a través de MQTT con autenticación JWT segura.
- [ ] **Backend de Alto Rendimiento (Rust):** Microservicio en Rust (Tokio/Axum) suscrito a Pub/Sub, encargado de la validación y almacenamiento (ej. PostgreSQL/Firestore).
- [ ] **Frontend Distribuido (Firebase + Wasm):** Despliegue de la web alojada en Firebase. El backend envía telemetría en tiempo real al cliente vía Server-Sent Events (SSE) hacia la aplicación compilada en WebAssembly.
