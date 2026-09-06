# AGENTS.md — Normativa Global del Workspace (room-monitoring)

> **Normativa de cumplimiento obligatorio** para todo agente o desarrollador en este workspace.
> Terminología normativa: **MUST** (obligatorio), **SHOULD** (recomendado), **MAY** (opcional).

## 0) Estructura de Políticas (3 Capas)
Este documento representa la **Capa 1 (Política Global Ejecutiva)**. Todo cambio en este repositorio MUST adherirse además a:
- **Capa 2 (Estándar GitHub):** `policies/github-governance.md` (Rulesets, CI/CD, Supply Chain).
- **Capa 3 (Perfil del Nodo Sensor):** `policies/sensor-node-profile.md` (Hardware BOM, erratas de silicio, pinout, restricciones energéticas, riesgos).

> ⚠️ **INSTRUCCIÓN CRÍTICA PARA EL AGENTE:**
> Antes de modificar código de hardware (GPIO, I2C, SPI, Deep Sleep, PMU), protocolo de red (ESP-NOW), o la serialización de datos (Protobuf/Nanopb), **MUST** leer primero `policies/sensor-node-profile.md` para consultar erratas de silicio, pinout validado, contrato de datos con el Gateway y patrones obligatorios de energía.

> ⚠️ **CONSULTA MCP OBLIGATORIA:**
> Para cualquier cambio que afecte inicialización de hardware o selección de componentes, el agente MUST consultar proactivamente:
> - `esp-pilot-mcp` → `catalog_list_socs`, `catalog_list_bmgr_boards`, `catalog_get_bmgr_doc`
> - `espressif-engineering` → `ts_skill_get` (channels: `hardware`, `security`, `solution`)
> - `espressif-documentation` → `search_espressif_sources` (para APIs y ejemplos oficiales)

## 1) Identidad del Proyecto
- **Tipo:** Nodo Sensor ambiental de ultra-bajo consumo (Edge IoT).
- **SoC:** ESP32-WROOM (ESP32-D0WD-V3), revisión de silicio v3.1.
- **Placa:** SparkFun IoT RedBoard ESP32.
- **Framework:** ESP-IDF v5.3.x (Xtensa dual-core).
- **Rol en el ecosistema:** Recolección de datos sensoriales → Empaquetado Protobuf → Transmisión ESP-NOW → Gateway Edge (ESP32-P4).
- **Conectividad:** El nodo **NUNCA** se conecta a Wi-Fi (802.11 b/g/n). La radio opera **exclusivamente** en modo ESP-NOW para máxima eficiencia energética.
- **Inteligencia local:** Solo la librería precompilada **BSEC v3.3** de Bosch (fusión sensorial BME688). Cero TensorFlow Lite u otros modelos pesados. El procesamiento AI se delega al Gateway P4.

## 2) Flujo de trabajo y trazabilidad
- Todo cambio no trivial MUST estar vinculado a un Issue.
- Commits MUST seguir Conventional Commits (`feat:`, `fix:`, `refactor:`, `chore:`...).
- `CHANGELOG.md` MUST seguir Keep a Changelog + SemVer.
- Puntos de Restauración Seguros (Git): Una vez completado el "scaffolding", es obligatorio inicializar el repositorio y generar un commit inicial.
- README MUST reflejar estado real del proyecto; no se permite "feature drift".
- Decisiones de arquitectura críticas MUST registrarse en `docs/adr/`.

## 3) Arquitectura de Software e Infraestructura
- `/main` MUST contener solo orquestación y arranque.
- Drivers/HAL/servicios MUST vivir en `/components/<modulo>`.
- Cada componente MUST tener: `include/*.h` (API pública), `*.c` (implementación), `CMakeLists.txt` y tests unitarios.
- APIs entre componentes MUST ser explícitas y desacopladas.
- **Topología Segregada (Nodo → Gateway → Cloud):** El nodo sensor MUST utilizar comunicaciones de radio ultracortas de milisegundos (ESP-NOW) hacia un Gateway dedicado. Las pesadas rutinas criptográficas (TLS/JWT/TCP-IP) se delegan al Gateway (ESP32-P4 + Ethernet PoE).
- **Contrato de Datos:** El Protobuf del nodo MUST ser un subconjunto estricto del `TelemetryPayload` canónico del Gateway, usando los mismos field numbers. El nodo **NO** envía `device_id`; será el Companion C6 del Gateway el responsable de inyectar la dirección MAC del nodo como identificador.

## 4) Concurrencia, Núcleos y Tiempo Real
- En dual-core, tareas críticas MUST crearse con `xTaskCreatePinnedToCore()`.
- Stack de red SHOULD residir en Core 0 y hardware/sensores en Core 1.
- Bloqueos activos MUST evitarse; usar notificaciones, colas, event groups y `vTaskDelay`.
- Gestión de Memoria Segura: Favorecer variables estáticas locales. Prohibido usar `malloc`/`free` en rutas calientes (hot paths) para evitar fragmentación.

## 5) Resiliencia y Manejo de Errores
- Ningún `esp_err_t` puede ignorarse. Se MUST usar manejo explícito con `ESP_LOGE` + ruta de recuperación.
- Tolerancia a Fallos de Hardware (Sanity Checks): Previo a inicializar buses (I2C/SPI), se MUST validar el estado eléctrico de los pines e inyectar mecanismos de recuperación (ej. 9 pulsos de reloj Bit-Banging para Latch-Up).
- Timeouts/reintentos MUST definirse por componente y documentarse.
- **Lecturas con CRC:** Todas las lecturas críticas de sensores I2C (SCD41) MUST validarse mediante CRC-8 antes de aceptar los datos.

