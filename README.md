# 🧪 Room-Monitoring: Estación Ambiental de Precisión (ESP-IDF)

[![ESP-IDF CI](https://github.com/oscar-bio-dev/room-monitoring/actions/workflows/esp-idf-ci.yml/badge.svg)](https://github.com/oscar-bio-dev/room-monitoring/actions/workflows/esp-idf-ci.yml)
![ESP-IDF v5.3](https://img.shields.io/badge/ESP--IDF-v5.3-red.svg)
![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)

Este proyecto implementa un nodo de telemetría ambiental (Calidad del Aire) de grado industrial, exprimiendo al máximo las capacidades de ultra-bajo consumo del silicio **ESP32** integrando instrumentación de alta gama de **Bosch Sensortec (BME688, BMV080)** y **Sensirion (SCD41)**.

---

## 🏗️ Arquitectura de Software y Hardware

El firmware ha sido diseñado bajo los estándares empresariales más estrictos (basado en el RFC-2119 documentado internamente en `AGENTS.md`), priorizando la tolerancia a fallos, la retención física de hardware y una arquitectura limpia basada en componentes.

### Hardware del Nodo

| Componente | Sensor | Interfaz | Medición |
|-----------|--------|----------|----------|
| **Bosch BME688** | MOX + BSEC 3.0 | I2C (0x76) | IAQ, Temperatura, Humedad, Presión, VOCs |
| **Sensirion SCD41** | NDIR fotoacústico | I2C (0x62) | CO₂ (400–5000 ppm), T, H |
| **Bosch BMV080** | Láser óptico | I2C (0x57) | PM1.0, PM2.5, PM10 (µg/m³) |
| **RV-1805-C3** | RTC hardware ±2 ppm | I2C Qwiic | Timestamp de precisión |
| **MicroSD** (onboard) | FATFS on-demand | SPI (VSPI) | Store-and-Forward (Caja Negra) |
| **ESP32-D0WD-V3** | Xtensa dual-core, rev 3.1 | — | MCU + radio ESP-NOW |

### Topología Segregada (Core 0 / Core 1)
- **Core 0 (Pro Core):** Tareas asíncronas pesadas (stack de red, ESP-NOW, telemetría).
- **Core 1 (App Core):** Tareas críticas ancladas vía FreeRTOS dedicadas a los drivers I2C y temporización de sensores láser.

### Máquina de Estados (Smart Light-Sleep v1.x)

El nodo opera en un bucle de producción continuo basado en Light-Sleep, que retiene la totalidad de la RAM (RTOS + heap + estado BSEC) entre ciclos:

```mermaid
stateDiagram-v2
    direction TB
    [*] --> Cold_Boot

    state Cold_Boot {
        Init_I2C: I2C Bus Init + 9-pulse Recovery
        Sync_RTC: RV-1805 Time Sync
        Init_BSEC: BSEC Init (1 Hz Continuous)
        Init_Sensors: SCD41 + BMV080 Init
        Init_I2C --> Sync_RTC
        Sync_RTC --> Init_BSEC
        Init_BSEC --> Init_Sensors
    }

    Cold_Boot --> Warmup

    state Warmup {
        note right of Warmup
            12 pulsos a 1 Hz (~60s)
            is_calibrating = true
            IAQ Accuracy: 0 → 1
        end note
    }

    Warmup --> BSEC_Transition: Pulso 12/12

    state BSEC_Transition {
        note right of BSEC_Transition
            Continuous (1 Hz) → ULP/LP
            mark_state_stale()
        end note
    }

    BSEC_Transition --> Production

    state Production {
        Measure: BSEC + SCD41 + BMV080
        Transmit: ESP-NOW TX (Protobuf)
        Fallback: SD Store-and-Forward
        Measure --> Transmit
        Transmit --> Fallback: ACK fail?
    }

    state "Smart Light-Sleep" as Sleep {
        note right of Sleep
            Mode 0: ~5s (LP BSEC)
            Mode 1: ~55s (LP BSEC)
            Mode 2: ~293s (ULP BSEC)
            RAM + RTOS + BSEC retenidos
        end note
    }

    Production --> Sleep
    Sleep --> Production: Timer Wakeup
```

> **Nota histórica:** La arquitectura original usaba Deep Sleep como ciclo maestro (WAKE_A → Light-Sleep 4.85s → WAKE_B → Deep Sleep). Este diseño fue abandonado tras la auditoría de BSEC que demostró incompatibilidad temporal del filtro de Kalman con el boot del ESP32. Ver [ADR-001](docs/ADR-001-Power-Management-BSEC.md).

---

## 🛠️ Resiliencia y Manejo de Errores

Este repositorio implementa tácticas críticas para hardware desplegado en campo:

1. **Bit-Banging Latch-Up Recovery:** Previo a montar el periférico hardware de I2C, el sistema inyecta 9 pulsos de reloj (SCL) manuales. Esto destraba esclavos que se hayan quedado "colgados" tirando de la línea SDA a tierra tras una caída abrupta de voltaje.
2. **Retención de Estado en RAM (Light-Sleep):** En el modo de producción, Smart Light-Sleep retiene la totalidad de la RAM entre ciclos. Las variables de BSEC 3.0, las direcciones I2C dinámicas y el contexto FreeRTOS sobreviven sin necesidad de serialización.
3. **I2C General Call Reset:** Uso del comando broadcast de reset `0x00 -> 0x06` en el bus maestro para forzar un Soft Reset a nivel de silicio en los sensores Bosch, garantizando inicializaciones inmaculadas post-Cold-Boot.
4. **Integridad de mediciones:** Las tres palabras de la trama SCD41 se validan mediante CRC-8 antes de convertirlas a CO₂, temperatura y humedad. Las operaciones SCD41 se reintentan hasta tres veces y los fallos de inicialización de cada sensor deshabilitan únicamente esa medición.
5. **Sincronización de Tiempo Real (RTC Híbrido RV-1805):** En Cold Boot, el sistema sincroniza `gettimeofday()` contra el chip de hardware RV-1805 (±2 ppm). El resto del ciclo confía en el reloj interno anclado al temporizador RTC profundo (`CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y`), logrando control de tiempo milimétrico sin penalizar el bus I2C ni consumir batería.
6. **Anticolisión I2C (Clock-Stretching):** Implementación de retardos tácticos mecánicos estables entre la excitación del escáner láser BMV080 (250ms), el disparo del sensor NDIR SCD41 (50ms) y la ráfaga de datos del BME688. Además, el láser BMV080 se sondea mediante *fast-polling* (100ms) durante el calentamiento y rutinas de purgado (15-buffer flush) para evitar fallos catastróficos por desbordamiento de su FIFO interno y bloqueos de bus (`I2C software timeout`).
7. **Telemetría ESP-NOW y Caja Negra (Store-and-Forward):** La transmisión de datos opera vía ESP-NOW (*peer-to-peer*) hacia el Gateway para minimizar el tiempo de radio encendida. Si el Gateway no emite confirmación (ACK), el sistema inicializa *On-Demand* el lector MicroSD (bus VSPI), empaqueta las lecturas con **Nanopb** (Protobuf), las anexa a un archivo binario y apaga el bus SPI por completo. Al recuperar conexión, la "Caja Negra" se vacía dinámicamente enviando lotes máximos de 15 registros para prevenir caídas de tensión (Brown-out).

---

## 📊 Diagnóstico en Campo

En cada ciclo de recolección, el firmware registra los motivos de despertar y reinicio, heap libre y el mínimo de stack disponible de la tarea de sensores. Smart Light-Sleep preserva el contexto completo del sistema (I²C, FreeRTOS, BSEC) entre mediciones, garantizando que los diagnósticos reflejen el estado acumulado real del dispositivo.

El BMV080 se integra exclusivamente a través de `bmv080_driver`, que encapsula el SDK binario de Bosch. Las transferencias de sus callbacks se validan y limitan a 512 palabras para proteger el heap ante datos anómalos del SDK.

---

## 📁 Estructura del Proyecto

```
room-monitoring/
├── main/                        # Orquestador principal (room-monitoring.c)
├── components/
│   ├── bme688_bsec/            # BSEC 3.0 wrapper (fusión sensorial IAQ)
│   ├── bme688_driver/          # Driver bajo nivel BME688
│   ├── bmv080_driver/          # SDK binario Bosch BMV080 (PM láser)
│   ├── bosch_hal/              # HAL compartido Bosch (I2C callbacks)
│   ├── scd41/                  # Driver CO₂ Sensirion (NDIR)
│   ├── rv1805_rtc/             # RTC hardware Qwiic (±2 ppm)
│   ├── i2c_bus/                # Abstracción I2C + Bit-Banging recovery
│   ├── i2c_hal/                # HAL I2C bajo nivel
│   ├── power_manager/          # Gestión de energía y modos de sueño
│   ├── network_manager/        # ESP-NOW (TX/RX, ACK, peer management)
│   ├── storage_manager/        # MicroSD Store-and-Forward (FATFS/SPI)
│   ├── telemetry/              # Empaquetado de datos de telemetría
│   ├── telemetry_proto/        # Nanopb auto-generado (.pb.c/.pb.h)
│   └── nanopb/                 # Upstream Nanopb (protobuf para embedded)
├── proto/                       # telemetry.proto (esquema canónico)
├── docs/                        # ADRs (Architecture Decision Records)
├── .agents/                     # Políticas normativas (3 capas)
│   ├── AGENTS.md               # Capa 1: Política global ejecutiva
│   └── policies/
│       ├── github-governance.md # Capa 2: Estándar GitHub
│       └── sensor-node-profile.md # Capa 3: BOM, erratas, pinout, riesgos
├── CHANGELOG.md
└── README.md
```

---

## 🚀 Compilación y Desarrollo (ESP-IDF)

### 1. Preparar Entorno
```bash
# Cargar variables de compilación del ESP-IDF v5.3+
. $HOME/esp/esp-idf/export.sh
```

### 2. Auto-Mantenimiento de Código (Pre-Commit)
El código debe cumplir estrictamente con los lineamientos de `.clang-format` (Estilo LLVM/Espressif). Asegúrate de tener instalado y activado el hook local:
```bash
pipx install pre-commit
pre-commit install
```

### 3. Compilar, Flashear y Monitorizar
```bash
idf.py set-target esp32
idf.py build flash monitor
```

---

## 🔒 Buenas Prácticas de GitHub (Workspace Rules)
Este proyecto sigue políticas estrictas de gobierno:
- **Commits:** Uso exclusivo de **Conventional Commits** (`feat:`, `fix:`, `docs:`, `refactor:`).
- **Control de Versiones:** `CHANGELOG.md` mantenido bajo estándar **Keep a Changelog** y **SemVer**.
- **Pull Requests:** Código nuevo debe pasar linters, compilación sin warnings en rutas críticas y adjuntar pruebas unitarias.

---

## 📝 Roadmap

- [x] **Fase 1:** Arquitectura de Componentes (HAL BME688) e I2C Recovery.
- [x] **Fase 2a:** Máquina de Estados de Doble Despertar (4.85s) y Aislamiento `gpio_hold_en`.
- [x] **Fase 2b:** Integración Total de SCD41, BMV080, RTC Hardware Híbrido (RV-1805) y estabilización de bus I2C.
- [x] **Fase 3:** Telemetría Resiliente ESP-NOW y "Caja Negra" Store-and-Forward (MicroSD SPI) con Nanopb.
- [x] **Fase 3b:** Auditoría BSEC Deep Sleep — ADR-001 aprobado. Pivot a Smart Light-Sleep. Ver [`docs/ADR-001-Power-Management-BSEC.md`](docs/ADR-001-Power-Management-BSEC.md).
- [ ] **Fase 3c:** Refactor Smart Light-Sleep (3 modos: 5s / 1min / 5min con `esp_light_sleep_start()`).
- [ ] **Fase 4:** Gateway Criptográfico Edge (ESP32-P4) con conectividad a Google Cloud.
- [ ] **Fase 5:** Inteligencia Embebida BSEC 3.0 y TinyML para Clasificación Química.

---

## 📄 Decisiones Arquitectónicas

| ADR | Título | Estado |
|-----|--------|--------|
| [ADR-001](docs/ADR-001-Power-Management-BSEC.md) | Power Management — BSEC Deep Sleep Failure & Smart Light-Sleep Pivot | ✅ Aceptado |
