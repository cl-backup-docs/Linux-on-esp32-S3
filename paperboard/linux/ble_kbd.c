// BLE keyboard front-end: a NimBLE central that finds a HOGP (HID over GATT)
// keyboard, pairs with it and feeds its reports into the same ESP_KBD_IF
// stream the USB keyboard uses (paperboard/linux/pb_kbd_bridge.c), so the
// Linux side -- kernel driver, ttyS0, /dev/pbkbd -- stays untouched.
//
// The keyboard's report map is parsed (hid_report.c) to locate the modifier
// byte and the key array inside its input report; devices without a usable
// map fall back to the 8-byte boot protocol when they offer it.
//
// The ESP32-S3 radio is BLE-only: keyboards that pair over classic Bluetooth
// cannot work, and 2.4 GHz dongle keyboards already show up as plain USB HID.
//
// Everything runs on the NimBLE host task -- the callouts, the GAP/GATT
// callbacks and the auto-repeat tick -- so the shared PbKeyboard state never
// needs a lock.
#include "sdkconfig.h"

#ifdef CONFIG_BT_ENABLED

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "hid_keymap.h"
#include "hid_report.h"
#include "pb_kbd_bridge.h"

// ble_prov.c calls this too; NimBLE keeps no public declaration for it.
void ble_store_config_init(void);

// HCI "remote user terminated connection": fine for a local hang-up.
#ifndef BLE_ERR_REM_USER_CONN_TERM
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#endif

static const char* TAG = "ble_kbd";

#define HID_SVC_UUID16 0x1812
#define CHR_PROTOCOL_MODE_UUID16 0x2A4E
#define CHR_REPORT_MAP_UUID16 0x2A4B
#define CHR_REPORT_UUID16 0x2A4D
#define CHR_BOOT_KBD_INPUT_UUID16 0x2A22
#define DSC_CCCD_UUID16 0x2902
#define DSC_REPORT_REFERENCE_UUID16 0x2908
#define APPEARANCE_HID_KEYBOARD 0x03C1
#define REPORT_TYPE_INPUT 0x01
#define PROTOCOL_MODE_BOOT 0x00
#define PROTOCOL_MODE_REPORT 0x01
#define CCCD_NOTIFY 0x0001
#define CCCD_INDICATE 0x0002
#define MAX_REPORT_CHRS 6
#define MAX_FAILED_PEERS 8
#define REPORT_MAP_MAX 512
#define REPORT_NOTIFY_MAX 32
#define CONNECT_TIMEOUT_MS 30000
#define SETUP_TIMEOUT_MS 45000
#define RETRY_DELAY_MS 500
#define TICK_PERIOD_MS 20
#define ADDR_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define ADDR_ARG(a) (a).val[5], (a).val[4], (a).val[3], (a).val[2], (a).val[1], (a).val[0]

enum {
    ST_IDLE, ST_SCAN, ST_CONNECT, ST_SECURE, ST_SVC, ST_CHRS, ST_DSCS,
    ST_REFS, ST_READ_MAP, ST_SUBSCRIBE, ST_READY
};

struct report_chr {
    uint16_t val_handle;
    uint16_t cccd_handle;
    uint16_t ref_handle;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t props;
};

static PbKeyboard kbd;
static uint8_t own_addr_type;
static bool addr_ready;
static uint8_t state = ST_IDLE;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int64_t setup_deadline_ms;
static struct ble_npl_callout drive_callout;
static struct ble_npl_callout tick_callout;

static uint16_t hid_start_handle, hid_end_handle;
static uint16_t chr_proto_mode, chr_report_map, chr_boot_input;
static uint8_t chr_proto_mode_props;
static struct report_chr reports[MAX_REPORT_CHRS];
static uint16_t boot_input_cccd;
static unsigned num_reports, ref_index;
static bool boot_mode;
static uint16_t notify_handle;
static PbHidKeyboardMap hid_map;
static uint8_t map_buf[REPORT_MAP_MAX];
static uint16_t map_len;

