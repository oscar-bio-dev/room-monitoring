# Room Monitoring Sensor Node — Perfil de Repositorio (Capa 3)

> **Anexo normativo específico** del proyecto `room-monitoring`.
> Este documento complementa la Capa 1 (AGENTS.md) y la Capa 2 (github-governance.md).
> Terminología normativa: **MUST** (obligatorio), **SHOULD** (recomendado), **MAY** (opcional).

## §1. Declaración de Hardware (Bill of Materials Normativo)

| Rol | Componente | Detalle |
|-----|-----------|---------|
| **MCU** | ESP32-WROOM (ESP32-D0WD-V3) | Xtensa dual-core (240 MHz), rev v3.1, 520 KB SRAM |
| **Placa** | SparkFun IoT RedBoard ESP32 | v10, Qwiic I2C, USB-C, MicroSD Slot |
| **Sensor IAQ** | Bosch BME688 | Temp, Hum, Presión, VOCs (I2C: 0x76/0x77) |
| **Sensor CO₂** | Sensirion SCD41 | NDIR, 400-5000 ppm (I2C: 0x62) |
| **Sensor PM** | Bosch BMV080 | Láser, PM1.0/PM2.5/PM10 (I2C: 0x54-0x57) |
| **RTC Hardware** | Micro Crystal RV-1805 | Qwiic, backup de tiempo para Deep Sleep |
| **Storage** | MicroSD (slot onboard) | SPI (VSPI), modo FATFS on-demand |
| **LDO** | AP2112K-3.3TRG1 | 3.3V, 600mA continuo |
| **Radio** | ESP-NOW (vía PHY integrada) | Peer-to-peer, CCMP-128, canal configurable |

## §2. Erratas de Silicio y Workarounds Obligatorios

### 2.1 ESP32 v3.1 — RTC Register Read Error After Light-Sleep [RTC-126]

**Afecta:** Todas las revisiones (v0.0 a v3.1).

**Síntoma:** Si un periférico RTC se apaga durante el Light-sleep, hay probabilidad de que al despertar la CPU lea registros del dominio RTC incorrectamente.

**Workaround obligatorio:**
- NO apagar periféricos RTC durante el Light-sleep (modo Micro-Sleep de 4.85s).
- El framework ESP-IDF gestiona esto automáticamente si no se llama a `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF)`.

> ⚠️ **REGLA INMUTABLE:** En el intervalo de Light-sleep (WAKE_A → WAKE_B), MUST mantener los periféricos RTC encendidos. Solo en el Deep Sleep maestro se permite el apagado completo.

### 2.2 ESP32 v3.1 — ULP y Touch Sensors incompatibles con RTC_PERIPH [ULP-3.19]

**Afecta:** Todas las revisiones.

**Implicación:** Si se necesita EXT0 wakeup, los periféricos RTC deben permanecer encendidos, pero el ULP y touch sensor dejarán de funcionar.

**Decisión arquitectónica:** Este nodo usa `esp_sleep_enable_timer_wakeup()` exclusivamente. No se usa EXT0, ULP ni touch. No hay conflicto.

### 2.3 Memoria RTC para BSEC

**Contexto:** La librería BSEC v3.3 de Bosch requiere persistir ~2 KB de estado de calibración entre ciclos de Deep Sleep.

**Patrón obligatorio:**
- El estado BSEC MUST almacenarse en memoria RTC Slow (`RTC_DATA_ATTR`).
- La memoria RTC Slow se mantiene viva automáticamente cuando hay variables `RTC_DATA_ATTR` declaradas en el programa.
- El bug de corrupción de RTC en alta temperatura (ar2024-005) **NO afecta** al ESP32 clásico (solo ESP32-C3/S3).

## §3. Pinout Map — SparkFun IoT RedBoard ESP32

> Mapa de pines críticos validados en laboratorio. Solo se listan los pines con asignación funcional confirmada.

### 3.1 I2C / Qwiic (Bus Maestro)

| GPIO | Función | Notas |
|------|---------|-------|
| **21** | `I2C_SDA` | Conector Qwiic JST. Pull-ups integrados en PCB. |
| **22** | `I2C_SCL` | Conector Qwiic JST. Pull-ups integrados en PCB. |

