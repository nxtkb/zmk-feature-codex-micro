/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
#include <zmk/ble.h>
#define NXTKB_CODEX_PROTOCOL_CONTEXT_COUNT (1 + ZMK_BLE_PROFILE_COUNT)
#else
#define NXTKB_CODEX_PROTOCOL_CONTEXT_COUNT 1
#endif

#include <nxtkb/codex/protocol.h>
#include <nxtkb/codex/state.h>
#include <nxtkb/codex/transport.h>

LOG_MODULE_REGISTER(nxtkb_codex_protocol, CONFIG_NXTKB_CODEX_LOG_LEVEL);

struct codex_rpc_context {
    char buffer[CONFIG_NXTKB_CODEX_RPC_BUFFER_SIZE];
    size_t length;
    int64_t last_fragment_at;
};

static struct codex_rpc_context rpc_contexts[NXTKB_CODEX_PROTOCOL_CONTEXT_COUNT];
K_MUTEX_DEFINE(codex_rpc_mutex);

static struct codex_rpc_context *context_for_route(nxtkb_codex_route_t route)
{
    int index = nxtkb_codex_transport_route_index(route);
    if (index < 0 || index >= ARRAY_SIZE(rpc_contexts)) {
        return NULL;
    }
    return &rpc_contexts[index];
}

/*
 * The Work Louder HID transport is deliberately asymmetric: host requests are
 * raw JSON split into 61-byte reports, while device responses/notifications
 * are newline terminated.  Do not wait for a newline on RX; it never arrives
 * for JSON-RPC requests sent by wl-device-kit.
 */
static bool rpc_buffer_contains_complete_object(const struct codex_rpc_context *context)
{
    int depth = 0;
    bool started = false;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < context->length; i++) {
        char byte = context->buffer[i];

        if (!started) {
            if (byte == '{') {
                started = true;
                depth = 1;
            }
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            }
            continue;
        }

        if (byte == '"') {
            in_string = true;
        } else if (byte == '{') {
            depth++;
        } else if (byte == '}') {
            depth--;
            if (depth == 0) {
                return true;
            }
        }
    }

    return false;
}

static bool extract_method(const char *json, char *method, size_t method_size)
{
    const char *key = strstr(json, "\"method\"");
    if (key == NULL) {
        return false;
    }

    const char *colon = strchr(key + 8, ':');
    if (colon == NULL) {
        return false;
    }

    const char *start = strchr(colon + 1, '"');
    if (start == NULL) {
        return false;
    }
    start++;

    const char *end = strchr(start, '"');
    if (end == NULL || end == start || (size_t)(end - start) >= method_size) {
        return false;
    }

    memcpy(method, start, end - start);
    method[end - start] = '\0';
    return true;
}

static bool extract_id(const char *json, int *id)
{
    int object_depth = 0;
    int array_depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char *cursor = json; *cursor != '\0'; cursor++) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (*cursor == '\\') {
                escaped = true;
            } else if (*cursor == '"') {
                in_string = false;
            }
            continue;
        }

        if (*cursor == '{') {
            object_depth++;
            continue;
        }
        if (*cursor == '}') {
            object_depth--;
            continue;
        }
        if (*cursor == '[') {
            array_depth++;
            continue;
        }
        if (*cursor == ']') {
            array_depth--;
            continue;
        }
        if (*cursor != '"') {
            continue;
        }

        /* Only accept the id member of the root request object.  Methods such
         * as v.oai.thstatus carry per-agent objects with their own id fields. */
        if (object_depth == 1 && array_depth == 0 && strncmp(cursor, "\"id\"", 4) == 0) {
            const char *colon = cursor + 4;
            while (*colon == ' ' || *colon == '\t') {
                colon++;
            }
            if (*colon != ':') {
                continue;
            }

            char *end;
            long parsed = strtol(colon + 1, &end, 10);
            if (end == colon + 1 || parsed < 0 || parsed > 998) {
                return false;
            }

            *id = (int)parsed;
            return true;
        }

        in_string = true;
    }

    return false;
}