static ble_addr_t pending_peer;
static ble_addr_t failed_peers[MAX_FAILED_PEERS];
static unsigned num_failed;
static bool pair_try_passkey; // first attempt asks for a typed passkey
static uint8_t pair_attempt;
static bool scan_announced;

static void drive(void);
static int kbd_event(struct ble_gap_event* event, void* arg);
static void start_service_discovery(void);
static void read_next_ref(void);
static void read_report_map(void);
static void choose_report(void);
static void subscribe_report(void);
static void fail_peer(void);
static int chr_cb(uint16_t conn, const struct ble_gatt_error* error,
                  const struct ble_gatt_chr* chr, void* arg);
static int dsc_cb(uint16_t conn, const struct ble_gatt_error* error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc* dsc, void* arg);
static int cccd_cb(uint16_t conn, const struct ble_gatt_error* error,
                   struct ble_gatt_attr* attr, void* arg);

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void schedule(uint32_t ms) {
    ble_npl_callout_reset(&drive_callout, ble_npl_time_ms_to_ticks32(ms));
}

static void notice(const char* text) {
    ESP_LOGI(TAG, "%s", text);
    pb_kbd_notice(text);
}

static void emit(void* ctx, const uint8_t* bytes, size_t len) {
    (void)ctx;
    pb_kbd_send(bytes, len);
}

// A device that failed setup once (a mouse answering the HID service scan,
// for example) is skipped until a real keyboard works or the board reboots.
static bool peer_failed(const ble_addr_t* addr) {
    for (unsigned i = 0; i < num_failed; i++)
        if (failed_peers[i].type == addr->type && !memcmp(failed_peers[i].val, addr->val, 6))
            return true;
    return false;
}

static void peer_blacklist(const ble_addr_t* addr) {
    if (peer_failed(addr))
        return;
    if (num_failed == MAX_FAILED_PEERS) {
        memmove(&failed_peers[0], &failed_peers[1], sizeof(failed_peers[0]) * (MAX_FAILED_PEERS - 1));
        num_failed--;
    }
    failed_peers[num_failed++] = *addr;
}

static bool peer_is_bonded(const ble_addr_t* addr) {
    ble_addr_t peers[8];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, 8) != 0)
        return false;
    for (int i = 0; i < count; i++)
        if (peers[i].type == addr->type && !memcmp(peers[i].val, addr->val, 6))
            return true;
    return false;
}

// Provisioning (ble_prov.c) pairs Just Works; a keyboard prefers the passkey
// the user types. Without a bond the passkey is tried first because it is the
// authenticated path, and the retry falls back to Just Works.
static void set_pairing_mode(bool passkey_display) {
    ble_hs_cfg.sm_io_cap = passkey_display ? BLE_SM_IO_CAP_DISP_ONLY : BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_mitm = passkey_display ? 1 : 0;
}

static void set_pairing_defaults(void) {
    set_pairing_mode(false);
}

static void fail_peer(void) {
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        peer_blacklist(&pending_peer);
        set_pairing_defaults();
        state = ST_SECURE; // stays mid-setup so the disconnect path stays quiet
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        state = ST_IDLE;
        schedule(RETRY_DELAY_MS);
    }
}

static bool adv_is_keyboard(const struct ble_gap_disc_desc* disc) {
    struct ble_hs_adv_fields fields;
    if (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND)
        return false;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0)
        return false;
    if (fields.appearance_is_present && fields.appearance == APPEARANCE_HID_KEYBOARD)
        return true;
    for (unsigned i = 0; i < fields.num_uuids16; i++)
        if (ble_uuid_u16(&fields.uuids16[i].u) == HID_SVC_UUID16)
            return true;
    return false;
}

static void feed_report(const uint8_t* data, size_t len) {
    uint8_t boot[PB_HID_REPORT_LEN];
    if (boot_mode) {
        if (len >= PB_HID_REPORT_LEN)
            pb_kbd_report(&kbd, data, now_ms(), emit, NULL);
    } else if (pb_hid_map_apply(&hid_map, data, len, boot)) {
        pb_kbd_report(&kbd, boot, now_ms(), emit, NULL);
    }
}