**Velocidad:** 100 kHz (modo estándar).
**Recovery:** 9 pulsos de reloj bit-banging + condición STOP en cada Cold Boot.
**Retención Deep Sleep:** `gpio_hold_en(21)` y `gpio_hold_en(22)` antes de dormir para evitar I2C Latch-up.

### 3.2 MicroSD (VSPI — SPI2)

| GPIO | Función | Notas |
|------|---------|-------|
| **5** | `SD_CS` | Chip Select |
| **18** | `SD_SCK` | Clock |
| **23** | `SD_MOSI` | Data Out |
| **19** | `SD_MISO` | Data In |

**Modo:** FATFS sobre SPI (no SDMMC nativo).
**Política energética:** Bus apagado por defecto. Solo se activa en fallback de red (Store-and-Forward). Los pines MUST revertirse a alta impedancia (`gpio_reset_pin()`) tras cada operación.

### 3.3 USB-Serial (Debug/Flash)

| Pin | Función | Notas |
|-----|---------|-------|
| USB-C | CP2102N USB-UART Bridge | Para flash y monitor (`idf.py flash monitor`) |

## §4. Contrato de Datos (Protobuf / Nanopb)

### Single Source of Truth
El esquema canónico del ecosistema es `TelemetryPayload` definido en:
- **Gateway:** `edge-telemetry-gateway/proto/telemetry.proto`
- **Backend:** `oscar-bio-dev/proto/telemetry.proto`

### Reglas del Nodo
1. El `.proto` del nodo MUST usar el **mismo nombre de mensaje** (`TelemetryPayload`) y los **mismos field numbers** que el esquema canónico.
2. El nodo MUST usar `package telemetry;` para alinearse con Gateway/Backend.
3. Los campos de identidad (`event_id`, `gateway_id`, `device_id`) y de ingestión (`ingested_at_ms`) MUST **omitirse** del `.proto` del nodo. Los inyecta el Gateway.
4. Los campos `string` MUST omitirse del Nanopb del nodo para evitar buffers dinámicos.
5. Todos los campos de telemetría MUST ser `optional` para soportar presencia dinámica.
6. Las banderas `has_*` MUST activarse en C solo si el sensor reportó lecturas válidas.

### Field Number Map (Nodo → Gateway)

| Campo | Field # | Tipo | Fuente |
|-------|---------|------|--------|
| `protocol_version` | 1 | uint32 | Fijo (1) |
| `schema_version` | 2 | uint32 | Fijo (1) |
| `node_sequence` | 6 | uint32 | Contador RTC |
| `measured_at_ms` | 7 | uint64 | RTC Hardware (ms) |
| `temperature` | 9 | float | BME688/BSEC |
| `humidity` | 10 | float | BME688/BSEC |
| `pressure` | 11 | float | BME688/BSEC |
| `gas_resistance` | 12 | float | BME688/BSEC |
| `iaq` | 13 | float | BME688/BSEC |
| `co2` | 14 | uint32 | SCD41 |
| `pm1_0` | 15 | float | BMV080 |
| `pm2_5` | 16 | float | BMV080 |
| `pm10_0` | 17 | float | BMV080 |
| `battery_mv` | 20 | uint32 | ADC (futuro) |
| `sleep_cycles` | 21 | uint32 | Contador RTC |

## §5. Máquina de Estados de Doble Despertar

```
                  ┌──────────────────────────────┐
                  │     DEEP SLEEP (Master)       │
                  │  3s (calibración) / 5min (op) │
                  └──────────────┬───────────────┘
                                 │ Timer Wakeup
                                 ▼
                  ┌──────────────────────────────┐
                  │       WAKE A: Trigger         │
                  │  • BMV080: Calentar láser     │
                  │  • SCD41:  Trigger single-shot│
                  │  • BME688: BSEC read IAQ      │
                  └──────────────┬───────────────┘
                                 │
                                 ▼
                  ┌──────────────────────────────┐
                  │    LIGHT SLEEP (4.85s)        │
                  │  SCD41 procesa su medición    │
                  │  I2C bus preservado           │
                  └──────────────┬───────────────┘
                                 │ Timer Wakeup
                                 ▼
                  ┌──────────────────────────────┐
                  │     WAKE B: Collect & TX      │
                  │  • SCD41:  Leer CO₂           │
                  │  • BMV080: Leer PM (purga+1)  │
                  │  • Pack:   Protobuf (Nanopb)  │
                  │  • TX:     ESP-NOW encriptado  │
                  │  • Fail?:  SD Store-and-Fwd   │
                  │  • OK?:    Vaciar Caja Negra  │
                  └──────────────┬───────────────┘
                                 │
                                 ▼
                  ┌──────────────────────────────┐
                  │  gpio_hold_en(SDA, SCL)       │
                  │  Aislar SPI (si se usó SD)    │
                  │          → DEEP SLEEP         │
                  └──────────────────────────────┘
```