static bool extract_object_integer(const char *object, const char *field, long *value)
{
    char key[12];
    int key_length = snprintf(key, sizeof(key), "\"%s\"", field);
    if (key_length <= 0 || key_length >= sizeof(key)) {
        return false;
    }

    const char *match = strstr(object, key);
    if (match == NULL) {
        return false;
    }

    const char *colon = strchr(match + key_length, ':');
    if (colon == NULL) {
        return false;
    }

    char *end;
    long parsed = strtol(colon + 1, &end, 10);
    if (end == colon + 1) {
        return false;
    }

    *value = parsed;
    return true;
}

static enum nxtkb_codex_agent_state state_from_color(uint32_t color)
{
    switch (color) {
    case 0x000000:
        return NXTKB_CODEX_AGENT_OFF;
    case 0xFFFFFF:
        return NXTKB_CODEX_AGENT_IDLE;
    case 0x304FFE:
        return NXTKB_CODEX_AGENT_WORKING;
    case 0x00FF4C:
        return NXTKB_CODEX_AGENT_UNREAD;
    case 0xFF6D00:
        return NXTKB_CODEX_AGENT_AWAITING_INPUT;
    case 0xFF0033:
        return NXTKB_CODEX_AGENT_ERROR;
    default:
        return NXTKB_CODEX_AGENT_UNKNOWN;
    }
}

static void process_thread_status(nxtkb_codex_route_t route, char *json)
{
    const char *params = strstr(json, "\"params\"");
    if (params == NULL) {
        return;
    }

    char *array = strchr((char *)params, '[');
    if (array == NULL) {
        return;
    }

    struct nxtkb_codex_agent_snapshot snapshot;
    nxtkb_codex_state_get_for_route(route, &snapshot);
    uint8_t changed_slots = 0;

    for (char *cursor = array + 1; *cursor != '\0' && *cursor != ']'; cursor++) {
        if (*cursor != '{') {
            continue;
        }

        char *end = strchr(cursor + 1, '}');
        if (end == NULL) {
            break;
        }

        char saved = end[1];
        end[1] = '\0';

        long id;
        if (extract_object_integer(cursor, "id", &id) && id >= 0 &&
            id < NXTKB_CODEX_AGENT_COUNT) {
            struct nxtkb_codex_agent_slot slot = snapshot.slots[id];
            long color;
            long effect;

            if (extract_object_integer(cursor, "c", &color) && color >= 0 && color <= 0xFFFFFF) {
                slot.raw_rgb = (uint32_t)color;
                slot.state = state_from_color(slot.raw_rgb);
            }
            if (extract_object_integer(cursor, "e", &effect) && effect >= 0 && effect <= UINT8_MAX) {
                slot.selected = effect == 4;
            }

            snapshot.slots[id] = slot;
            changed_slots |= BIT(id);
        }

        end[1] = saved;
        cursor = end;
    }

    nxtkb_codex_state_set_slots(route, snapshot.slots, changed_slots);
}

static void send_formatted_response(nxtkb_codex_route_t route, const char *format, int id)
{
    char response[CONFIG_NXTKB_CODEX_TX_MESSAGE_SIZE];
    int length = snprintf(response, sizeof(response), format, id);

    if (length <= 0 || length >= sizeof(response)) {
        LOG_WRN("Codex response did not fit TX buffer");
        return;
    }

    int err = nxtkb_codex_transport_send_message(route, response, length);
    if (err != 0) {
        LOG_WRN("Unable to queue Codex response: %d", err);
    }
}

