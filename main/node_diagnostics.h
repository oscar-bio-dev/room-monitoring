#ifndef NODE_DIAGNOSTICS_H
#define NODE_DIAGNOSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

#define ERR_NONE 0x00             // 0: Sistema Saludable
#define ERR_I2C_BUS (1 << 0)      // 0x01: Colapso general del bus I2C (SDA/SCL bloqueados)
#define ERR_BME688 (1 << 1)       // 0x02: Fallo BME (IAQ Accuracy 0, o I2C timeout)
#define ERR_SCD41 (1 << 2)        // 0x04: Fallo SCD (Self-Test fallido o I2C timeout)
#define ERR_BMV080 (1 << 3)       // 0x08: Fallo Láser ASIC (Error interno reportado)
#define ERR_RTC_RV1805 (1 << 4)   // 0x10: Fallo del reloj externo (desfase temporal crítico)
#define ERR_SD_CARD (1 << 5)      // 0x20: Fallo en SPI o montaje de la tarjeta MicroSD
#define ERR_ESPNOW_TX (1 << 6)    // 0x40: Falla crónica de transmisión (Gateway inalcanzable)
#define ERR_BMV080_DIRTY (1 << 7) // 0x80: Alerta pasiva de obstrucción del láser (Dirtiness)

#ifdef __cplusplus
}
#endif

#endif // NODE_DIAGNOSTICS_H