static void start_scan(void) {
    struct ble_gap_disc_params params = {0};
    params.itvl = 0x0010; // 10 ms
    params.window = 0x0010;
    params.filter_duplicates = 1;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, kbd_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan failed to start: %d", rc);
        schedule(1000);
        return;
    }
    state = ST_SCAN;
    if (!scan_announced) {
        scan_announced = true;
        notice("\r\n[ble] scanning for keyboard...\r\n");
    }
}

static void start_connect(void) {
    struct ble_gap_conn_params params = {0};
    params.scan_itvl = 0x0010;
    params.scan_window = 0x0010;
    params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    params.latency = 0;
    params.supervision_timeout = 0x0100; // 2.56 s
    state = ST_CONNECT;
    int rc = ble_gap_connect(own_addr_type, &pending_peer, CONNECT_TIMEOUT_MS, &params, kbd_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect failed to start: %d", rc);
        peer_blacklist(&pending_peer);
        state = ST_IDLE;
        schedule(RETRY_DELAY_MS);
    }
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error* error,
                  const struct ble_gatt_svc* svc, void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (hid_start_handle) {
            state = ST_CHRS;
            if (ble_gattc_disc_all_chrs(conn_handle, hid_start_handle, hid_end_handle, chr_cb, NULL) != 0)
                fail_peer();
        } else {
            ESP_LOGW(TAG, "no HID service found");
            fail_peer();
        }
        return 0;
    }
    if (error->status != 0)
        return 0;
    if (!hid_start_handle) {
        hid_start_handle = svc->start_handle;
        hid_end_handle = svc->end_handle;
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error* error,
                  const struct ble_gatt_chr* chr, void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        state = ST_DSCS;
        if (ble_gattc_disc_all_dscs(conn_handle, hid_start_handle, hid_end_handle, dsc_cb, NULL) != 0)
            fail_peer();
        return 0;
    }
    if (error->status != 0)
        return 0;
    switch (ble_uuid_u16(&chr->uuid.u)) {
    case CHR_PROTOCOL_MODE_UUID16:
        chr_proto_mode = chr->val_handle;
        chr_proto_mode_props = chr->properties;
        break;
    case CHR_REPORT_MAP_UUID16:
        chr_report_map = chr->val_handle;
        break;
    case CHR_BOOT_KBD_INPUT_UUID16:
        chr_boot_input = chr->val_handle;
        break;
    case CHR_REPORT_UUID16:
        if (num_reports < MAX_REPORT_CHRS) {
            reports[num_reports].val_handle = chr->val_handle;
            reports[num_reports].props = chr->properties;
            num_reports++;
        }
        break;
    default:
        break;
    }
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error* error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc* dsc, void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        ref_index = 0;
        state = ST_REFS;
        read_next_ref();
        return 0;
    }
    if (error->status != 0)
        return 0;
    switch (ble_uuid_u16(&dsc->uuid.u)) {
    case DSC_CCCD_UUID16:
        for (unsigned i = 0; i < num_reports; i++)
            if (reports[i].val_handle == chr_val_handle)
                reports[i].cccd_handle = dsc->handle;
        if (chr_val_handle == chr_boot_input)
            boot_input_cccd = dsc->handle;
        break;
    case DSC_REPORT_REFERENCE_UUID16:
        for (unsigned i = 0; i < num_reports; i++)
            if (reports[i].val_handle == chr_val_handle)
                reports[i].ref_handle = dsc->handle;
        break;
    default:
        break;
    }
    return 0;
}

// Report Reference descriptors carry [report id, report type]; they tell the
// report characteristics apart (keyboard input, mouse, consumer control...).
static int ref_cb(uint16_t conn, const struct ble_gatt_error* error,
                  struct ble_gatt_attr* attr, void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == 0 && attr && attr->om && ref_index < num_reports) {
        uint8_t val[2];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(val))
            len = sizeof(val);
        if (len == sizeof(val) && ble_hs_mbuf_to_flat(attr->om, val, len, &len) == 0) {
            reports[ref_index].report_id = val[0];
            reports[ref_index].report_type = val[1];
        }
    }
    ref_index++;
    read_next_ref();
    return 0;
}

