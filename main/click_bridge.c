#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "click_bridge";
static const char CLICK_NAME[] = "Zwift Click";
static const uint8_t RIDE_ON[] = {'R', 'i', 'd', 'e', 'O', 'n'};

#define INVALID_HANDLE 0
#define MAX_CLICK_CHARACTERISTICS 10

/* NimBLE stores 128-bit UUID bytes least-significant byte first. */
static const ble_uuid128_t ZWIFT_SERVICE_UUID = BLE_UUID128_INIT(
    0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86,
    0x51, 0x46, 0xca, 0x19, 0x01, 0x00, 0x00, 0x00);
static const ble_uuid128_t ZWIFT_ASYNC_UUID = BLE_UUID128_INIT(
    0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86,
    0x51, 0x46, 0xca, 0x19, 0x02, 0x00, 0x00, 0x00);
static const ble_uuid128_t ZWIFT_SYNC_RX_UUID = BLE_UUID128_INIT(
    0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86,
    0x51, 0x46, 0xca, 0x19, 0x03, 0x00, 0x00, 0x00);
static const ble_uuid128_t ZWIFT_SYNC_TX_UUID = BLE_UUID128_INIT(
    0xd1, 0x09, 0xdd, 0xdc, 0x29, 0xfa, 0xe5, 0x86,
    0x51, 0x46, 0xca, 0x19, 0x04, 0x00, 0x00, 0x00);
struct click_chr {
    uint16_t def_handle;
    uint16_t val_handle;
};

enum descriptor_target {
    DSC_TARGET_ASYNC,
    DSC_TARGET_SYNC_TX,
};

enum subscribe_stage {
    SUBSCRIBE_ASYNC,
    SUBSCRIBE_SYNC_TX,
};

static uint16_t click_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t click_svc_start;
static uint16_t click_svc_end;
static uint16_t async_val_handle;
static uint16_t async_cccd_handle;
static uint16_t sync_rx_val_handle;
static uint16_t sync_tx_val_handle;
static uint16_t sync_tx_cccd_handle;
static uint8_t async_properties;
static uint8_t sync_tx_properties;
static struct click_chr click_chrs[MAX_CLICK_CHARACTERISTICS];
static size_t click_chr_count;
static enum descriptor_target descriptor_target;
static bool connecting_to_click;
static int8_t plus_pressed = -1;
static int8_t minus_pressed = -1;

static int click_gap_event(struct ble_gap_event *event, void *arg);
static void click_scan(void);
static void subscribe_and_handshake(void);

static void reset_click_state(void)
{
    click_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    click_svc_start = INVALID_HANDLE;
    click_svc_end = INVALID_HANDLE;
    async_val_handle = INVALID_HANDLE;
    async_cccd_handle = INVALID_HANDLE;
    sync_rx_val_handle = INVALID_HANDLE;
    sync_tx_val_handle = INVALID_HANDLE;
    sync_tx_cccd_handle = INVALID_HANDLE;
    async_properties = 0;
    sync_tx_properties = 0;
    click_chr_count = 0;
    plus_pressed = -1;
    minus_pressed = -1;
}

static bool advertisement_is_click(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0 || fields.name == NULL) {
        return false;
    }
    return fields.name_len == strlen(CLICK_NAME) &&
           memcmp(fields.name, CLICK_NAME, fields.name_len) == 0;
}

static uint16_t characteristic_end_handle(uint16_t val_handle)
{
    uint16_t end_handle = click_svc_end;
    for (size_t i = 0; i < click_chr_count; ++i) {
        if (click_chrs[i].def_handle > val_handle &&
            click_chrs[i].def_handle - 1 < end_handle) {
            end_handle = click_chrs[i].def_handle - 1;
        }
    }
    return end_handle;
}

