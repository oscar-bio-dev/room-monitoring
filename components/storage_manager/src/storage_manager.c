#include "storage_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include "pb_encode.h"
#include "pb_decode.h"
#include "driver/gpio.h"

static const char *TAG = "storage_manager";
#define MOUNT_POINT "/sdcard"
#define OFFLINE_FILE MOUNT_POINT "/offline.dat"
#define TMP_FILE MOUNT_POINT "/tmp.dat"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

static sdmmc_card_t *card = NULL;

static esp_err_t mount_sd(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, .max_files = 2, .allocation_unit_size = 16 * 1024};

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

esp_err_t storage_manager_save_offline(const EnvironmentalData *data) {
    if (mount_sd() != ESP_OK)
        return ESP_FAIL;

    uint8_t      buffer[128];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, EnvironmentalData_fields, data)) {
        ESP_LOGE(TAG, "Protobuf encoding failed: %s", PB_GET_ERROR(&stream));
        unmount_sd();
        return ESP_FAIL;
    }

    FILE *f = fopen(OFFLINE_FILE, "ab");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for appending");
        unmount_sd();
        return ESP_FAIL;
    }

    uint8_t size = stream.bytes_written;
    fwrite(&size, 1, 1, f);
    fwrite(buffer, 1, size, f);
    fclose(f);

    ESP_LOGI(TAG, "Saved %d bytes offline", size);
    unmount_sd();
    return ESP_OK;
}

esp_err_t storage_manager_get_offline_batch(EnvironmentalData *batch, size_t max_items, size_t *out_count) {
    *out_count = 0;
    if (mount_sd() != ESP_OK)
        return ESP_FAIL;

    FILE *f = fopen(OFFLINE_FILE, "rb");
    if (!f) {
        unmount_sd();
        return ESP_OK; // No file, no items
    }

    uint8_t size;
    uint8_t buffer[128];
    while (*out_count < max_items && fread(&size, 1, 1, f) == 1) {
        if (fread(buffer, 1, size, f) != size) {
            break; // Corrupted EOF
        }

        pb_istream_t stream = pb_istream_from_buffer(buffer, size);
        if (pb_decode(&stream, EnvironmentalData_fields, &batch[*out_count])) {
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

    FILE *f = fopen(OFFLINE_FILE, "rb");
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

    uint8_t size;
    uint8_t buffer[128];
    size_t  skipped = 0;

    // Skip items
    while (skipped < items_to_remove && fread(&size, 1, 1, f) == 1) {
        if (fread(buffer, 1, size, f) != size)
            break;
        skipped++;
    }

    // Copy remaining items
    while (fread(&size, 1, 1, f) == 1) {
        if (fread(buffer, 1, size, f) != size)
            break;
        fwrite(&size, 1, 1, ftmp);
        fwrite(buffer, 1, size, ftmp);
    }

    fclose(f);
    fclose(ftmp);

    // Replace original file
    remove(OFFLINE_FILE);
    rename(TMP_FILE, OFFLINE_FILE);

    // If file is empty, delete it
    f = fopen(OFFLINE_FILE, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fclose(f);
            remove(OFFLINE_FILE);
        } else {
            fclose(f);
        }
    }

    unmount_sd();
    return ESP_OK;
}
