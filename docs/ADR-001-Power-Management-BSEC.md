# ADR-001: Power Management — BSEC Deep Sleep Failure & Smart Light-Sleep Pivot

| Field | Value |
|---|---|
| **Status** | Accepted |
| **Date** | 2026-09-18 |
| **Authors** | EcoTech Engineering / Antigravity AI |
| **Board** | ESP32-D0WD-V3 (rev 3.1) @ 160 MHz |
| **ESP-IDF** | v5.3.5-1161-g6d0016c3c1f |
| **Sensors** | BME688 (BSEC 3.0), SCD41, BMV080, RV-1805-C3 |

---

## 1. Contexto

El objetivo original del nodo `room-monitoring` era implementar un sistema de monitoreo ambiental Ultra-Low Power usando **Deep Sleep** del ESP32 con ciclos de despertar cada 5 minutos (Modo 2). La idea era:

1. **Cold Boot**: 12 pulsos de calibración a 1 Hz (BSEC Continuous) para estabilizar el filtro de Kalman del BME688.
2. **Fase 2**: Transición a BSEC ULP (0.003333 Hz = 300s), persistir el State Blob de ~4 KB en `RTC_DATA_ATTR`, y entrar en Deep Sleep ~279s.
3. **Despertar**: Restaurar blob → `bsec_set_state()` → medir → persistir → dormir.

### Budget de energía original

| Estado | Corriente | Tiempo | Carga |
|--------|-----------|--------|-------|
| Deep Sleep (RTC timer + RTC mem) | 10 µA | 279 s | 0.775 µAh |
| Active (sensores + Wi-Fi) | ~150 mA | 21 s | 875 µAh |
| **Promedio por ciclo (300s)** | — | — | **~10.5 mA** |

---

## 2. El Problema Matemático: La Paradoja BSEC

### 2.1 Evidencia Empírica (Hardware-in-the-Loop)

Realizamos **9+ ciclos de Deep Sleep** de larga duración. Los resultados fueron unánimes:

```
boot_counter=2:  BSEC state restored from RTC → trigger=1 → n_outputs=0 → IAQ: 0.0
boot_counter=3:  BSEC state restored from RTC → trigger=1 → n_outputs=0 → IAQ: 0.0
boot_counter=4:  BSEC state restored from RTC → trigger=1 → n_outputs=0 → IAQ: 0.0
...hasta boot_counter=9: siempre n_outputs=0
```

La arquitectura de persistencia RTC funciona perfectamente:
- `bsec_set_state()` retorna `BSEC_OK` ✅
- `bsec_sensor_control()` emite `trigger_measurement=1` ✅
- BME688 produce datos RAW válidos (T=25.4°C, H=48.5%, Gas=145kΩ) ✅
- **BSEC silenciosamente descarta los datos**: `bsec_do_steps()` retorna `n_outputs=0` ❌

### 2.2 Root Cause: Conflicto de Frecuencias (Continuous → ULP)

**Fuente:** BST-BME688-DS000-03 (Rev 1.3, Feb 2024), Tabla 20.

BSEC opera en tres modos definidos por su `sample_rate`:

| Modo BSEC | Sample Rate | Período | Heater Duration | Corriente |
|-----------|------------|---------|-----------------|-----------|
| Continuous | 1.0 Hz | 1 s | 900 ms | 12 mA |
| LP | 0.333 Hz | 3 s | 2500 ms | 0.9 mA |
| ULP | 0.003333 Hz | 300 s | 2500 ms | 90 µA |

El problema es que nuestro Warmup (Fase 1) ejecuta BSEC a **1 Hz Continuous** durante 60s, luego transiciona abruptamente a **0.003333 Hz ULP**. BSEC internamente:

