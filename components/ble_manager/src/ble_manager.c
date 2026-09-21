#include "ble_manager.h"
#include "config_manager.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "ble_manager";

static uint8_t             own_addr_type;
static uint16_t            self_test_val_handle;
static ble_self_test_cb_t  user_self_test_cb    = NULL;
static ble_epoch_sync_cb_t user_epoch_sync_cb   = NULL;
static bool                is_ble_connected     = false;
static bool                is_provisioned_event = false;

// Custom Service: 0xFF00
static const ble_uuid16_t gatt_svr_svc_uuid    = BLE_UUID16_INIT(0xFF00);
static const ble_uuid16_t gatt_mac_uuid        = BLE_UUID16_INIT(0xFF01);
static const ble_uuid16_t gatt_interval_uuid   = BLE_UUID16_INIT(0xFF02);
static const ble_uuid16_t gatt_frc_uuid        = BLE_UUID16_INIT(0xFF03);
static const ble_uuid16_t gatt_self_test_uuid  = BLE_UUID16_INIT(0xFF04);
static const ble_uuid16_t gatt_epoch_sync_uuid = BLE_UUID16_INIT(0xFF05);

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){{
                                                           .uuid      = &gatt_mac_uuid.u,
                                                           .access_cb = gatt_svr_chr_access,
                                                           .flags     = BLE_GATT_CHR_F_WRITE,
                                                       },
                                                       {
                                                           .uuid      = &gatt_interval_uuid.u,
                                                           .access_cb = gatt_svr_chr_access,
                                                           .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                                                       },
                                                       {
                                                           .uuid      = &gatt_frc_uuid.u,
                                                           .access_cb = gatt_svr_chr_access,
                                                           .flags     = BLE_GATT_CHR_F_WRITE,
                                                       },
                                                       {
                                                           .uuid       = &gatt_self_test_uuid.u,
                                                           .access_cb  = gatt_svr_chr_access,
                                                           .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                                           .val_handle = &self_test_val_handle,
                                                       },
                                                       {
                                                           .uuid      = &gatt_epoch_sync_uuid.u,
                                                           .access_cb = gatt_svr_chr_access,
                                                           .flags     = BLE_GATT_CHR_F_WRITE,
                                                       },
                                                       {
                                                           0, // No more characteristics in this service
                                                       }},
    },
    {
        0, // No more services
    },
};

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg) {
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &gatt_mac_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 6) {
                uint8_t mac[6];
                os_mbuf_copydata(ctxt->om, 0, 6, mac);

                nvs_node_config_t cfg = *config_manager_get();
                memcpy(cfg.gateway_mac, mac, 6);
                config_manager_save(&cfg);

                ESP_LOGI(TAG, "Gateway MAC updated via BLE");
                is_provisioned_event = true; // Trigger exit
                return 0;
            }
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
    } else if (ble_uuid_cmp(uuid, &gatt_interval_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 4) {
                uint32_t interval;
                os_mbuf_copydata(ctxt->om, 0, 4, &interval);

                nvs_node_config_t cfg       = *config_manager_get();
                cfg.monitoring_interval_sec = interval;
                config_manager_save(&cfg);

                ESP_LOGI(TAG, "Interval updated to %lu via BLE", (unsigned long) interval);
                return 0;
            }
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            nvs_node_config_t cfg = *config_manager_get();
            os_mbuf_append(ctxt->om, &cfg.monitoring_interval_sec, 4);
            return 0;
        }
    } else if (ble_uuid_cmp(uuid, &gatt_frc_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 2) {
                uint16_t ppm;
                os_mbuf_copydata(ctxt->om, 0, 2, &ppm);
                ESP_LOGI(TAG, "Requested FRC Trigger: %u ppm", ppm);
                // Currently mock, but could be routed to SCD41
                return 0;
            }
        }
    } else if (ble_uuid_cmp(uuid, &gatt_self_test_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 1) {
                uint8_t val;
                os_mbuf_copydata(ctxt->om, 0, 1, &val);
                if (val == 0x01 && user_self_test_cb) {
                    ESP_LOGI(TAG, "Self-Test triggered via BLE");
                    user_self_test_cb();
                }
                return 0;
            }
        }
    } else if (ble_uuid_cmp(uuid, &gatt_epoch_sync_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 8) {
                uint64_t epoch_s;
                os_mbuf_copydata(ctxt->om, 0, 8, &epoch_s);
                if (user_epoch_sync_cb) {
                    ESP_LOGI(TAG, "Epoch Sync triggered via BLE: %llu", (unsigned long long) epoch_s);
                    user_epoch_sync_cb(epoch_s);
                }
                return 0;
            }
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int bleprph_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "Connection %s", event->connect.status == 0 ? "established" : "failed");
            if (event->connect.status == 0) {
                is_ble_connected = true;
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
            is_ble_connected = false;
            // Resume advertising if we didn't provision
            if (!is_provisioned_event) {
                struct ble_gap_adv_params adv_params;
                memset(&adv_params, 0, sizeof(adv_params));
                adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
                adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
                ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprph_gap_event, NULL);
            }
            break;
    }
    return 0;
}

static void ble_app_on_sync(void) {
    int rc;
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE Advertising started");
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_manager_notify_self_test_result(uint32_t error_bitmask) {
    if (is_ble_connected) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&error_bitmask, sizeof(error_bitmask));
        int             rc = ble_gatts_notify_custom(
            1, self_test_val_handle,
            om); // assuming conn_handle is 1 for simplification, in real scenario we should track it
        if (rc == 0) {
            ESP_LOGI(TAG, "Notified Self-Test result: 0x%08X", (unsigned int) error_bitmask);
        } else {
            ESP_LOGE(TAG, "Failed to notify: %d", rc);
        }
    }
}

bool run_ble_provisioning_loop_blocking(uint32_t timeout_sec, ble_self_test_cb_t self_test_cb,
                                        ble_epoch_sync_cb_t epoch_sync_cb) {
    user_self_test_cb    = self_test_cb;
    user_epoch_sync_cb   = epoch_sync_cb;
    is_provisioned_event = false;
    is_ble_connected     = false;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble port: %d", err);
        return false;
    }

    ble_svc_gap_device_name_set("Room-Monitor");
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(ble_host_task);

    uint32_t elapsed_sec = 0;
    while (elapsed_sec < timeout_sec) {
        if (is_provisioned_event) {
            // Wait for disconnect or force it, but for simplicity, we just exit loop.
            vTaskDelay(pdMS_TO_TICKS(1000)); // give time for the response to be sent
            break;
        }

        if (is_ble_connected) {
            elapsed_sec = 0; // Reset timeout while connected
        } else {
            elapsed_sec++;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    bool success = is_provisioned_event;

    ESP_LOGI(TAG, "BLE Loop Exit. Success: %d", success);

    nimble_port_stop();
    nimble_port_deinit();

    return success;
}