static void read_next_ref(void) {
    while (ref_index < num_reports && reports[ref_index].ref_handle == 0)
        ref_index++;
    if (ref_index >= num_reports) {
        read_report_map();
        return;
    }
    if (ble_gattc_read(conn_handle, reports[ref_index].ref_handle, ref_cb, NULL) != 0)
        fail_peer();
}

static int map_cb(uint16_t conn, const struct ble_gatt_error* error,
                  struct ble_gatt_attr* attr, void* arg) {
    (void)conn;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        choose_report();
        return 0;
    }
    if (error->status != 0 || !attr || !attr->om) {
        ESP_LOGW(TAG, "report map read: %d", error->status);
        choose_report();
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len && attr->offset + len <= sizeof(map_buf)) {
        uint16_t got = len;
        if (ble_hs_mbuf_to_flat(attr->om, map_buf + attr->offset, len, &got) == 0)
            map_len = (uint16_t)(attr->offset + got);
    }
    return 0;
}

static void read_report_map(void) {
    if (!chr_report_map) {
        choose_report();
        return;
    }
    map_len = 0;
    state = ST_READ_MAP;
    if (ble_gattc_read_long(conn_handle, chr_report_map, 0, map_cb, NULL) != 0)
        fail_peer();
}

static void subscribe_report(void) {
    uint16_t cccd = 0;
    uint16_t value = CCCD_NOTIFY;
    if (boot_mode) {
        cccd = boot_input_cccd;
    } else {
        for (unsigned i = 0; i < num_reports; i++) {
            if (reports[i].val_handle != notify_handle)
                continue;
            cccd = reports[i].cccd_handle;
            if (!(reports[i].props & BLE_GATT_CHR_PROP_NOTIFY) &&
                (reports[i].props & BLE_GATT_CHR_PROP_INDICATE))
                value = CCCD_INDICATE;
            break;
        }
    }
    if (!cccd) {
        ESP_LOGW(TAG, "no CCCD to subscribe to");
        fail_peer();
        return;
    }
    state = ST_SUBSCRIBE;
    if (ble_gattc_write_flat(conn_handle, cccd, &value, sizeof(value), cccd_cb, NULL) != 0)
        fail_peer();
}

static int cccd_cb(uint16_t conn, const struct ble_gatt_error* error,
                   struct ble_gatt_attr* attr, void* arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "report subscription failed: %d", error->status);
        fail_peer();
        return 0;
    }
    state = ST_READY;
    num_failed = 0;
    notice("\r\n[ble keyboard ready]\r\n");
    return 0;
}

static int proto_mode_cb(uint16_t conn, const struct ble_gatt_error* error,
                         struct ble_gatt_attr* attr, void* arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (error->status != 0)
        ESP_LOGW(TAG, "protocol mode write: %d", error->status);
    subscribe_report();
    return 0;
}

static void write_protocol_mode(uint8_t mode) {
    if (chr_proto_mode_props & BLE_GATT_CHR_PROP_WRITE) {
        if (ble_gattc_write_flat(conn_handle, chr_proto_mode, &mode, 1, proto_mode_cb, NULL) != 0)
            fail_peer();
    } else {
        // No response is not signalled, but ATT keeps the order, so the later
        // CCCD write still lands after this one.
        if (ble_gattc_write_no_rsp_flat(conn_handle, chr_proto_mode, &mode, 1) != 0)
            fail_peer();
        else
            subscribe_report();
    }
}

