/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include <zmk/usb.h>

#include <nxtkb/codex/protocol.h>
#include <nxtkb/codex/transport.h>

LOG_MODULE_REGISTER(nxtkb_codex_usb, CONFIG_NXTKB_CODEX_LOG_LEVEL);

/* These request values are private to ZMK's legacy USB HID implementation. */
#define HID_GET_REPORT_TYPE_MASK 0xFF00
#define HID_REPORT_TYPE_OUTPUT 0x0200

struct codex_rx_frame {
    nxtkb_codex_route_t route;
    uint8_t length;
    uint8_t data[NXTKB_CODEX_REPORT_SIZE];
};

struct codex_tx_message {
    nxtkb_codex_route_t route;
    bool selected_only;
    uint16_t length;
    char data[CONFIG_NXTKB_CODEX_TX_MESSAGE_SIZE];
};

K_MSGQ_DEFINE(codex_rx_queue, sizeof(struct codex_rx_frame),
              CONFIG_NXTKB_CODEX_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(codex_tx_queue, sizeof(struct codex_tx_message),
              CONFIG_NXTKB_CODEX_TX_QUEUE_DEPTH, 4);
K_SEM_DEFINE(codex_tx_ready, 1, 1);

static const struct device *codex_hid_dev;

static const uint8_t codex_hid_report_desc[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, NXTKB_CODEX_REPORT_ID,
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xFF, 0x00, /* Logical Maximum (255) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, NXTKB_CODEX_REPORT_BODY_SIZE,
    0x09, 0x01,
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x95, NXTKB_CODEX_REPORT_BODY_SIZE,
    0x09, 0x01,
    0x91, 0x02,       /* Output (Data, Variable, Absolute) */
    0xC0,             /* End Collection */
};

static int queue_rx_report(const uint8_t *data, size_t length)
{
    struct codex_rx_frame frame = {0};
    frame.route = nxtkb_codex_transport_usb_route();

    if (length == NXTKB_CODEX_REPORT_BODY_SIZE) {
        frame.data[0] = NXTKB_CODEX_REPORT_ID;
        memcpy(&frame.data[1], data, length);
        frame.length = NXTKB_CODEX_REPORT_SIZE;
    } else if (length == NXTKB_CODEX_REPORT_SIZE) {
        memcpy(frame.data, data, length);
        frame.length = length;
    } else {
        LOG_WRN("Ignoring Codex output report with length %u", (unsigned int)length);
        return -EMSGSIZE;
    }

    if (frame.data[0] != NXTKB_CODEX_REPORT_ID) {
        LOG_WRN("Ignoring Codex output report with ID %u", frame.data[0]);
        return -EINVAL;
    }

    if (k_msgq_put(&codex_rx_queue, &frame, K_NO_WAIT) != 0) {
        LOG_WRN("Codex RX queue full");
        return -ENOSPC;
    }

    return 0;
}

static int codex_set_report(const struct device *dev, struct usb_setup_packet *setup,
                            int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);

    uint8_t report_id = setup->wValue & 0xFF;

    if ((setup->wValue & HID_GET_REPORT_TYPE_MASK) != HID_REPORT_TYPE_OUTPUT ||
        report_id != NXTKB_CODEX_REPORT_ID) {
        return -ENOTSUP;
    }

    if (*len < 0 || *data == NULL) {
        return -EINVAL;
    }

    return queue_rx_report(*data, (size_t)*len);
}

static void codex_int_out_ready(const struct device *dev)
{
    uint8_t report[NXTKB_CODEX_REPORT_SIZE];
    uint32_t bytes_read = 0;
    int err = hid_int_ep_read(dev, report, sizeof(report), &bytes_read);

    if (err != 0) {
        LOG_WRN("Unable to read Codex OUT endpoint: %d", err);
        return;
    }

    queue_rx_report(report, bytes_read);
}

static void codex_int_in_ready(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&codex_tx_ready);
}

static const struct hid_ops codex_hid_ops = {
    .set_report = codex_set_report,
    .int_in_ready = codex_int_in_ready,
    .int_out_ready = codex_int_out_ready,
};

bool nxtkb_codex_usb_transport_ready(nxtkb_codex_route_t route)
{
    return nxtkb_codex_transport_route_is_valid(route) &&
           nxtkb_codex_transport_route_is_usb(route) && codex_hid_dev != NULL &&
           zmk_usb_is_hid_ready();
}

int nxtkb_codex_usb_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                           size_t length, bool selected_only)
{
    struct codex_tx_message queued = {0};

    if (!nxtkb_codex_transport_route_is_valid(route) ||
        !nxtkb_codex_transport_route_is_usb(route) ||
        (selected_only && !nxtkb_codex_transport_route_is_selected(route))) {
        return -ENOTCONN;
    }
    if (message == NULL || length == 0 || length > sizeof(queued.data)) {
        return -EINVAL;
    }

    queued.route = route;
    queued.selected_only = selected_only;
    queued.length = (uint16_t)length;
    memcpy(queued.data, message, length);

    if (k_msgq_put(&codex_tx_queue, &queued, K_NO_WAIT) != 0) {
        return -ENOSPC;
    }

    return 0;
}

