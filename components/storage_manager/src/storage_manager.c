#include "storage_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include <unistd.h>
#include "pb_encode.h"
#include "pb_decode.h"
#include "driver/gpio.h"
#include "esp_rom_crc.h"

static const char *TAG = "storage_manager";
#define MOUNT_POINT "/sdcard"
#define OFFLINE_FILE MOUNT_POINT "/offline.dat"
#define TMP_FILE MOUNT_POINT "/tmp.dat"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define STORAGE_MAGIC 0x4242

static sdmmc_card_t *card = NULL;

static esp_err_t mount_sd(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024};

    sdmmc_host_t     host    = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = PIN_NUM_MISO,
        .sclk_io_num     = PIN_NUM_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SPI bus");
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = PIN_NUM_CS;
    slot_config.host_id               = host.slot;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card");
        spi_bus_free(host.slot);
        return ret;
    }

    // Crash recovery
    FILE *f_old = fopen(MOUNT_POINT "/offline_old.dat", "rb");
    if (f_old) {
        fclose(f_old);
        FILE *f_off = fopen(OFFLINE_FILE, "rb");
        if (!f_off) {
            FILE *f_tmp = fopen(TMP_FILE, "rb");
            if (f_tmp) {
                fclose(f_tmp);
                rename(TMP_FILE, OFFLINE_FILE);
            } else {
                rename(MOUNT_POINT "/offline_old.dat", OFFLINE_FILE);
            }
        } else {
            fclose(f_off);
        }
        remove(MOUNT_POINT "/offline_old.dat");
    }
    remove(TMP_FILE);

    return ESP_OK;
}

static void unmount_sd(void) {
    if (card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        card = NULL;
        spi_bus_free(SDSPI_DEFAULT_HOST);

        // Return pins to safe low-power state
        gpio_reset_pin(PIN_NUM_MISO);
        gpio_reset_pin(PIN_NUM_MOSI);
        gpio_reset_pin(PIN_NUM_CLK);
        gpio_reset_pin(PIN_NUM_CS);
    }
}

esp_err_t storage_manager_save_offline(const telemetry_TelemetryPayload *data) {
    if (mount_sd() != ESP_OK)
        return ESP_FAIL;

    uint8_t      buffer[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, telemetry_TelemetryPayload_fields, data)) {
        ESP_LOGE(TAG, "Protobuf encoding failed: %s", PB_GET_ERROR(&stream));
        unmount_sd();
        return ESP_FAIL;
    }

#define OFFLINE_BAK MOUNT_POINT "/offline_bak.dat"

    FILE *f = fopen(OFFLINE_FILE, "ab");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for appending");
        unmount_sd();
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size >= 256 * 1024) {
        ESP_LOGW(TAG, "Offline file exceeds 256KB, rotating to backup...");
        fclose(f);
        remove(OFFLINE_BAK);
        rename(OFFLINE_FILE, OFFLINE_BAK);
        f = fopen(OFFLINE_FILE, "ab");
        if (!f) {
            ESP_LOGE(TAG, "Failed to create new offline file after rotation");
            unmount_sd();
            return ESP_FAIL;
        }
    }

    uint16_t magic = STORAGE_MAGIC;
    uint16_t size  = stream.bytes_written;
    uint32_t crc   = esp_rom_crc32_le(0, (const uint8_t *) &magic, sizeof(magic));
    crc            = esp_rom_crc32_le(crc, (const uint8_t *) &size, sizeof(size));
    crc            = esp_rom_crc32_le(crc, buffer, size);

    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&size, sizeof(size), 1, f);
    fwrite(buffer, 1, size, f);
    fwrite(&crc, sizeof(crc), 1, f);

    // Volcado Físico Inmediato para prevenir corrupción antes del Light-Sleep
    fflush(f);
    fsync(fileno(f));

    fclose(f);

    ESP_LOGI(TAG, "Saved %d bytes offline (CRC: 0x%08X)", size, (unsigned int) crc);
    unmount_sd();
    return ESP_OK;
}

