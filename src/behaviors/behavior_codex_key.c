/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT nxtkb_behavior_codex_key

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <nxtkb/codex/protocol.h>

LOG_MODULE_REGISTER(nxtkb_codex_behavior, CONFIG_NXTKB_CODEX_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
    int err = nxtkb_codex_protocol_notify_key(binding->param1, true);
    if (err != 0 && err != -ENOTCONN) {
        LOG_WRN("Unable to send Codex key press: %d", err);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
    int err = nxtkb_codex_protocol_notify_key(binding->param1, false);
    if (err != 0 && err != -ENOTCONN) {
        LOG_WRN("Unable to send Codex key release: %d", err);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api codex_key_driver_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define CODEX_KEY_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &codex_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CODEX_KEY_INST)
