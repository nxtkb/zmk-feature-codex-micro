/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#include <nxtkb/codex/state.h>

#include <nxtkb/codex/transport.h>

LOG_MODULE_REGISTER(nxtkb_codex_transport, CONFIG_NXTKB_CODEX_LOG_LEVEL);

#define NXTKB_CODEX_ENDPOINT_MASK 0xFFU
#define NXTKB_CODEX_ENDPOINT_NONE 0U
#define NXTKB_CODEX_ENDPOINT_USB 1U
#define NXTKB_CODEX_ENDPOINT_BLE_BASE 2U
#define NXTKB_CODEX_ROUTE_GENERATION_STEP (NXTKB_CODEX_ENDPOINT_MASK + 1U)
#define NXTKB_CODEX_TRANSPORT_INIT_PRIORITY 99

/* The low byte identifies the endpoint and the upper bytes form a generation.
 * One atomic token therefore prevents torn transport/profile reads and makes
 * work queued before an A -> B -> A round trip permanently stale. */
static atomic_t selected_route = ATOMIC_INIT(NXTKB_CODEX_ENDPOINT_NONE);

static uint32_t endpoint_key(struct zmk_endpoint_instance endpoint)
{
    switch (endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        return NXTKB_CODEX_ENDPOINT_USB;
    case ZMK_TRANSPORT_BLE:
        return NXTKB_CODEX_ENDPOINT_BLE_BASE + endpoint.ble.profile_index;
    case ZMK_TRANSPORT_NONE:
    default:
        return NXTKB_CODEX_ENDPOINT_NONE;
    }
}

static uint32_t route_endpoint(nxtkb_codex_route_t route)
{
    return route & NXTKB_CODEX_ENDPOINT_MASK;
}

static uint32_t route_generation(nxtkb_codex_route_t route)
{
    return route & ~NXTKB_CODEX_ENDPOINT_MASK;
}

static nxtkb_codex_route_t route_for_endpoint(uint32_t endpoint)
{
    return route_generation(nxtkb_codex_transport_current_route()) | endpoint;
}

static nxtkb_codex_route_t next_route(nxtkb_codex_route_t previous, uint32_t endpoint)
{
    uint32_t generation = (previous & ~NXTKB_CODEX_ENDPOINT_MASK) +
                          NXTKB_CODEX_ROUTE_GENERATION_STEP;

    /* Generation zero is reserved for the uninitialized route. */
    if (generation == 0U) {
        generation = NXTKB_CODEX_ROUTE_GENERATION_STEP;
    }
    return generation | endpoint;
}

nxtkb_codex_route_t nxtkb_codex_transport_current_route(void)
{
    return (nxtkb_codex_route_t)atomic_get(&selected_route);
}

bool nxtkb_codex_transport_route_is_current(nxtkb_codex_route_t route)
{
    return route_endpoint(route) != NXTKB_CODEX_ENDPOINT_NONE &&
           route_generation(route) ==
               route_generation(nxtkb_codex_transport_current_route());
}

bool nxtkb_codex_transport_route_is_valid(nxtkb_codex_route_t route)
{
    return route_endpoint(route) != NXTKB_CODEX_ENDPOINT_NONE;
}

bool nxtkb_codex_transport_route_is_selected(nxtkb_codex_route_t route)
{
    return route == nxtkb_codex_transport_current_route();
}

bool nxtkb_codex_transport_route_targets_selected(nxtkb_codex_route_t route)
{
    return route_endpoint(route) ==
           route_endpoint(nxtkb_codex_transport_current_route());
}

bool nxtkb_codex_transport_route_is_usb(nxtkb_codex_route_t route)
{
    return route_endpoint(route) == NXTKB_CODEX_ENDPOINT_USB;
}

int nxtkb_codex_transport_route_ble_profile(nxtkb_codex_route_t route)
{
    uint32_t endpoint = route_endpoint(route);
    return endpoint >= NXTKB_CODEX_ENDPOINT_BLE_BASE
               ? (int)(endpoint - NXTKB_CODEX_ENDPOINT_BLE_BASE)
               : -1;
}

int nxtkb_codex_transport_route_index(nxtkb_codex_route_t route)
{
    uint32_t endpoint = route_endpoint(route);
    if (endpoint == NXTKB_CODEX_ENDPOINT_USB) {
        return 0;
    }
    if (endpoint >= NXTKB_CODEX_ENDPOINT_BLE_BASE) {
        return (int)(endpoint - NXTKB_CODEX_ENDPOINT_BLE_BASE) + 1;
    }
    return -1;
}

