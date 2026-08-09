/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

#include <zmk/event_manager.h>

struct nxtkb_codex_state_changed {
    uint8_t changed_slots;
};

ZMK_EVENT_DECLARE(nxtkb_codex_state_changed);