static void choose_report(void) {
    const struct report_chr* chosen = NULL;
    if (map_len && pb_hid_map_parse(map_buf, map_len, &hid_map) && hid_map.valid) {
        ESP_LOGI(TAG, "report map: id %u, %u bytes, mods@%u keys@%u x%u", hid_map.report_id,
                 hid_map.input_len, hid_map.mod_offset, hid_map.keys_offset, hid_map.keys_count);
        for (unsigned i = 0; i < num_reports; i++) {
            if (reports[i].report_type == REPORT_TYPE_INPUT && reports[i].cccd_handle &&
                reports[i].report_id == hid_map.report_id) {
                chosen = &reports[i];
                break;
            }
        }
    }
    bool proto_writable = (chr_proto_mode_props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) != 0;
    if (chosen) {
        boot_mode = false;
        notify_handle = chosen->val_handle;
        if (chr_proto_mode && proto_writable)
            write_protocol_mode(PROTOCOL_MODE_REPORT);
        else
            subscribe_report();
    } else if (chr_boot_input && boot_input_cccd && chr_proto_mode && proto_writable) {
        ESP_LOGW(TAG, "no usable report map; using boot protocol");
        boot_mode = true;
        notify_handle = chr_boot_input;
        write_protocol_mode(PROTOCOL_MODE_BOOT);
    } else {
        ESP_LOGW(TAG, "HID device has no usable keyboard report");
        fail_peer();
    }
}

static void start_service_discovery(void) {
    hid_start_handle = hid_end_handle = 0;
    chr_proto_mode = chr_report_map = chr_boot_input = 0;
    chr_proto_mode_props = 0;
    boot_input_cccd = 0;
    num_reports = ref_index = 0;
    boot_mode = false;
    notify_handle = 0;
    map_len = 0;
    state = ST_SVC;
    setup_deadline_ms = now_ms() + SETUP_TIMEOUT_MS;
    // Keep drive() alive through discovery: a device that stalls without ever
    // producing a callback must still hit the setup deadline.
    schedule(1000);
    if (ble_gattc_disc_svc_by_uuid(conn_handle, BLE_UUID16_DECLARE(HID_SVC_UUID16), svc_cb, NULL) != 0)
        fail_peer();
}

static void drive(void) {
    if (!ble_hs_synced()) {
        schedule(300);
        return;
    }
    if (!addr_ready) {
        if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
            ESP_LOGW(TAG, "waiting for a usable BLE address");
            schedule(1000);
            return;
        }
        addr_ready = true;
    }
    if (state == ST_IDLE) {
        start_scan();
        return;
    }
    if (state >= ST_SECURE && state <= ST_SUBSCRIBE) {
        if (now_ms() >= setup_deadline_ms) {
            ESP_LOGW(TAG, "keyboard setup timed out");
            fail_peer();
            return;
        }
        schedule(1000);
    }
}

static void drive_cb(struct ble_npl_event* ev) {
    (void)ev;
    drive();
}

static void tick_cb(struct ble_npl_event* ev) {
    (void)ev;
    pb_kbd_tick(&kbd, now_ms(), emit, NULL);
    ble_npl_callout_reset(&tick_callout, ble_npl_time_ms_to_ticks32(TICK_PERIOD_MS));
}