## §6. Checks de CI Específicos del Nodo

### Build Matrix
| Firmware | Target | Path | Imagen Docker |
|----------|--------|------|---------------|
| Sensor Node | `esp32` | `.` (raíz) | `espressif/idf:v5.3` |

### Exclusiones de Terceros
Los siguientes directorios MUST excluirse de `clang-format` y hooks de estilo:
- `components/nanopb/` — Librería Nanopb upstream
- `components/telemetry_proto/src/*.pb.*` — Archivos auto-generados por Nanopb
- `components/bme688_bsec/lib/` — Binarios precompilados de Bosch BSEC v3.3
- `components/bmv080_driver/lib/` — Binarios precompilados de Bosch BMV080

### Size Gate
- Binario del nodo MUST caber en partición factory (1 MB = `0x100000` bytes).
- Margen libre SHOULD ser ≥ 15% para futuras expansiones (BSEC state, nuevos sensores).

## §7. Riesgos Conocidos y Mitigaciones

| # | Riesgo | Severidad | Mitigación | Estado |
|---|--------|-----------|-----------|--------|
| R1 | I2C Latch-up tras Deep Sleep (SDA LOW) | 🔴 Crítico | `gpio_hold_en()` + 9 pulsos recovery en Cold Boot | ✅ Mitigado |
| R2 | BSEC state corruption entre ciclos de sueño | 🔴 Crítico | Estado en `RTC_DATA_ATTR`, restauración en init | ✅ Mitigado |
| R3 | BMV080 FIFO overflow por polling lento | 🟠 Alto | Fast-polling 100ms + buffer flush de 15 lecturas | ✅ Mitigado |
| R4 | Brown-out por ráfagas RF sostenidas | 🟠 Alto | Batched Recovery: máx 15 registros por despertar | ✅ Mitigado |
| R5 | ESP-NOW sin encriptación (texto plano) | 🔴 Crítico | PMK/LMK vía Kconfig + `encrypt=true` | 🔄 En progreso |
| R6 | Protobuf field numbers desalineados con Gateway | 🔴 Crítico | Reescritura del `.proto` al Mega-Esquema canónico | 🔄 En progreso |
| R7 | Un solo desarrollador → bus factor = 1 | 🟡 Medio | Documentación exhaustiva, ADRs, gobernanza | 🔄 En progreso |
| R8 | ADC de batería no implementado | 🟡 Medio | Campo `battery_mv` presente pero siempre 0 | ⏳ Pendiente |
| R9 | Provisioning de llaves ESP-NOW sin UI | 🟡 Medio | Kconfig para desarrollo; NVS encriptado para producción | ⏳ Pendiente |

## §8. Roadmap de Hardening de Seguridad

| Fase | Feature | Prioridad | Dependencia |
|------|---------|-----------|-------------|
| Actual | **ESP-NOW Encryption** (PMK/LMK, CCMP-128) | 🔴 Alta | MAC del Companion C6 |
| Post-Test 2 | **Secure Boot v2** (ECDSA, eFuse key) | Alta | Estabilidad de firmware |
| Post-Test 2 | **Flash Encryption** (Development → Release) | Alta | Secure Boot primero |
| Futuro | **NVS Encryption** (HMAC-based) | Media | Flash Encryption habilitado |
| Futuro | **OTA Seguro** (firma de imágenes desde Gateway) | Alta | Secure Boot + IPC confiable |

### Checklist Pre-Hardening
- [ ] Confirmar chip capability matrix para ESP32 clásico (eFuse layout, key slots)
- [ ] Documentar estrategia de key management (per-device vs shared)
- [ ] Crear ADR dedicado para decisiones de seguridad
- [ ] Validar que BSEC state sobrevive al flash encryption sin corrupción
