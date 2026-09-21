#include "rv1805_wrapper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "RV1805";

#define UNIX_OFFSET_2000 946684800ULL // Segundos entre 1970-01-01 y 2000-01-01

static uint8_t bcd_to_dec(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

// Simple algorithm to convert date/time to seconds since 2000-01-01
static uint32_t datetime_to_seconds(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                    uint8_t second) {
    // Days per month (non-leap year)
    static const uint16_t days_per_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    // Add years
    uint32_t days = year * 365;
    // Add leap days from previous years
    days += (year + 3) / 4;

    // Add days of current year (up to current month)
    days += days_per_month[month - 1];

    // Leap year check for current year
    if ((month > 2) && (year % 4 == 0)) {
        days++;
    }

    // Add current days (1-based, so subtract 1)
    days += (day - 1);

    uint32_t total_seconds = (days * 86400UL) + (hour * 3600UL) + (minute * 60UL) + second;
    return total_seconds;
}

esp_err_t rv1805_wrapper_init(i2c_master_bus_handle_t bus_handle, i2c_master_dev_handle_t *rv_dev_handle) {
    if (!bus_handle || !rv_dev_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t rv1805_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RV1805_I2C_ADDR, .scl_speed_hz = 100000};

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &rv1805_cfg, rv_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add RV1805 to I2C bus");
        return err;
    }

    ESP_LOGI(TAG, "RV-1805 I2C device initialized successfully");
    return ESP_OK;
}

bool rv1805_is_time_valid(i2c_master_dev_handle_t rv_dev_handle) {
    if (!rv_dev_handle)
        return false;

    uint8_t   reg      = 0x06; // Year register
    uint8_t   year_bcd = 0;
    esp_err_t err      = i2c_master_transmit_receive(rv_dev_handle, &reg, 1, &year_bcd, 1, pdMS_TO_TICKS(100));

    if (err != ESP_OK)
        return false;

    uint8_t year = bcd_to_dec(year_bcd);
    return (year >= 24); // >= 2024
}

esp_err_t rv1805_sync_from_epoch(i2c_master_dev_handle_t rv_dev_handle, uint64_t epoch_s) {
    if (!rv_dev_handle)
        return ESP_ERR_INVALID_ARG;

    struct tm timeinfo;
    time_t    t = (time_t) epoch_s;
    gmtime_r(&t, &timeinfo);

    uint8_t year = timeinfo.tm_year >= 100 ? timeinfo.tm_year - 100 : 0; // 00-99

    uint8_t data[8];
    data[0] = 0x00; // Start register (Hundredths)
    data[1] = 0;    // Hundredths
    data[2] = dec_to_bcd(timeinfo.tm_sec);
    data[3] = dec_to_bcd(timeinfo.tm_min);
    data[4] = dec_to_bcd(timeinfo.tm_hour);
    data[5] = dec_to_bcd(timeinfo.tm_mday);
    data[6] = dec_to_bcd(timeinfo.tm_mon + 1); // 1-12
    data[7] = dec_to_bcd(year);

    esp_err_t err = i2c_master_transmit(rv_dev_handle, data, 8, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC RV-1805 Synced to UTC: %04d-%02d-%02d %02d:%02d:%02d", year + 2000, timeinfo.tm_mon + 1,
                 timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
    }
    return err;
}

esp_err_t rv1805_get_time_ns(i2c_master_dev_handle_t rv_dev_handle, int64_t *time_ns) {
    if (!rv_dev_handle || !time_ns) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg     = 0x00; // Start at Hundredths register
    uint8_t data[7] = {0};

    // Read registers 0x00 to 0x06
    esp_err_t err = i2c_master_transmit_receive(rv_dev_handle, &reg, 1, data, 7, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RV1805 time registers");
        return err;
    }

    uint8_t hund  = bcd_to_dec(data[0]);
    uint8_t sec   = bcd_to_dec(data[1] & 0x7F);
    uint8_t min   = bcd_to_dec(data[2] & 0x7F);
    uint8_t hour  = bcd_to_dec(data[3] & 0x3F); // 24-hour mode mask
    uint8_t day   = bcd_to_dec(data[4] & 0x3F);
    uint8_t month = bcd_to_dec(data[5] & 0x1F);
    uint8_t year  = bcd_to_dec(data[6]);

    // Ensure safe ranges in case of uninitialized RTC memory
    if (month == 0)
        month = 1;
    if (day == 0)
        day = 1;

    uint32_t total_sec = datetime_to_seconds(year, month, day, hour, min, sec);

    // Convert to UNIX epoch if year >= 24
    if (year >= 24) {
        *time_ns = ((int64_t) total_sec + UNIX_OFFSET_2000) * 1000000000LL + (int64_t) hund * 10000000LL;
    } else {
        // Fallback for monotonic BSEC behavior if unsynced
        *time_ns = (int64_t) total_sec * 1000000000LL + (int64_t) hund * 10000000LL;
    }

    return ESP_OK;
}