static int send_report(nxtkb_codex_route_t route, bool selected_only, const uint8_t *report)
{
    int64_t deadline = k_uptime_get() + 2000;

    while (k_uptime_get() < deadline) {
        if (!nxtkb_codex_transport_route_is_valid(route) ||
            (selected_only && !nxtkb_codex_transport_route_is_selected(route))) {
            return -ENOTCONN;
        }

        enum usb_dc_status_code status = zmk_usb_get_status();
        if (status == USB_DC_SUSPEND) {
            int wake_err = usb_wakeup_request();
            if (wake_err != 0 && wake_err != -EAGAIN) {
                return wake_err;
            }
            k_sleep(K_MSEC(10));
            continue;
        }

        bool acquired = k_sem_take(&codex_tx_ready, K_MSEC(200)) == 0;
        if (!nxtkb_codex_transport_route_is_valid(route) ||
            (selected_only && !nxtkb_codex_transport_route_is_selected(route))) {
            if (acquired) {
                k_sem_give(&codex_tx_ready);
            }
            return -ENOTCONN;
        }
        int err = hid_int_ep_write(codex_hid_dev, report, NXTKB_CODEX_REPORT_SIZE, NULL);
        if (err == 0) {
            return 0;
        }

        if (acquired) {
            k_sem_give(&codex_tx_ready);
        }
        if (err != -EAGAIN && err != -EBUSY) {
            return err;
        }

        /* A completion callback can be lost across suspend/resume.  Trying
         * the endpoint after a semaphore timeout safely re-synchronizes us:
         * the USB driver returns -EAGAIN while a real transfer is active. */
        k_sleep(K_MSEC(5));
    }

    return -ETIMEDOUT;
}

static void codex_rx_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    struct codex_rx_frame frame;

    while (true) {
        k_msgq_get(&codex_rx_queue, &frame, K_FOREVER);

        if (frame.length != NXTKB_CODEX_REPORT_SIZE ||
            !nxtkb_codex_transport_route_is_valid(frame.route) ||
            !nxtkb_codex_transport_route_is_usb(frame.route)) {
            continue;
        }

        uint8_t channel = frame.data[1];
        uint8_t fragment_length = frame.data[2];
        if (fragment_length > NXTKB_CODEX_FRAGMENT_SIZE) {
            LOG_WRN("Invalid Codex fragment length %u", fragment_length);
            continue;
        }

        nxtkb_codex_protocol_receive_fragment(frame.route, channel, &frame.data[3],
                                              fragment_length);
    }
}

static void codex_tx_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    struct codex_tx_message message;

    while (true) {
        k_msgq_get(&codex_tx_queue, &message, K_FOREVER);

        size_t offset = 0;
        while (offset < message.length) {
            if (!nxtkb_codex_transport_route_is_valid(message.route) ||
                (message.selected_only &&
                 !nxtkb_codex_transport_route_is_selected(message.route)) ||
                !nxtkb_codex_transport_route_is_usb(message.route)) {
                break;
            }

            uint8_t report[NXTKB_CODEX_REPORT_SIZE] = {0};
            size_t remaining = message.length - offset;
            size_t fragment_length = MIN(remaining, NXTKB_CODEX_FRAGMENT_SIZE);

            report[0] = NXTKB_CODEX_REPORT_ID;
            report[1] = NXTKB_CODEX_CHANNEL_RPC;
            report[2] = fragment_length;
            memcpy(&report[3], &message.data[offset], fragment_length);

            int err = send_report(message.route, message.selected_only, report);
            if (err != 0) {
                LOG_WRN("Unable to send Codex IN report: %d", err);
                break;
            }

            offset += fragment_length;
        }
    }
}

K_THREAD_DEFINE(codex_rx_thread_id, CONFIG_NXTKB_CODEX_THREAD_STACK_SIZE,
                codex_rx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);
K_THREAD_DEFINE(codex_tx_thread_id, CONFIG_NXTKB_CODEX_THREAD_STACK_SIZE,
                codex_tx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);

static int nxtkb_codex_usb_init(void)
{
    codex_hid_dev = device_get_binding("HID_1");
    if (codex_hid_dev == NULL) {
        LOG_ERR("Unable to locate HID_1; CONFIG_USB_HID_DEVICE_COUNT must be at least 2");
        return -ENODEV;
    }

    usb_hid_register_device(codex_hid_dev, codex_hid_report_desc,
                            sizeof(codex_hid_report_desc), &codex_hid_ops);
    return usb_hid_init(codex_hid_dev);
}

SYS_INIT(nxtkb_codex_usb_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