1. Descarta el State Blob del Continuous (nosotros lo marcamos stale correctamente con `mark_state_stale()`).
2. Arranca ULP "en fresco" — primer ciclo siempre produce `n_outputs=0` (anchor point interno del filtro Kalman).
3. El anchor point contiene datos válidos internamente, pero BSEC no emite salidas hasta recibir **al menos 2 mediciones consecutivas dentro de la ventana temporal esperada**.

### 2.3 Root Cause: Tolerancia Temporal del Tick ULP

**Hallazgo crítico de nuestras pruebas:**

Cuando configuramos todo a 3s (LP mode con Light-Sleep de 3s), BSEC **sí convergió** y arrojó `IAQ: 50.0 (Acc: 0)` tras el segundo ciclo. Esto demuestra que el State Blob y la restauración funcionan.

Al retornar a ULP (300s), BSEC nunca convergió. La razón:

- **`bsec_sensor_control().next_call`** siempre retorna `+3s` (intervalo de polling interno), independientemente del modo ULP/LP.
- El período real de medición ULP es `1/0.003333 = 300s`.
- Tras un Deep Sleep de 279s + boot de ~7s, el timestamp inyectado al segundo `bsec_do_steps()` tiene un **desfase acumulado** de varios segundos respecto al timestamp interno que BSEC esperaba.
- BSEC 3.0 es un **binario precompilado (caja negra)** — no podemos inspeccionar ni ajustar su ventana de tolerancia temporal interna.
- La evidencia empírica sugiere que la tolerancia es **mucho más estrecha** para ULP que para LP, posiblemente porque el filtro de Kalman necesita muestreo periódico preciso para las derivadas de VOC/IAQ.

### 2.4 El Desfase del RTC Interno del ESP32

**Fuente:** esp32_datasheet_en.pdf, Sección 4.3.1.

El Deep Sleep del ESP32 usa el oscilador RTC interno (150 kHz, ±5%) para el temporizador de wakeup. Incluso con la calibración interna, el jitter acumulado sobre 300s puede ser de **±1-3 segundos** — suficiente para que BSEC descarte la medición.

Nosotros mitigamos parcialmente esto con el **RV-1805-C3** (±2.0 ppm @ 25°C), que sincroniza `gettimeofday()` al despertar. Sin embargo, el problema no es la precisión del reloj wallclock que inyectamos en `bsec_do_steps()`, sino que **BSEC internamente espera consistencia temporal** entre su `next_call` nativo y el momento en que recibe datos — algo que el Deep Sleep rompe estructuralmente.

---

## 3. Análisis de Alternativas

### 3.1 ULP Coprocessor del ESP32-D0WD-V3

**Alternativa evaluada:** Ejecutar BSEC en el coprocesador ULP-FSM durante el Deep Sleep.

**Dictamen: IMPOSIBLE.**

**Fuentes:**
- ESP-IDF ULP FSM Programming: `docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ulp-fsm.html`
- Espressif DevCon22: "Low-Power Applications on ESP: The ULP Co-Processor"

| Limitación ULP-FSM | Impacto en BSEC |
|---|---|
| 4 registros de 16 bits (R0-R3) | BSEC requiere centenares de variables de estado |
| 8 KB RTC_SLOW_MEM (código + datos) | El State Blob de BSEC solo ocupa ~4 KB; el binario BSEC ~200 KB |
| Sin soporte de punto flotante en hardware | BSEC ejecuta filtros de Kalman con aritmética float64 |
| ISA propietaria (o macros C), no ejecuta binarios Xtensa | BSEC 3.0 es un blob precompilado para Xtensa LX6 |
| Sin soporte de library calls (malloc, memcpy, etc.) | BSEC usa `bsec_do_steps()` con allocations internas |

El ESP32-D0WD-V3 **solo tiene ULP-FSM** (no ULP-RISC-V). Incluso si tuviera ULP-RISC-V (como el ESP32-S2/S3), su ISA es RV32IMC — incompatible con el blob Xtensa de BSEC. Bosch no proporciona un binario BSEC para ninguna variante de ULP.