static void disconnect_click_after_error(const char *message, int rc)
{
    ESP_LOGE(TAG, "%s; rc=%d", message, rc);
    if (click_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(click_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static uint16_t cccd_value_for_properties(uint8_t properties)
{
    if (properties & BLE_GATT_CHR_PROP_NOTIFY) {
        return 1;
    }
    if (properties & BLE_GATT_CHR_PROP_INDICATE) {
        return 2;
    }
    return 0;
}

static bool update_button_state(const char *name, int8_t *old_state,
                                int8_t new_state)
{
    if (*old_state == -1) {
        *old_state = new_state;
        if (!new_state) {
            return false;
        }
    } else if (*old_state == new_state) {
        return false;
    } else {
        *old_state = new_state;
    }
    ESP_LOGI(TAG, "%s %s", name, new_state ? "PRESSED" : "RELEASED");
    return true;
}

static bool read_varint(const uint8_t *data, size_t length,
                        size_t *offset, uint64_t *value)
{
    uint64_t result = 0;
    unsigned int shift = 0;
    while (*offset < length && shift < 64) {
        uint8_t byte = data[(*offset)++];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static void parse_click_notification(const uint8_t *data, size_t length)
{
    if (length < 2 || data[0] != 0x37) {
        return;
    }
    size_t offset = 1;
    int plus_value = -1;
    int minus_value = -1;
    while (offset < length) {
        uint64_t key;
        uint64_t value;
        if (!read_varint(data, length, &offset, &key)) {
            break;
        }
        if ((key & 0x07) != 0 ||
            !read_varint(data, length, &offset, &value)) {
            break;
        }
        if ((key >> 3) == 1) {
            plus_value = (int)value;
        } else if ((key >> 3) == 2) {
            minus_value = (int)value;
        }
    }

    if (plus_value >= 0) {
        update_button_state("PLUS", &plus_pressed, plus_value == 0);
    }
    if (minus_value >= 0) {
        update_button_state("MINUS", &minus_pressed, minus_value == 0);
    }
}

static void handle_click_notification(struct ble_gap_event *event)
{
    uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
    uint8_t data[32];
    if (length <= sizeof(data) &&
        os_mbuf_copydata(event->notify_rx.om, 0, length, data) == 0) {
        if (event->notify_rx.attr_handle == async_val_handle) {
            parse_click_notification(data, length);
        }
    }
}

static void send_ride_on(uint16_t conn_handle)
{
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, sync_rx_val_handle,
                                         RIDE_ON, sizeof(RIDE_ON));
    if (rc != 0) {
        disconnect_click_after_error("Could not send RideOn handshake", rc);
    } else {
        ESP_LOGI(TAG, "Ready; press + or - on Click");
    }
}

static int subscribe_write_complete(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    struct ble_gatt_attr *attr,
                                    void *arg)
{
    (void)attr;
    enum subscribe_stage stage = (enum subscribe_stage)(intptr_t)arg;
    if (error->status != 0) {
        disconnect_click_after_error("Click subscription failed",
                                     error->status);
        return 0;
    }

    if (stage == SUBSCRIBE_ASYNC) {
        uint16_t cccd_value = cccd_value_for_properties(sync_tx_properties);
        int rc = ble_gattc_write_flat(
            conn_handle, sync_tx_cccd_handle,
            &cccd_value, sizeof(cccd_value), subscribe_write_complete,
            (void *)(intptr_t)SUBSCRIBE_SYNC_TX);
        if (rc != 0) {
            disconnect_click_after_error("Could not subscribe to Click handshake response",
                                         rc);
        }
        return 0;
    }

    send_ride_on(conn_handle);
    return 0;
}

static void subscribe_and_handshake(void)
{
    if (async_cccd_handle == INVALID_HANDLE ||
        sync_tx_cccd_handle == INVALID_HANDLE ||
        sync_rx_val_handle == INVALID_HANDLE) {
        disconnect_click_after_error("Click is missing a required handle",
                                     BLE_HS_ENOENT);
        return;
    }
    uint16_t cccd_value = cccd_value_for_properties(async_properties);
    int rc = ble_gattc_write_flat(
        click_conn_handle, async_cccd_handle,
        &cccd_value, sizeof(cccd_value), subscribe_write_complete,
        (void *)(intptr_t)SUBSCRIBE_ASYNC);
    if (rc != 0) {
        disconnect_click_after_error("Could not subscribe to Click buttons", rc);
    }
}

static int descriptor_discovery_cb(uint16_t conn_handle,
                                   const struct ble_gatt_error *error,
                                   uint16_t chr_val_handle,
                                   const struct ble_gatt_dsc *dsc,
                                   void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u,
                         BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            if (descriptor_target == DSC_TARGET_ASYNC) {
                async_cccd_handle = dsc->handle;
            } else {
                sync_tx_cccd_handle = dsc->handle;
            }
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        disconnect_click_after_error("Click descriptor discovery failed",
                                     error->status);
        return 0;
    }

    if (descriptor_target == DSC_TARGET_ASYNC) {
        if (async_cccd_handle == INVALID_HANDLE) {
            disconnect_click_after_error("Click async CCCD not found",
                                         BLE_HS_ENOENT);
            return 0;
        }
        descriptor_target = DSC_TARGET_SYNC_TX;
        int rc = ble_gattc_disc_all_dscs(
            click_conn_handle, sync_tx_val_handle,
            characteristic_end_handle(sync_tx_val_handle),
            descriptor_discovery_cb, NULL);
        if (rc != 0) {
            disconnect_click_after_error("Could not discover sync TX CCCD", rc);
        }
    } else {
        if (sync_tx_cccd_handle == INVALID_HANDLE) {
            disconnect_click_after_error("Click sync TX CCCD not found",
                                         BLE_HS_ENOENT);
            return 0;
        }
        subscribe_and_handshake();
    }
    return 0;
}

static int characteristic_discovery_cb(uint16_t conn_handle,
                                       const struct ble_gatt_error *error,
                                       const struct ble_gatt_chr *chr,
                                       void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0) {
        if (click_chr_count < MAX_CLICK_CHARACTERISTICS) {
            click_chrs[click_chr_count++] = (struct click_chr) {
                .def_handle = chr->def_handle,
                .val_handle = chr->val_handle,
            };
        }
        if (ble_uuid_cmp(&chr->uuid.u, &ZWIFT_ASYNC_UUID.u) == 0) {
            async_val_handle = chr->val_handle;
            async_properties = chr->properties;
        } else if (ble_uuid_cmp(&chr->uuid.u, &ZWIFT_SYNC_RX_UUID.u) == 0) {
            sync_rx_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &ZWIFT_SYNC_TX_UUID.u) == 0) {
            sync_tx_val_handle = chr->val_handle;
            sync_tx_properties = chr->properties;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        disconnect_click_after_error("Click characteristic discovery failed",
                                     error->status);
        return 0;
    }
    if (async_val_handle == INVALID_HANDLE ||
        sync_rx_val_handle == INVALID_HANDLE ||
        sync_tx_val_handle == INVALID_HANDLE ||
        cccd_value_for_properties(async_properties) == 0 ||
        cccd_value_for_properties(sync_tx_properties) == 0) {
        disconnect_click_after_error("Click characteristics are incomplete",
                                     BLE_HS_ENOENT);
        return 0;
    }

    descriptor_target = DSC_TARGET_ASYNC;
    int rc = ble_gattc_disc_all_dscs(
        click_conn_handle, async_val_handle,
        characteristic_end_handle(async_val_handle),
        descriptor_discovery_cb, NULL);
    if (rc != 0) {
        disconnect_click_after_error("Could not discover Click button CCCD", rc);
    }
    return 0;
}

static int service_discovery_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *service,
                                void *arg)
{
    (void)arg;
    if (error->status == 0) {
        click_svc_start = service->start_handle;
        click_svc_end = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        disconnect_click_after_error("Click service discovery failed",
                                     error->status);
        return 0;
    }
    if (click_svc_start == INVALID_HANDLE) {
        disconnect_click_after_error("Zwift service not found", BLE_HS_ENOENT);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn_handle, click_svc_start,
                                     click_svc_end,
                                     characteristic_discovery_cb, NULL);
    if (rc != 0) {
        disconnect_click_after_error("Could not discover Click characteristics",
                                     rc);
    }
    return 0;
}

static void connect_to_click(const ble_addr_t *addr)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        connecting_to_click = false;
        click_scan();
        return;
    }
    rc = ble_gap_connect(own_addr_type, addr, 30000, NULL,
                         click_gap_event, NULL);
    if (rc != 0) {
        connecting_to_click = false;
        ESP_LOGE(TAG, "Could not connect to Click; rc=%d", rc);
        click_scan();
    }
}