static void process_rpc_message(nxtkb_codex_route_t route, char *json)
{
    char method[32];
    int id = 0;
    bool has_id = extract_id(json, &id);

    if (!extract_method(json, method, sizeof(method))) {
        if (has_id) {
            send_formatted_response(route,
                "{\"id\":%d,\"error\":{\"code\":-32600,\"message\":\"Invalid request\"}}\n",
                id);
        }
        return;
    }

    if (strcmp(method, "sys.version") == 0) {
        if (has_id) {
            send_formatted_response(route,
                "{\"id\":%d,\"result\":{\"version\":\"nxtkb-codex/0.7.0\"}}\n",
                id);
        }
        return;
    }

    if (strcmp(method, "device.status") == 0) {
        if (has_id) {
            /* wl-device-kit treats every device.status member as optional.
             * Keep this health-check response below one 61-byte HID payload;
             * it is polled while the app is idle and must never depend on a
             * second interrupt-IN transfer. */
            send_formatted_response(route,
                "{\"id\":%d,\"result\":{\"version\":\"0.7.0\"}}\n", id);
        }
        return;
    }

    if (strcmp(method, "v.oai.thstatus") == 0) {
        process_thread_status(route, json);
        if (has_id) {
            send_formatted_response(route, "{\"id\":%d,\"result\":null}\n", id);
        }
        return;
    }

    if (strcmp(method, "v.oai.rgbcfg") == 0) {
        if (has_id) {
            send_formatted_response(route, "{\"id\":%d,\"result\":null}\n", id);
        }
        return;
    }

    if (has_id) {
        send_formatted_response(route,
            "{\"id\":%d,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n",
            id);
    }
}

void nxtkb_codex_protocol_receive_fragment(nxtkb_codex_route_t route, uint8_t channel,
                                           const uint8_t *data, size_t length)
{
    if (channel != NXTKB_CODEX_CHANNEL_RPC || data == NULL || length == 0) {
        return;
    }

    k_mutex_lock(&codex_rpc_mutex, K_FOREVER);

    /* The route may have changed while an ISR/workqueue callback was waiting
     * for this mutex. Never let a stale endpoint repopulate the reset buffer. */
    if (!nxtkb_codex_transport_route_is_valid(route)) {
        k_mutex_unlock(&codex_rpc_mutex);
        return;
    }

    struct codex_rpc_context *context = context_for_route(route);
    if (context == NULL) {
        k_mutex_unlock(&codex_rpc_mutex);
        return;
    }

    int64_t now = k_uptime_get();
    if (context->length > 0 &&
        now - context->last_fragment_at > CONFIG_NXTKB_CODEX_FRAME_TIMEOUT_MS) {
        context->length = 0;
    }
    context->last_fragment_at = now;

    for (size_t i = 0; i < length; i++) {
        char byte = data[i];

        if (byte == '\n' || byte == '\r') {
            context->buffer[context->length] = '\0';
            if (context->length > 0) {
                process_rpc_message(route, context->buffer);
            }
            context->length = 0;
            continue;
        }

        if (context->length >= sizeof(context->buffer) - 1) {
            LOG_WRN("Codex RPC message exceeded reassembly buffer");
            context->length = 0;
            continue;
        }

        context->buffer[context->length++] = byte;

        if (rpc_buffer_contains_complete_object(context)) {
            context->buffer[context->length] = '\0';
            process_rpc_message(route, context->buffer);
            context->length = 0;
        }
    }

    k_mutex_unlock(&codex_rpc_mutex);
}

int nxtkb_codex_protocol_notify_key(uint8_t key, bool pressed)
{
    if (key > 12) {
        return -EINVAL;
    }
    if (!nxtkb_codex_transport_ready()) {
        return -ENOTCONN;
    }

    char message[CONFIG_NXTKB_CODEX_TX_MESSAGE_SIZE];
    int length;
    if (key <= 5) {
        length = snprintf(
            message, sizeof(message),
            "{\"method\":\"v.oai.hid\",\"params\":{\"k\":\"AG%02u\",\"act\":%u,\"ag\":%u}}\n",
            key, pressed ? 1 : 0, key);
    } else {
        length = snprintf(
            message, sizeof(message),
            "{\"method\":\"v.oai.hid\",\"params\":{\"k\":\"ACT%02u\",\"act\":%u}}\n",
            key, pressed ? 1 : 0);
    }

    if (length <= 0 || length >= sizeof(message)) {
        return -EMSGSIZE;
    }

    return nxtkb_codex_transport_send_selected_message(
        nxtkb_codex_transport_current_route(), message, length);
}