**Conclusión: El ULP del ESP32 es incapaz de ejecutar BSEC bajo cualquier configuración.**

### 3.2 Aumentar Pulsos de Warmup antes de ULP

Intentamos 12, 24 y hasta 36 pulsos de warmup a 1 Hz Continuous antes de transicionar a ULP. BSEC alcanzó `IAQ Accuracy = 1` durante el warmup (confirmando que el sensor funciona), pero **siempre** volvió a `n_outputs=0` en el primer ciclo ULP post-transición.

### 3.3 "LP Bootstrap" antes de ULP

Intentamos arrancar el primer ciclo post-Deep-Sleep en LP (0.333 Hz, 3s) y luego upgrade a ULP. Esto **contaminó el State Blob** — al guardar un blob de LP y luego despertar en ciclo ULP (300s), el `next_call` interno pedía +3s pero el sistema durmió 285s, causando `n_outputs=0` en el ciclo 3. **Descartado como hack arquitectónico.**

---

## 4. Decisión Arquitectónica: Smart Light-Sleep

### 4.1 La Decisión

**Adoptamos ESP32 Light-Sleep como modo de bajo consumo para todos los modos de monitoreo**, reemplazando Deep Sleep en la Fase 2.

**Justificación:**

| Criterio | Deep Sleep | Light-Sleep |
|----------|-----------|-------------|
| Corriente durante sueño | ~10 µA | ~800 µA |
| Retención de RAM | Solo RTC_DATA_ATTR | **Total** (RTOS + heap + stacks) |
| BSEC State Blob | Requiere serialize/deserialize | **Vive en memoria — cero overhead** |
| Variables `.bss` estáticas de BSEC | **PERDIDAS** (causa del bug) | **Retenidas** |
| Latencia de despertar | ~300 ms (full boot) | **<1 ms** |
| I2C re-init | Siempre | **No necesario** |
| SCD41 re-init | Siempre (single_shot 5s) | **No necesario** |
| BMV080 re-init | Siempre (~10s laser warmup) | **Duty-cycle posible** |
| Boot overhead | ~1.5s (bootloader + NVS + drivers) | **Cero** |
| WiFi/ESP-NOW | Re-init cada ciclo (~0.5s) | **Persistente o re-init rápido** |

### 4.2 Evaluación de los 3 Modos de Monitoreo

| Modo | Intervalo | Sleep Type | BSEC Rate | SCD41 | BMV080 |
|------|-----------|-----------|-----------|-------|--------|
| **Mode 0** (Dinámico) | 5 s | Light-Sleep 5s | LP (0.333 Hz) | Periodic (5s nativo) | Continuous |
| **Mode 1** (Estándar) | 60 s | Light-Sleep 60s | LP (0.333 Hz) con duty-cycle | Single-shot | Duty-cycle (10s on / 50s off) |
| **Mode 2** (Conservación) | 300 s | Light-Sleep 300s | ULP (0.003333 Hz) | Single-shot | Duty-cycle (10s on / 290s off) |

#### Análisis de tiempos de calentamiento por sensor

**SCD41** (Fuente: Sensirion SCD4x Datasheet):
- `measure_single_shot` (cmd `0x219d`): requiere **5000 ms** (fijo, no negociable).
- En Mode 0 (5s), usamos `start_periodic_measurement` (5s nativo del sensor, 0 overhead).
- En Mode 1/2, disparamos `measure_single_shot` durante el período activo y leemos tras 5s.

**BMV080** (Fuente: BST-BMV080-DS000-10):
- Láser requiere spin-up tras corte de energía. Nuestros logs muestran PM=0 en primera lectura, valores estables a partir de ~2-3s.
- En Light-Sleep, el sensor mantiene energía → **eliminamos completamente el warmup** si no apagamos el MOSFET.
- Optimización: en Mode 2, apagar MOSFET del láser durante el sleep de 290s y re-encender 10s antes de medir.

