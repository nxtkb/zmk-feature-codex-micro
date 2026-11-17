/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

#include <nxtkb/codex/protocol.h>
#include <nxtkb/codex/transport.h>

LOG_MODULE_REGISTER(nxtkb_codex_ble, CONFIG_NXTKB_CODEX_LOG_LEVEL);

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
    HIDS_INPUT = 0x01,
    HIDS_OUTPUT = 0x02,
};

struct codex_hids_info {
    uint16_t version;
    uint8_t country_code;
    uint8_t flags;
} __packed;

struct codex_hids_report_ref {
    uint8_t id;
    uint8_t type;
} __packed;

struct codex_ble_tx_message {
    nxtkb_codex_route_t route;
    bool selected_only;
    uint16_t length;
    char data[CONFIG_NXTKB_CODEX_TX_MESSAGE_SIZE];
};

static struct codex_hids_info codex_hids_info = {
    .version = 0x0111,
    .flags = HIDS_REMOTE_WAKE | HIDS_NORMALLY_CONNECTABLE,
};
static struct codex_hids_report_ref codex_input_ref = {
    .id = NXTKB_CODEX_REPORT_ID,
    .type = HIDS_INPUT,
};
static struct codex_hids_report_ref codex_output_ref = {
    .id = NXTKB_CODEX_REPORT_ID,
    .type = HIDS_OUTPUT,
};
static uint8_t codex_ctrl_point;
static uint8_t codex_input_report[NXTKB_CODEX_REPORT_BODY_SIZE];

static struct bt_conn *connection_for_route(nxtkb_codex_route_t route)
{
    int profile_index = nxtkb_codex_transport_route_ble_profile(route);
    if (profile_index < 0 || profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return NULL;
    }

    bt_addr_le_t *address = zmk_ble_profile_address(profile_index);
    if (bt_addr_le_cmp(address, BT_ADDR_LE_ANY) == 0) {
        return NULL;
    }
    return bt_conn_lookup_addr_le(BT_ID_DEFAULT, address);
}

static const uint8_t codex_ble_report_map[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, NXTKB_CODEX_REPORT_ID,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, NXTKB_CODEX_REPORT_BODY_SIZE,
    0x09, 0x01,
    0x81, 0x02,
    0x95, NXTKB_CODEX_REPORT_BODY_SIZE,
    0x09, 0x01,
    0x91, 0x02,
    0xC0,
};

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset, const void *value, size_t value_len)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_len);
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                         uint16_t len, uint16_t offset)
{
    return read_value(conn, attr, buf, len, offset, &codex_hids_info, sizeof(codex_hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                               uint16_t len, uint16_t offset)
{
    return read_value(conn, attr, buf, len, offset, codex_ble_report_map,
                      sizeof(codex_ble_report_map));
}

static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                               uint16_t len, uint16_t offset)
{
    return read_value(conn, attr, buf, len, offset, attr->user_data,
                      sizeof(struct codex_hids_report_ref));
}

static ssize_t read_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                 uint16_t len, uint16_t offset)
{
    return read_value(conn, attr, buf, len, offset, codex_input_report,
                      sizeof(codex_input_report));
}

static ssize_t write_output_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    int profile_index = zmk_ble_profile_index(bt_conn_get_dst(conn));
    nxtkb_codex_route_t route = nxtkb_codex_transport_ble_route(profile_index);
    if (profile_index < 0 || !nxtkb_codex_transport_route_is_valid(route)) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    const uint8_t *report = buf;
    uint16_t report_length = len;

    /* HOGP normally carries the report body without the report ID because
     * the Report Reference descriptor identifies it. macOS's IOHID BLE
     * bridge keeps the report ID in SetReport payloads, however. Accept both
     * representations so the same service works with either host behavior. */
    if (len == NXTKB_CODEX_REPORT_SIZE) {
        if (report[0] != NXTKB_CODEX_REPORT_ID) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        report++;
        report_length--;
    }

    if (report_length != NXTKB_CODEX_REPORT_BODY_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t fragment_length = report[1];
    if (fragment_length > NXTKB_CODEX_FRAGMENT_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    nxtkb_codex_protocol_receive_fragment(route, report[0], &report[2], fragment_length);
    return len;
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_INF("Codex BLE notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset + len > sizeof(codex_ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy((uint8_t *)&codex_ctrl_point + offset, buf, len);
    return len;
}

/* Zephyr sorts static GATT services by symbol name. Android's HOGP host selects
 * only the first HID service, so this must sort after ZMK's normal hog_svc.
 * Otherwise Android sees only the vendor collection and creates no keyboard.
 */
BT_GATT_SERVICE_DEFINE(
    nxtkb_codex_hog_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
                           read_info, NULL, &codex_hids_info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_report_map, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_report_ref,
                       NULL, &codex_input_ref),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
                               BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_input_report, write_output_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_report_ref,
                       NULL, &codex_output_ref),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &codex_ctrl_point));