static int click_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!connecting_to_click && advertisement_is_click(&event->disc)) {
            ble_addr_t addr = event->disc.addr;
            connecting_to_click = true;
            ble_gap_disc_cancel();
            connect_to_click(&addr);
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        connecting_to_click = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Click connection failed; status=%d",
                     event->connect.status);
            click_scan();
            return 0;
        }
        reset_click_state();
        click_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "Click connected");
        if (ble_gattc_disc_svc_by_uuid(
                click_conn_handle, &ZWIFT_SERVICE_UUID.u,
                service_discovery_cb, NULL) != 0) {
            disconnect_click_after_error("Could not discover Zwift service",
                                         BLE_HS_EUNKNOWN);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Click disconnected; reason=%d",
                 event->disconnect.reason);
        reset_click_state();
        click_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_click_notification(event);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!connecting_to_click &&
            click_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            click_scan();
        }
        return 0;
    default:
        return 0;
    }
}

static void click_scan(void)
{
    if (click_conn_handle != BLE_HS_CONN_HANDLE_NONE ||
        connecting_to_click || ble_gap_disc_active()) {
        return;
    }
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not determine scan address type; rc=%d", rc);
        return;
    }
    /* Active scan at 10% duty cycle; scanning stops after Click connects. */
    struct ble_gap_disc_params params = {
        .itvl = 160,
        .window = 16,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    ESP_LOGI(TAG, "Scanning for Click (wake it with a button press)");
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params,
                      click_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start Click scan; rc=%d", rc);
    }
}

static void host_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
    connecting_to_click = false;
    reset_click_state();
}

static void host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not create BLE identity address; rc=%d", rc);
        return;
    }
    click_scan();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    reset_click_state();
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    nimble_port_freertos_init(host_task);
}
