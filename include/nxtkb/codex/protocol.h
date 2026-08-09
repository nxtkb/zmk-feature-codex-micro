/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nxtkb/codex/transport.h>

void nxtkb_codex_protocol_receive_fragment(nxtkb_codex_route_t route, uint8_t channel,
                                           const uint8_t *data, size_t length);
int nxtkb_codex_protocol_notify_key(uint8_t key, bool pressed);
