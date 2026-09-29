#include "hid_keyboard.h"

#include <string.h>

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/hid/ble_svc_hid.h"
#include "store/config/ble_store_config.h"

/* Declared this way by the ESP-IDF NimBLE examples. */
void ble_store_config_init(void);

static const char *TAG = "hid_keyboard";
static const char DEVICE_NAME[] = "Click Bridge V1";

#define HID_APPEARANCE_KEYBOARD 0x03c1
#define HID_USAGE_I 0x0c
#define HID_USAGE_K 0x0e

/* ESP-IDF esp_hid_device keyboard report map. */
static const uint8_t KEYBOARD_REPORT_MAP[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xa1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /* Report ID (1) */
    0x05, 0x07,       /* Usage Page (Keyboard/Keypad) */
    0x19, 0xe0,       /* Usage Minimum (Left Control) */
    0x29, 0xe7,       /* Usage Maximum (Right GUI) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x08,       /* Report Count (8) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x08,       /* Report Size (8) */
    0x81, 0x03,       /* Input (Constant) */
    0x95, 0x05,       /* Report Count (5) */
    0x75, 0x01,       /* Report Size (1) */
    0x05, 0x08,       /* Usage Page (LEDs) */
    0x19, 0x01,       /* Usage Minimum (Num Lock) */
    0x29, 0x05,       /* Usage Maximum (Kana) */
    0x91, 0x02,       /* Output (Data, Variable, Absolute) */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x03,       /* Report Size (3) */
    0x91, 0x03,       /* Output (Constant) */
    0x95, 0x05,       /* Report Count (5) */
    0x75, 0x08,       /* Report Size (8) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x65,       /* Logical Maximum (101) */
    0x05, 0x07,       /* Usage Page (Keyboard/Keypad) */
    0x19, 0x00,       /* Usage Minimum (0) */
    0x29, 0x65,       /* Usage Maximum (101) */
    0x81, 0x00,       /* Input (Data, Array, Absolute) */
    0xc0,             /* End Collection */
};

static uint16_t hid_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t report_input_handle;
static uint16_t boot_input_handle;
static bool report_input_subscribed;
static bool boot_input_subscribed;
static bool advertising_enabled;
static unsigned int report_characteristic_index;
static struct ble_npl_callout advertising_retry;

static int hid_gap_event(struct ble_gap_event *event, void *arg);
static void hid_advertise(void);

static void hid_advertising_retry_cb(struct ble_npl_event *event)
{
    (void)event;
    hid_advertise();
}

static void hid_schedule_advertising(void)
{
    ble_npl_callout_reset(&advertising_retry,
                          ble_npl_time_ms_to_ticks32(200));
}

static bool hid_is_peripheral_connection(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    return ble_gap_conn_find(conn_handle, &desc) == 0 &&
           desc.role == BLE_GAP_ROLE_SLAVE;
}

static void hid_gatt_register_cb(struct ble_gatt_register_ctxt *ctxt,
                                 void *arg)
{
    (void)arg;
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR ||
        ble_uuid_u16(ctxt->chr.svc_def->uuid) != BLE_SVC_HID_UUID16) {
        return;
    }

    uint16_t uuid = ble_uuid_u16(ctxt->chr.chr_def->uuid);
    if (uuid == BLE_SVC_HID_CHR_UUID16_RPT) {
        /* Reports are registered in the order supplied below: input, output. */
        if (report_characteristic_index++ == 0) {
            report_input_handle = ctxt->chr.val_handle;
        }
    } else if (uuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP) {
        boot_input_handle = ctxt->chr.val_handle;
    }
}

static void hid_advertise(void)
{
    if (!advertising_enabled ||
        hid_conn_handle != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_adv_active()) {
        return;
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not determine advertising address; rc=%d", rc);
        return;
    }

    const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BLE_SVC_HID_UUID16);
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .appearance = HID_APPEARANCE_KEYBOARD,
        .appearance_is_present = 1,
        .name = (uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
        .uuids16 = (ble_uuid16_t *)&hid_uuid,
        .num_uuids16 = 1,
        .uuids16_is_complete = 1,
    };
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set HID advertising data; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(300),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(300),
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, hid_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start HID advertising; rc=%d", rc);
        hid_schedule_advertising();
    } else {
        ESP_LOGI(TAG, "HID advertising started as %s", DEVICE_NAME);
    }
}