// Helper para leer registros con validación Magic + CRC32
static bool read_next_record(FILE *f, uint8_t *payload_buffer, uint16_t max_size, uint16_t *out_size) {
    uint8_t  byte;
    uint16_t magic_scan = 0;

    // Byte-by-byte scan for Magic Word (0x4242)
    while (fread(&byte, 1, 1, f) == 1) {
        magic_scan = (magic_scan >> 8) | (byte << 8); // Little Endian scan
        if (magic_scan == STORAGE_MAGIC) {
            uint16_t size;
            if (fread(&size, sizeof(size), 1, f) != 1)
                continue;

            if (size > max_size) {
                ESP_LOGW(TAG, "Payload size %d exceeds buffer, skipping", size);
                continue;
            }

            if (fread(payload_buffer, 1, size, f) != size)
                continue;

            uint32_t read_crc;
            if (fread(&read_crc, sizeof(read_crc), 1, f) != 1)
                continue;

            uint16_t magic_ref = STORAGE_MAGIC;
            uint32_t calc_crc  = esp_rom_crc32_le(0, (const uint8_t *) &magic_ref, sizeof(magic_ref));
            calc_crc           = esp_rom_crc32_le(calc_crc, (const uint8_t *) &size, sizeof(size));
            calc_crc           = esp_rom_crc32_le(calc_crc, payload_buffer, size);

            if (read_crc == calc_crc) {
                *out_size = size;
                return true; // Record is valid
            } else {
                ESP_LOGW(TAG, "CRC mismatch (read 0x%08X, calc 0x%08X), skipping", (unsigned int) read_crc,
                         (unsigned int) calc_crc);
            }
        }
    }
    return false; // EOF or no valid record found
}

esp_err_t storage_manager_get_offline_batch(telemetry_TelemetryPayload *batch, size_t max_items, size_t *out_count) {
    *out_count = 0;
    if (mount_sd() != ESP_OK)
        return ESP_FAIL;

    FILE *f = fopen(MOUNT_POINT "/offline_bak.dat", "rb");
    if (!f) {
        f = fopen(OFFLINE_FILE, "rb");
    }
    if (!f) {
        unmount_sd();
        return ESP_OK; // No file, no items
    }

    uint16_t size;
    uint8_t  buffer[256];

    while (*out_count < max_items && read_next_record(f, buffer, sizeof(buffer), &size)) {
        pb_istream_t stream = pb_istream_from_buffer(buffer, size);
        if (pb_decode(&stream, telemetry_TelemetryPayload_fields, &batch[*out_count])) {
            (*out_count)++;
        }
    }

    fclose(f);
    unmount_sd();
    return ESP_OK;
}

esp_err_t storage_manager_clear_offline_batch(size_t items_to_remove) {
    if (items_to_remove == 0)
        return ESP_OK;
    if (mount_sd() != ESP_OK)
        return ESP_FAIL;

    const char *target_file = OFFLINE_FILE;
    FILE       *f_bak       = fopen(MOUNT_POINT "/offline_bak.dat", "rb");
    if (f_bak) {
        target_file = MOUNT_POINT "/offline_bak.dat";
        fclose(f_bak);
    }

    FILE *f = fopen(target_file, "rb");
    if (!f) {
        unmount_sd();
        return ESP_OK;
    }

    FILE *ftmp = fopen(TMP_FILE, "wb");
    if (!ftmp) {
        fclose(f);
        unmount_sd();
        return ESP_FAIL;
    }

    uint16_t size;
    uint8_t  buffer[256];
    size_t   skipped = 0;

    // Parse and either skip or copy records
    while (read_next_record(f, buffer, sizeof(buffer), &size)) {
        if (skipped < items_to_remove) {
            skipped++; // Skip this valid record
        } else {
            // Copy remaining valid records
            uint16_t magic    = STORAGE_MAGIC;
            uint32_t calc_crc = esp_rom_crc32_le(0, (const uint8_t *) &magic, sizeof(magic));
            calc_crc          = esp_rom_crc32_le(calc_crc, (const uint8_t *) &size, sizeof(size));
            calc_crc          = esp_rom_crc32_le(calc_crc, buffer, size);

            fwrite(&magic, sizeof(magic), 1, ftmp);
            fwrite(&size, sizeof(size), 1, ftmp);
            fwrite(buffer, 1, size, ftmp);
            fwrite(&calc_crc, sizeof(calc_crc), 1, ftmp);
        }
    }

    // Force flush the tmp file
    fflush(ftmp);
    fsync(fileno(ftmp));

    fclose(f);
    fclose(ftmp);

    // Replace original file (Crash-safe)
    rename(target_file, MOUNT_POINT "/offline_old.dat");
    rename(TMP_FILE, target_file);
    remove(MOUNT_POINT "/offline_old.dat");

    // If file is empty, delete it
    f = fopen(target_file, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fclose(f);
            remove(target_file);
        } else {
            fclose(f);
        }
    }

    unmount_sd();
    return ESP_OK;
}