static int kbd_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (state != ST_SCAN || !adv_is_keyboard(&event->disc) || peer_failed(&event->disc.addr))
            break;
        pending_peer = event->disc.addr;
        ESP_LOGI(TAG, "trying " ADDR_FMT, ADDR_ARG(event->disc.addr));
        ble_gap_disc_cancel();
        start_connect();
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (state == ST_SCAN) {
            ESP_LOGI(TAG, "scan ended: %d", event->disc_complete.reason);
            schedule(RETRY_DELAY_MS);
        }
        break;

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            peer_blacklist(&pending_peer);
            state = ST_IDLE;
            schedule(RETRY_DELAY_MS);
            break;
        }
        conn_handle = event->connect.conn_handle;
        struct ble_gap_conn_desc desc;
        bool found = ble_gap_conn_find(conn_handle, &desc) == 0;
        if (found)
            ESP_LOGI(TAG, "connected to " ADDR_FMT " (handle %u)", ADDR_ARG(desc.peer_id_addr), conn_handle);
        else
            ESP_LOGI(TAG, "connected (handle %u)", conn_handle);
        if (found && desc.sec_state.encrypted) {
            set_pairing_defaults();
            start_service_discovery();
            break;
        }
        setup_deadline_ms = now_ms() + SETUP_TIMEOUT_MS;
        state = ST_SECURE;
        pair_attempt = 0;
        pair_try_passkey = !(found && peer_is_bonded(&desc.peer_id_addr));
        set_pairing_mode(pair_try_passkey);
        int rc = ble_gap_security_initiate(conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "security initiate failed: %d", rc);
            fail_peer();
        } else {
            schedule(1000);
        }
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle != conn_handle)
            break;
        if (event->enc_change.status == 0) {
            set_pairing_defaults();
            ESP_LOGI(TAG, "link encrypted");
            start_service_discovery();
            break;
        }
        ESP_LOGW(TAG, "encryption failed: %d", event->enc_change.status);
        if (pair_attempt == 0) {
            struct ble_gap_conn_desc desc;
            pair_attempt = 1;
            if (ble_gap_conn_find(conn_handle, &desc) == 0 && peer_is_bonded(&desc.peer_id_addr)) {
                ESP_LOGI(TAG, "dropping the stale bond");
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            pair_try_passkey = !pair_try_passkey;
            set_pairing_mode(pair_try_passkey);
            int rc = ble_gap_security_initiate(conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY)
                fail_peer();
            else
                schedule(1000);
        } else {
            fail_peer();
        }
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.conn_handle != conn_handle)
            break;
        struct ble_sm_io pkey = {0};
        pkey.action = event->passkey.params.action;
        char text[64];
        if (pkey.action == BLE_SM_IOACT_DISP) {
            pkey.passkey = 100000 + esp_random() % 900000;
            snprintf(text, sizeof(text), "\r\n[ble] passkey: %06" PRIu32 " (type it on the keyboard)\r\n",
                     pkey.passkey);
            notice(text);
        } else if (pkey.action == BLE_SM_IOACT_NUMCMP) {
            pkey.numcmp_accept = 1;
            snprintf(text, sizeof(text), "\r\n[ble] confirm passkey %06" PRIu32 " on the keyboard\r\n",
                     event->passkey.params.numcmp);
            notice(text);
        } else {
            ESP_LOGW(TAG, "unsupported passkey action %u", (unsigned)pkey.action);
        }
        if (ble_sm_inject_io(event->passkey.conn_handle, &pkey) != 0)
            ESP_LOGW(TAG, "ble_sm_inject_io failed");
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != conn_handle ||
            event->notify_rx.attr_handle != notify_handle || !event->notify_rx.om)
            break;
        uint8_t buf[REPORT_NOTIFY_MAX];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(buf))
            len = sizeof(buf);
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, &len) == 0)
            feed_report(buf, len);
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == conn_handle && event->subscribe.attr_handle == notify_handle)
            ESP_LOGI(TAG, "report subscription: notify=%d indicate=%d", event->subscribe.cur_notify,
                     event->subscribe.cur_indicate);
        break;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == conn_handle)
            ESP_LOGI(TAG, "ATT MTU: %u", event->mtu.value);
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnected: reason %d", event->disconnect.reason);
        bool was_ready = state == ST_READY;
        if (!was_ready && state != ST_IDLE && state != ST_SCAN)
            peer_blacklist(&pending_peer);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        notify_handle = 0;
        boot_mode = false;
        set_pairing_defaults();
        state = ST_IDLE;
        if (was_ready)
            notice("\r\n[ble] keyboard disconnected; rescanning\r\n");
        schedule(RETRY_DELAY_MS);
        break;
    }

    default:
        break;
    }
    return 0;
}

void ble_kbd_init(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    pb_kbd_init(&kbd);
    // Bonding (and the identity key) stays on for provisioning peers too.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    set_pairing_defaults();
    ble_npl_callout_init(&drive_callout, nimble_port_get_dflt_eventq(), drive_cb, NULL);
    ble_npl_callout_init(&tick_callout, nimble_port_get_dflt_eventq(), tick_cb, NULL);
    ble_npl_callout_reset(&tick_callout, ble_npl_time_ms_to_ticks32(TICK_PERIOD_MS));
    schedule(1000);
}

#endif // CONFIG_BT_ENABLED
