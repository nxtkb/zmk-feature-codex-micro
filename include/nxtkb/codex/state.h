/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <nxtkb/codex/transport.h>

#define NXTKB_CODEX_AGENT_COUNT 6

enum nxtkb_codex_agent_state {
    NXTKB_CODEX_AGENT_OFF,
    NXTKB_CODEX_AGENT_IDLE,
    NXTKB_CODEX_AGENT_WORKING,
    NXTKB_CODEX_AGENT_UNREAD,
    NXTKB_CODEX_AGENT_AWAITING_INPUT,
    NXTKB_CODEX_AGENT_ERROR,
    NXTKB_CODEX_AGENT_UNKNOWN,
};

struct nxtkb_codex_agent_slot {
    enum nxtkb_codex_agent_state state;
    bool selected;
    uint32_t raw_rgb;
};

struct nxtkb_codex_agent_snapshot {
    struct nxtkb_codex_agent_slot slots[NXTKB_CODEX_AGENT_COUNT];
    int8_t active_slot;
};

void nxtkb_codex_state_get(struct nxtkb_codex_agent_snapshot *snapshot);
void nxtkb_codex_state_get_for_route(nxtkb_codex_route_t route,
                                     struct nxtkb_codex_agent_snapshot *snapshot);
void nxtkb_codex_state_set_slots(nxtkb_codex_route_t route,
                                 const struct nxtkb_codex_agent_slot *slots,
                                 uint8_t changed_slots);
void nxtkb_codex_state_selected_route_changed(void);
