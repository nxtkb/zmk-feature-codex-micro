# Integrating with another ZMK keyboard

This guide keeps the keyboard repository responsible only for its matrix,
layers, and selected transports. The Codex protocol implementation remains in
this standalone module.

## 1. Add the module to the west manifest

Define or reuse an NXTKB remote in `config/west.yml`, then add the module to
the manifest's `projects` list:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: nxtkb
      url-base: https://github.com/nxtkb
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: <your-tested-zmk-revision>
      import: app/west.yml
    - name: zmk-feature-codex-micro
      remote: nxtkb
      revision: <release-tag-or-commit>
  self:
    path: config
```

Pin both projects for reproducible firmware. The public repository URL becomes
usable after this module is published as `nxtkb/zmk-feature-codex-micro`.

For a local checkout instead, add the module directory to
`ZMK_EXTRA_MODULES`:

```shell
-DZMK_EXTRA_MODULES=/absolute/path/to/zmk-feature-codex-micro
```

## 2. Apply the temporary official-ZMK compatibility patch

From the ZMK checkout:

```shell
git apply /absolute/path/to/zmk-feature-codex-micro/patches/zmk-usb-hid-interrupt-out.patch
```

Use `git apply --check` first in automated workflows. Stop applying the patch
once the same interrupt-OUT callback is available in the pinned official ZMK
revision. No other ZMK source change is required by the module.

## 3. Select transports in the central-half build

USB only:

```yaml
include:
  - board: nice_nano//zmk
    shield: corne_left
    snippet: nxtkb-codex-micro-usb
```

USB and Bluetooth:

```yaml
include:
  - board: nice_nano//zmk
    shield: corne_left
    snippet: nxtkb-codex-micro-usb nxtkb-codex-micro-ble
```

Do not add the snippets to a split peripheral such as `corne_right`. Codex runs
on the ZMK central half and follows the central's selected endpoint.

## 4. Add the key behavior

Include the module-owned behavior and constants near the top of the keymap:

```dts
#include <behaviors.dtsi>
#include <behaviors/codex_key.dtsi>
#include <dt-bindings/nxtkb/codex.h>
```

Bind actions directly or place them on a dedicated layer:

```dts
codex_layer {
    bindings = <
        &codex_key CODEX_AGENT_0
        &codex_key CODEX_AGENT_1
        &codex_key CODEX_AGENT_2
        &codex_key CODEX_AGENT_3
        &codex_key CODEX_AGENT_4
        &codex_key CODEX_AGENT_5
        &codex_key CODEX_FAST
        &codex_key CODEX_APPROVE
        &codex_key CODEX_DECLINE
        &codex_key CODEX_SPLIT
        &codex_key CODEX_MIC
        &codex_key CODEX_SEND
    >;
};
```

The complete Corne example has the correct binding count for that shield and
can be used as a compile test.

## 5. Optional status UI

No display is required. A display or LED module can subscribe to
`nxtkb_codex_state_changed` from `nxtkb/codex/events.h`, then call
`nxtkb_codex_state_get()` from `nxtkb/codex/state.h`.

The returned snapshot contains six slots, an active slot index, normalized
states, selection flags, and the original RGB values. The UI remains a separate
module so the Codex transport does not depend on a particular screen.

## 6. Resource and compatibility checks

For every target keyboard:

- compile USB-only and USB+BLE central builds;
- compile the normal peripheral build without the module;
- check flash and RAM usage, especially on boards smaller than nRF52840;
- verify normal keyboard, consumer, pointing, and ZMK Studio interfaces;
- re-pair every Bluetooth profile after the report map changes;
- test output switching with separate USB and Bluetooth hosts;
- test the installed ChatGPT desktop version because discovery is not a
  documented third-party API.

## Release boundary

The module source, protocol behavior, key behavior, examples, and generic ZMK
patch can be published under MIT. Do not publish firmware built with
`nxtkb-codex-micro-lab-identity`. Production firmware needs device identifiers
the distributor is authorized to use and a supported ChatGPT discovery path.