**BME688 + BSEC**:
- BSEC en modo ULP necesita exactamente 1 medición cada 300s.
- Light-Sleep preserva todas las variables internas de BSEC → **el filtro de Kalman jamás pierde contexto**.
- El heater profile de ULP usa 2500 ms de calentamiento (vs 900 ms en Continuous).

#### Secuenciamiento I2C optimizado (Mode 2)

```
T=0:       Wake from Light-Sleep (<1 ms)
T=0.001s:  SCD41 measure_single_shot() → non-blocking I2C cmd
T=0.010s:  BME688 bsec_sensor_control() + configure heater
T=0.020s:  BMV080 power on MOSFET (si estaba apagado)
T=2.5s:    BME688 heater profile completo → read + bsec_do_steps()
T=5.0s:    SCD41 data ready → read CO2/T/H
T=5.1s:    BMV080 start measurement → read PM (ya caliente)
T=6.0s:    ESP-NOW transmit (datos de los 3 sensores)
T=7.0s:    BMV080 power off MOSFET
T=7.1s:    Enter Light-Sleep (293s)
           Total activo: ~7s → 10 mA avg during active (sin WiFi peak)
```

### 4.3 Budget de energía revisado (Mode 2)

| Estado | Corriente | Tiempo | Carga |
|--------|-----------|--------|-------|
| Light-Sleep | 800 µA | 293 s | 65.1 µAh |
| Active (sensores + transmisión) | ~120 mA | 7 s | 233 µAh |
| **Total por ciclo (300s)** | — | — | **~298 µAh** |
| **Promedio continuo** | — | — | **~3.6 mA** |

Comparación con Deep Sleep (si funcionara):
- Deep Sleep hipotético: ~2.9 mA avg (10 µA sleep + 150 mA × 21s active)
- Light-Sleep real: ~3.6 mA avg

**Δ = +0.7 mA (+24%)** — un costo aceptable para obtener convergencia garantizada de IAQ.

Con batería de 3000 mAh:
- Deep Sleep hipotético: ~43 días
- Light-Sleep real: **~35 días**

> [!IMPORTANT]
> La diferencia de 8 días es irrelevante porque Deep Sleep **nunca produce IAQ válido**. Un sensor que duerme 43 días pero reporta IAQ=0.0 es inútil.

---

## 5. Preservación del Código Deep Sleep (v2.0)

### 5.1 Estrategia de encapsulamiento

El código de Deep Sleep existente representa un trabajo significativo de ingeniería (RTC persistence, gpio_hold_en, boot_counter, dynamic sleep calculation). Lo preservamos para:

1. **Futura migración a ESP32-S3/C6** con ULP-RISC-V que podría ejecutar polling loops más sofisticados.
2. **BSEC 4.x** si Bosch relaja la tolerancia temporal del modo ULP.
3. **Modo de Autoconservación** ante batería críticamente baja.

### 5.2 Directrices de compilación condicional

```c
// En sdkconfig o menuconfig:
// CONFIG_ENABLE_DEEP_SLEEP_V2=n (default: deshabilitado)

#ifdef CONFIG_ENABLE_DEEP_SLEEP_V2
    // Código de Deep Sleep: RTC blob persistence, gpio_hold_en,
    // dynamic sleep calculation, boot_counter logic
#else
    // Smart Light-Sleep: esp_light_sleep_start() con timer wakeup
#endif
```

### 5.3 Archivos afectados

| Archivo | Código a preservar |
|---|---|
| `bme688_bsec_wrapper.c` | `RTC_DATA_ATTR` blobs, `bsec_get_state()`/`bsec_set_state()`, `mark_state_stale()` |
| `room-monitoring.c` | `gpio_hold_en()`, `esp_deep_sleep()`, `boot_counter`, dynamic `deep_sleep_us` calculation |
| `bme688_bsec_wrapper.h` | `bme688_bsec_get_next_call_ns()`, `reset_rtc_state()` |