static int hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "iPad HID connection failed; status=%d",
                     event->connect.status);
            hid_schedule_advertising();
            return 0;
        }
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0 ||
            desc.role != BLE_GAP_ROLE_SLAVE) {
            ESP_LOGW(TAG, "Ignoring non-peripheral HID connection event");
            return 0;
        }
        hid_conn_handle = event->connect.conn_handle;
        ble_npl_callout_stop(&advertising_retry);
        report_input_subscribed = false;
        boot_input_subscribed = false;
        ESP_LOGI(TAG, "iPad HID connected (peripheral role)");
        rc = ble_gap_security_initiate(hid_conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "Could not start HID security; rc=%d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle != hid_conn_handle ||
            event->disconnect.conn.role != BLE_GAP_ROLE_SLAVE) {
            return 0;
        }
        ESP_LOGW(TAG, "iPad HID disconnected; reason=%d",
                 event->disconnect.reason);
        hid_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        report_input_subscribed = false;
        boot_input_subscribed = false;
        hid_schedule_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (!hid_is_peripheral_connection(event->subscribe.conn_handle)) {
            return 0;
        }
        if (event->subscribe.attr_handle == report_input_handle) {
            report_input_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == boot_input_handle) {
            boot_input_subscribed = event->subscribe.cur_notify;
        } else {
            return 0;
        }
        ESP_LOGI(TAG, "HID keyboard notifications %s",
                 (report_input_subscribed || boot_input_subscribed) ?
                 "enabled" : "disabled");
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle == hid_conn_handle) {
            ESP_LOGI(TAG, "HID security %s; status=%d",
                     event->enc_change.status == 0 ? "ready" : "failed",
                     event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == hid_conn_handle &&
            event->notify_tx.status != 0) {
            ESP_LOGW(TAG, "HID notification failed; handle=%u status=%d",
                     event->notify_tx.attr_handle, event->notify_tx.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (!hid_is_peripheral_connection(event->repeat_pairing.conn_handle)) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        hid_schedule_advertising();
        return 0;

    default:
        return 0;
    }
}

int hid_keyboard_init(void)
{
    static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x03};
    struct ble_svc_hid_params params = {0};

    memcpy(&params.hid_info, hid_info, sizeof(hid_info));
    memcpy(params.report_map, KEYBOARD_REPORT_MAP,
           sizeof(KEYBOARD_REPORT_MAP));
    params.report_map_len = sizeof(KEYBOARD_REPORT_MAP);
    params.external_rpt_ref = 0x180f; /* Battery Service UUID, per HOGP. */
    params.proto_mode_present = 1;
    params.proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;
    params.kbd_inp_present = 1;
    params.kbd_out_present = 1;
    params.rpts[0].type = BLE_SVC_HID_RPT_TYPE_INPUT;
    params.rpts[0].id = 1;
    params.rpts[1].type = BLE_SVC_HID_RPT_TYPE_OUTPUT;
    params.rpts[1].id = 1;
    params.rpts_len = 2;

    ble_hs_cfg.gatts_register_cb = hid_gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    /* Click V1 sends unencrypted notifications. NimBLE's runtime level >= 2
     * silently drops those in ble_att_svr_rx_notify()/rx_indicate(). Keep
     * CONFIG_BT_NIMBLE_SM_LVL=2 so the HID service still requires encrypted
     * reads/writes, but allow mixed-security links at runtime. Level 0 leaves
     * security to each service; level 1 would reject iPad pairing requests.
     */
    ble_hs_cfg.sm_sec_lvl = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        return rc;
    }
    rc = ble_svc_gap_device_appearance_set(HID_APPEARANCE_KEYBOARD);
    if (rc != 0) {
        return rc;
    }
    rc = ble_svc_hid_add(params);
    if (rc != 0) {
        return rc;
    }
    ble_svc_hid_init();
    ble_store_config_init();
    ble_npl_callout_init(&advertising_retry,
                         nimble_port_get_dflt_eventq(),
                         hid_advertising_retry_cb, NULL);
    return 0;
}

void hid_keyboard_reset(void)
{
    ble_npl_callout_stop(&advertising_retry);
    hid_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    report_input_subscribed = false;
    boot_input_subscribed = false;
}

void hid_keyboard_start_advertising(void)
{
    advertising_enabled = true;
    hid_advertise();
}

void hid_keyboard_send_state(bool plus_pressed, bool minus_pressed)
{
    if (hid_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint16_t handle = report_input_subscribed ? report_input_handle :
                      boot_input_subscribed ? boot_input_handle : 0;
    if (handle == 0) {
        ESP_LOGW(TAG, "HID report skipped: iPad has not subscribed yet");
        return;
    }

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(hid_conn_handle, &desc) != 0 ||
        !desc.sec_state.encrypted) {
        ESP_LOGW(TAG, "HID report skipped: iPad link is not encrypted yet");
        return;
    }

    uint8_t report[8] = {0};
    size_t key_index = 2;
    if (plus_pressed) {
        report[key_index++] = HID_USAGE_K;
    }
    if (minus_pressed) {
        report[key_index] = HID_USAGE_I;
    }

    /* Report Map: 1 modifier + 1 reserved + 5 key bytes. Boot mode uses 8.
     * The Report ID is carried by the Report Reference descriptor, not data.
     */
    size_t report_len = report_input_subscribed ? 7 : sizeof(report);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, report_len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Could not allocate HID report");
        return;
    }
    int rc = ble_gatts_notify_custom(hid_conn_handle, handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Could not send HID report; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "HID queued: K=%d I=%d; len=%u",
                 plus_pressed, minus_pressed, (unsigned int)report_len);
    }
}
