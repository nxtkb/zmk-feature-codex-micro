/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_NXTKB_CODEX_MICRO_BLE)
#include <zmk/ble.h>
#define NXTKB_CODEX_STATE_COUNT (1 + ZMK_BLE_PROFILE_COUNT)
#else
#define NXTKB_CODEX_STATE_COUNT 1
#endif

#include <nxtkb/codex/events.h>
#include <nxtkb/codex/state.h>

ZMK_EVENT_IMPL(nxtkb_codex_state_changed);

K_MUTEX_DEFINE(codex_state_mutex);

static struct nxtkb_codex_agent_snapshot codex_states[NXTKB_CODEX_STATE_COUNT];
static bool codex_states_initialized[NXTKB_CODEX_STATE_COUNT];

static struct nxtkb_codex_agent_snapshot *state_for_route(nxtkb_codex_route_t route)
{
    int index = nxtkb_codex_transport_route_index(route);
    if (index < 0 || index >= ARRAY_SIZE(codex_states)) {
        return NULL;
    }
    if (!codex_states_initialized[index]) {
        codex_states[index].active_slot = -1;
        codex_states_initialized[index] = true;
    }
    return &codex_states[index];
}

static bool slot_equal(const struct nxtkb_codex_agent_slot *left,
                       const struct nxtkb_codex_agent_slot *right)
{
    return left->state == right->state && left->selected == right->selected &&
           left->raw_rgb == right->raw_rgb;
}

void nxtkb_codex_state_get(struct nxtkb_codex_agent_snapshot *snapshot)
{
    nxtkb_codex_state_get_for_route(nxtkb_codex_transport_current_route(), snapshot);
}

void nxtkb_codex_state_get_for_route(nxtkb_codex_route_t route,
                                     struct nxtkb_codex_agent_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    k_mutex_lock(&codex_state_mutex, K_FOREVER);
    struct nxtkb_codex_agent_snapshot *state = state_for_route(route);
    if (state == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->active_slot = -1;
    } else {
        *snapshot = *state;
    }
    k_mutex_unlock(&codex_state_mutex);
}

void nxtkb_codex_state_set_slots(nxtkb_codex_route_t route,
                                 const struct nxtkb_codex_agent_slot *slots,
                                 uint8_t changed_slots)
{
    if (slots == NULL || changed_slots == 0) {
        return;
    }

    uint8_t actual_changes = 0;
    k_mutex_lock(&codex_state_mutex, K_FOREVER);
    struct nxtkb_codex_agent_snapshot *state = state_for_route(route);
    if (state == NULL) {
        k_mutex_unlock(&codex_state_mutex);
        return;
    }
    for (uint8_t i = 0; i < NXTKB_CODEX_AGENT_COUNT; i++) {
        if ((changed_slots & BIT(i)) != 0 && !slot_equal(&state->slots[i], &slots[i])) {
            state->slots[i] = slots[i];
            actual_changes |= BIT(i);
        }
    }

    if (actual_changes == 0) {
        k_mutex_unlock(&codex_state_mutex);
        return;
    }

    state->active_slot = -1;
    for (uint8_t i = 0; i < NXTKB_CODEX_AGENT_COUNT; i++) {
        if (state->slots[i].selected &&
            state->slots[i].state != NXTKB_CODEX_AGENT_OFF) {
            state->active_slot = i;
            break;
        }
    }
    k_mutex_unlock(&codex_state_mutex);

    if (nxtkb_codex_transport_route_targets_selected(route)) {
        raise_nxtkb_codex_state_changed(
            (struct nxtkb_codex_state_changed){.changed_slots = actual_changes});
    }
}

void nxtkb_codex_state_selected_route_changed(void)
{
    raise_nxtkb_codex_state_changed(
        (struct nxtkb_codex_state_changed){.changed_slots = BIT_MASK(NXTKB_CODEX_AGENT_COUNT)});
}