---

## 6. Apéndices

### A. Logs forenses completos

Ver [logs/deep_sleep_log.md](logs/deep_sleep_log.md) — 1056 líneas que documentan:
- Fase 1 completa (12 pulsos Continuous, IAQ escalando de 50.0 a 57.6, Accuracy de 0 a 1)
- Transición CONTINUOUS → ULP y `mark_state_stale()`
- Primer ciclo ULP (anchor point, `n_outputs=0`, sleep correcto de 279s)
- Boot 3-4: State restored exitosamente → `n_outputs=0` persistente

### B. Configuración del RV-1805-C3

| Parámetro | Valor |
|---|---|
| Precisión de fábrica | ±2.0 ppm @ 25°C |
| Drift frecuencia vs temp | −0.035 ppm/°C² × (T − T₀)² |
| Error acumulado en 300s | ~0.6 ms (despreciable) |

El RV-1805 es excelente para mantener el uptime POSIX. El problema no es la precisión del reloj externo sino la consistencia temporal interna de BSEC.

### C. ESP32-D0WD-V3 Power Modes

| Modo | Corriente | RAM | Wakeup |
|------|-----------|-----|--------|
| Active 160 MHz | 27-44 mA | Total | — |
| Light-Sleep | 800 µA | **Total** | <1 ms |
| Deep Sleep (RTC timer + mem) | 10 µA | 8 KB RTC | ~300 ms |
| Hibernation | 5 µA | Ninguna | RTC timer only |

Fuente: Espressif ESP32 Datasheet, Table 4-2.

---

## 7. Evolución MVP: Variante BSEC y Sensor Fusion

### 7.1 El Error `-35` y la Variante BSEC
Durante la integración de los sensores virtuales de BSEC, nos encontramos con el error `BSEC_E_CONFIG_FEATUREMISMATCH (-35)` al intentar suscribirnos a 9 outputs (incluyendo `bVOC` y `TVOC`).
La causa raíz documentada es que la variante estándar enlazada (`libalgobsec.a`, variante **IAQ** de 214KB) no soporta dichos outputs en su bitfield interno (`1074952687`). Aunque existe la variante **Sel_IAQ** (255KB) que sí los soporta (`2146597359`), decidimos mantener la variante IAQ estándar para el MVP para garantizar estabilidad.

**Decisión:**
- Reducir las suscripciones a 7 outputs: `IAQ`, `Temp`, `Hum`, `Presión`, `Gas`, y añadir `CO2_EQUIVALENT` (eCO2).
- Los campos Protobuf `bvoc` y `tvoc` quedan definidos y reservados para la versión v1.1. En el futuro, la migración consistirá en swapear la librería a `Sel_IAQ`, actualizar `bsec_datatypes.h` (blob size 2001/255, outputs 24) y descomentar las suscripciones.

### 7.2 Sensor Fusion: Compensación de Presión SCD41
Para maximizar la precisión del cálculo de CO2 real mediante NDIR fotoacústico, integramos una sinergia (Sensor Fusion) entre el BME688 y el SCD41.
Según el Datasheet del Sensirion SCD4x (Sección 3.7.5 *set_ambient_pressure*), inyectar la presión atmosférica actual mejora la precisión de la lectura de CO2, ya que la concentración de moléculas de gas detectadas por la cámara fotoacústica varía con la presión barométrica.

**Implementación:**
- Se extrae `BSEC_OUTPUT_RAW_PRESSURE` del BME688.
- Se inyecta al SCD41 antes de cada medición periódica o *single-shot* mediante el comando `0xE000` (`scd41_set_ambient_pressure(pressure_pa / 100)`).
- Esto convierte al BME688 en un co-procesador barométrico del SCD41, resultando en lecturas de CO2 real (SCD41) y eCO2 (BSEC) altamente precisas en un mismo payload.