## 6) Energía y Deep Sleep
- Estado entre ciclos MUST persistirse con `RTC_DATA_ATTR` (mínimo footprint).
- **Aislamiento de Hardware (Pin Retention):** Durante el Deep Sleep, el dominio de energía principal colapsa. Es obligatorio aislar los dominios RTC (ej. `esp_sleep_pd_config`) y retener el estado lógico usando `gpio_hold_en()` sobre pines que alimenten buses externos (I2C) para prevenir apagones en sensores ópticos o corrientes parásitas.
- **Bus SPI MicroSD:** El bus VSPI de la tarjeta SD MUST permanecer apagado por defecto. Solo se inicializa *on-demand* si falla la transmisión ESP-NOW (Caja Negra / Store-and-Forward). Al finalizar la operación, los pines MUST revertirse con `gpio_reset_pin()` para impedir fugas de corriente.
- **Anti Brown-out (Batched Recovery):** Al vaciar el buffer offline, el nodo MUST enviar un máximo de 15 registros por despertar para evitar picos de corriente sostenidos por la radio que colapsen el regulador LDO.
- Cada módulo crítico SHOULD exponer métricas de consumo/latencia por ciclo.

## 7) Seguridad de Comunicaciones (ESP-NOW)
- **Encriptación obligatoria:** Los datos MUST transmitirse encriptados por ESP-NOW (CCMP-128). Queda **terminantemente prohibido** enviar telemetría en texto plano (`peer_info.encrypt = false` es un estado de desarrollo que MUST eliminarse antes de producción).
- **Gestión de Llaves:** PMK y LMK MUST inyectarse vía Kconfig (`CONFIG_ESPNOW_PMK`, `CONFIG_ESPNOW_LMK`) y NO hardcodearse en el código fuente. Los valores quedan en `sdkconfig` local, excluido por `.gitignore`.
- **MAC del Gateway:** La dirección MAC del Companion C6 MUST configurarse vía Kconfig (`CONFIG_ESPNOW_GATEWAY_MAC`). Queda prohibido usar broadcast (`0xFF...`) en producción (incompatible con encriptación ESP-NOW).

## 8) Calidad de Código C/C++
- Logs MUST usar `ESP_LOG*` con `static const char *TAG`. Ningún proyecto usará llamadas directas a `printf()` para diagnóstico.
- Tipado fijo MUST usar `<stdint.h>` para rutas críticas.
- Garantía Automática (Pre-Commit): Todo proyecto MUST incluir configuración de `pre-commit` ligada a `clang-format` para garantizar estilo inmaculado.
- Headers nuevos MUST incluir licencia/copyright.
- Regla de warnings en CI: **0 warnings** en rutas críticas.

## 9) Testing y Quality Gates
- Proyecto MUST incluir tests (Unity) para componentes críticos.
- Gate mínimo sugerido: Cobertura unit tests >= 80%, build exitoso, sin regresión de tamaño/heap.
- Pruebas HIL SHOULD cubrir: Recovery de bus I2C, wake/sleep repetido, y fallas de radio (ACK timeout).

## 10) Seguridad de Supply Chain (Delegado)
- Las políticas sobre CodeQL, Secret Scanning, Push Protection y Hardening de GitHub Actions (mínimo privilegio, pinning por SHA) MUST regirse por la **Capa 2**: `policies/github-governance.md`.

## 11) CI/CD mínimo obligatorio
- Pipeline MUST ejecutar: Format/lint (`clang-format`, `clang-tidy`), `idf.py build`, `idf.py size`, y tests unitarios.
- Artefactos por PR SHOULD incluir binarios y reporte de tamaño.
- Releases MUST usar tags SemVer y notas de versión.

## 12) Definición de Hecho (DoD)
Un cambio se considera "Done" solo si:
1. Código compila en CI y pasa todos los Linters/Formatters.
2. Tests y checks (Sanity) pasan.
3. Documentación y changelog actualizados.
4. Riesgos documentados en PR.
5. Aprobación requerida obtenida.

## 13) Tooling y Entorno de Desarrollo (VS Code + ESP-IDF)
- **Extensión Oficial MUST ser la única fuente de verdad:** Para compilación, flasheo y monitorización se usarán exclusivamente las herramientas de la Extensión Oficial de ESP-IDF (Status Bar). Queda terminantemente prohibido usar extensiones genéricas (ej. *CMake Tools* de Microsoft) para evitar corrupción del `build/` (ej. falsos `build.ninja` compilados con `gcc` del sistema).
- **Aislamiento de Configuración:** Los proyectos MUST evitar rutas absolutas hardcodeadas (ej. `/home/user/`) en el `.vscode/settings.json`. Se debe delegar la resolución de entornos de Python y SDKs al *ESP-IDF Installation Manager (EIM)* o usar variables dinámicas de la extensión cuando sea seguro.
- **IntelliSense Nativo:** El archivo `c_cpp_properties.json` MUST delegar la resolución de dependencias declarando `"configurationProvider": "espressif.esp-idf-extension"`.
- **Resolución de Fallos (Bring-up / Fallback):** Si la UI de la extensión colapsa por desincronización de Node.js/Python, los desarrolladores MUST validar la operatividad del hardware ejecutando `idf.py build flash monitor` directamente desde la terminal integrada antes de intentar reconfigurar la extensión.