nxtkb_codex_route_t nxtkb_codex_transport_usb_route(void)
{
    return route_for_endpoint(NXTKB_CODEX_ENDPOINT_USB);
}

nxtkb_codex_route_t nxtkb_codex_transport_ble_route(int profile_index)
{
    if (profile_index < 0) {
        return route_for_endpoint(NXTKB_CODEX_ENDPOINT_NONE);
    }
    return route_for_endpoint(NXTKB_CODEX_ENDPOINT_BLE_BASE + (uint32_t)profile_index);
}

bool nxtkb_codex_transport_usb_is_selected(void)
{
    return nxtkb_codex_transport_route_is_usb(nxtkb_codex_transport_current_route());
}

bool nxtkb_codex_transport_ble_profile_is_selected(int profile_index)
{
    return profile_index >= 0 &&
           nxtkb_codex_transport_route_ble_profile(nxtkb_codex_transport_current_route()) ==
               profile_index;
}

int nxtkb_codex_transport_selected_ble_profile(void)
{
    return nxtkb_codex_transport_route_ble_profile(nxtkb_codex_transport_current_route());
}

bool nxtkb_codex_transport_route_ready(nxtkb_codex_route_t route)
{
#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
    if (nxtkb_codex_transport_route_ble_profile(route) >= 0) {
        return nxtkb_codex_ble_transport_ready(route);
    }
#endif
    return nxtkb_codex_transport_route_is_usb(route) &&
           nxtkb_codex_usb_transport_ready(route);
}

bool nxtkb_codex_transport_ready(void)
{
    return nxtkb_codex_transport_route_ready(nxtkb_codex_transport_current_route());
}

int nxtkb_codex_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                       size_t length)
{
#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
    if (nxtkb_codex_transport_route_ble_profile(route) >= 0) {
        return nxtkb_codex_ble_transport_send_message(route, message, length, false);
    }
#endif
    if (nxtkb_codex_transport_route_is_usb(route)) {
        return nxtkb_codex_usb_transport_send_message(route, message, length, false);
    }
    return -ENOTCONN;
}

int nxtkb_codex_transport_send_selected_message(nxtkb_codex_route_t route,
                                                const char *message, size_t length)
{
#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
    if (nxtkb_codex_transport_route_ble_profile(route) >= 0) {
        return nxtkb_codex_ble_transport_send_message(route, message, length, true);
    }
#endif
    if (nxtkb_codex_transport_route_is_usb(route)) {
        return nxtkb_codex_usb_transport_send_message(route, message, length, true);
    }
    return -ENOTCONN;
}

static int codex_endpoint_listener(const zmk_event_t *eh)
{
    const struct zmk_endpoint_changed *event = as_zmk_endpoint_changed(eh);
    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t next_endpoint = endpoint_key(event->endpoint);
    nxtkb_codex_route_t previous = nxtkb_codex_transport_current_route();
    if (next_endpoint == route_endpoint(previous)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    nxtkb_codex_route_t next = next_route(previous, next_endpoint);
    nxtkb_codex_route_t isolated =
        (next & ~NXTKB_CODEX_ENDPOINT_MASK) | NXTKB_CODEX_ENDPOINT_NONE;

    /* Invalidate queued work from the previous selection. Protocol and Agent
     * state remain alive per endpoint so both hosts can keep their Codex
     * sessions healthy while only the selected route receives key actions. */
    atomic_set(&selected_route, (atomic_val_t)isolated);
    atomic_set(&selected_route, (atomic_val_t)next);
    nxtkb_codex_state_selected_route_changed();

    LOG_INF("Codex route changed: 0x%08x -> 0x%08x", previous, next);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nxtkb_codex_endpoint, codex_endpoint_listener);
ZMK_SUBSCRIPTION(nxtkb_codex_endpoint, zmk_endpoint_changed);

static int nxtkb_codex_transport_init(void)
{
    uint32_t endpoint = endpoint_key(zmk_endpoint_get_selected());
    nxtkb_codex_route_t route = next_route(nxtkb_codex_transport_current_route(), endpoint);
    atomic_set(&selected_route, (atomic_val_t)route);
    LOG_INF("Initial Codex route: 0x%08x", route);
    return 0;
}

BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < NXTKB_CODEX_TRANSPORT_INIT_PRIORITY,
             "Codex transport must initialize after ZMK endpoints");
SYS_INIT(nxtkb_codex_transport_init, APPLICATION, NXTKB_CODEX_TRANSPORT_INIT_PRIORITY);