K_MSGQ_DEFINE(codex_ble_tx_queue, sizeof(struct codex_ble_tx_message),
              CONFIG_NXTKB_CODEX_TX_QUEUE_DEPTH, 4);

bool nxtkb_codex_ble_transport_ready(nxtkb_codex_route_t route)
{
    int profile_index = nxtkb_codex_transport_route_ble_profile(route);
    if (profile_index < 0) {
        return false;
    }
    struct bt_conn *conn = connection_for_route(route);
    if (conn == NULL) {
        return false;
    }

    bool ready = nxtkb_codex_transport_route_is_valid(route) &&
                 bt_gatt_is_subscribed(conn, &nxtkb_codex_hog_svc.attrs[6], BT_GATT_CCC_NOTIFY);
    bt_conn_unref(conn);
    return ready;
}

int nxtkb_codex_ble_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                           size_t length, bool selected_only)
{
    struct codex_ble_tx_message queued = {0};
    if (message == NULL || length == 0 || length > sizeof(queued.data)) {
        return -EINVAL;
    }

    if (!nxtkb_codex_transport_route_is_valid(route) ||
        nxtkb_codex_transport_route_ble_profile(route) < 0 ||
        (selected_only && !nxtkb_codex_transport_route_is_selected(route))) {
        return -ENOTCONN;
    }
    queued.route = route;
    queued.selected_only = selected_only;
    queued.length = length;
    memcpy(queued.data, message, length);
    return k_msgq_put(&codex_ble_tx_queue, &queued, K_NO_WAIT) == 0 ? 0 : -ENOSPC;
}

static void codex_ble_tx_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    struct codex_ble_tx_message message;

    while (true) {
        k_msgq_get(&codex_ble_tx_queue, &message, K_FOREVER);

        if (!nxtkb_codex_transport_route_is_valid(message.route) ||
            (message.selected_only &&
             !nxtkb_codex_transport_route_is_selected(message.route))) {
            continue;
        }

        int profile_index = nxtkb_codex_transport_route_ble_profile(message.route);

        struct bt_conn *conn = connection_for_route(message.route);
        if (conn == NULL) {
            continue;
        }

        bool ready = profile_index >= 0 &&
                     bt_gatt_is_subscribed(conn, &nxtkb_codex_hog_svc.attrs[6], BT_GATT_CCC_NOTIFY);
        if (!ready) {
            bt_conn_unref(conn);
            continue;
        }

        size_t offset = 0;
        while (offset < message.length) {
            if (!nxtkb_codex_transport_route_is_valid(message.route) ||
                (message.selected_only &&
                 !nxtkb_codex_transport_route_is_selected(message.route))) {
                break;
            }

            size_t fragment_length = MIN(message.length - offset, NXTKB_CODEX_FRAGMENT_SIZE);
            memset(codex_input_report, 0, sizeof(codex_input_report));
            codex_input_report[0] = NXTKB_CODEX_CHANNEL_RPC;
            codex_input_report[1] = fragment_length;
            memcpy(&codex_input_report[2], &message.data[offset], fragment_length);

            int err = bt_gatt_notify(conn, &nxtkb_codex_hog_svc.attrs[6], codex_input_report,
                                     sizeof(codex_input_report));
            if (err != 0) {
                LOG_WRN("Unable to send Codex BLE report: %d", err);
                break;
            }
            offset += fragment_length;
        }

        bt_conn_unref(conn);
    }
}

K_THREAD_DEFINE(codex_ble_tx_thread_id, CONFIG_NXTKB_CODEX_THREAD_STACK_SIZE,
                codex_ble_tx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);
