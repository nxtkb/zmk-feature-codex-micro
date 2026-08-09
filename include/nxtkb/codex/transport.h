/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NXTKB_CODEX_REPORT_ID 6
#define NXTKB_CODEX_REPORT_SIZE 64
#define NXTKB_CODEX_REPORT_BODY_SIZE 63
#define NXTKB_CODEX_FRAGMENT_SIZE 61
#define NXTKB_CODEX_CHANNEL_DEBUG 1
#define NXTKB_CODEX_CHANNEL_RPC 2

typedef uint32_t nxtkb_codex_route_t;

enum nxtkb_codex_transport_kind {
    NXTKB_CODEX_TRANSPORT_USB = 0,
    NXTKB_CODEX_TRANSPORT_BLE = 1,
};

bool nxtkb_codex_transport_ready(void);
bool nxtkb_codex_transport_route_ready(nxtkb_codex_route_t route);
int nxtkb_codex_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                       size_t length);
int nxtkb_codex_transport_send_selected_message(nxtkb_codex_route_t route,
                                                const char *message, size_t length);

nxtkb_codex_route_t nxtkb_codex_transport_current_route(void);
/* True when the route belongs to the current routing generation.  It does not
 * imply that the route is the selected keyboard output. */
bool nxtkb_codex_transport_route_is_current(nxtkb_codex_route_t route);
bool nxtkb_codex_transport_route_is_valid(nxtkb_codex_route_t route);
bool nxtkb_codex_transport_route_is_selected(nxtkb_codex_route_t route);
bool nxtkb_codex_transport_route_targets_selected(nxtkb_codex_route_t route);
bool nxtkb_codex_transport_route_is_usb(nxtkb_codex_route_t route);
int nxtkb_codex_transport_route_ble_profile(nxtkb_codex_route_t route);
int nxtkb_codex_transport_route_index(nxtkb_codex_route_t route);
nxtkb_codex_route_t nxtkb_codex_transport_usb_route(void);
nxtkb_codex_route_t nxtkb_codex_transport_ble_route(int profile_index);

bool nxtkb_codex_transport_usb_is_selected(void);
bool nxtkb_codex_transport_ble_profile_is_selected(int profile_index);
int nxtkb_codex_transport_selected_ble_profile(void);
bool nxtkb_codex_usb_transport_ready(nxtkb_codex_route_t route);
int nxtkb_codex_usb_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                           size_t length, bool selected_only);

#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
bool nxtkb_codex_ble_transport_ready(nxtkb_codex_route_t route);
int nxtkb_codex_ble_transport_send_message(nxtkb_codex_route_t route, const char *message,
                                           size_t length, bool selected_only);
#endif
