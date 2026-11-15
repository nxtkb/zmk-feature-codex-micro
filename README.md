# zmk-feature-codex-micro

An experimental, keyboard-independent ZMK module that exposes Codex Micro-style
controls over USB and Bluetooth HID.

The module is maintained by NXTKB, but it does not depend on NXTKB keyboard
hardware, a display, a particular matrix, or a particular keymap. Any ZMK
central half with USB device support and enough flash and RAM can bind the
supplied `&codex_key` behavior. Screenless keyboards receive the same controls;
a display is only an optional consumer of the public Agent-state event.

This is an independent interoperability project. It is not an OpenAI or Work
Louder SDK, firmware, or officially certified accessory.

## Features

- Six Agent Keys and six default command actions.
- Press and release events; ChatGPT interprets tap, double-tap, and hold gestures.
- A second vendor-defined USB HID interface, leaving normal ZMK input intact.
- Optional encrypted HID-over-GATT transport on the active Bluetooth profile.
- Independent protocol and six-Agent state for USB and every Bluetooth profile.
- Routing that follows the ZMK-selected output without disconnecting other hosts.
- Public state API and ZMK event for optional displays, LEDs, or other indicators.
- No dependency on a specific keyboard shield or display module.

## Quick integration

1. Add this repository as a project in your ZMK config's `config/west.yml`.
2. Apply the small official-ZMK compatibility patch described below.
3. Add `nxtkb-codex-micro-usb` to the central-half build snippets. Add
   `nxtkb-codex-micro-ble` if Bluetooth control is required. Add the optional
   `nxtkb-codex-micro-compat-identity` snippet only after reviewing the identity
   and distribution notice below.
4. Include the behavior and key IDs in the keymap:

```dts
#include <behaviors/codex_key.dtsi>
#include <dt-bindings/nxtkb/codex.h>
```

5. Bind any physical keys you want:

```dts
&codex_key CODEX_AGENT_0
&codex_key CODEX_APPROVE
&codex_key CODEX_MIC
&codex_key CODEX_SEND
```

See [the complete integration guide](docs/integration.md) and the buildable
[Corne example](examples/corne/config/corne.keymap).

## Snippets

| Snippet | Purpose |
| --- | --- |
| `nxtkb-codex-micro-usb` | Enables the module and its second USB HID interface. |
| `nxtkb-codex-micro-ble` | Adds Codex HID-over-GATT to the active ZMK Bluetooth profile. |
| `nxtkb-codex-micro-compat-identity` | Supplies the minimal USB and Bluetooth PnP identifiers currently used by ChatGPT discovery. |

Only the central half should enable the module. A split peripheral uses its
normal firmware.

## Required official-ZMK patch

The Corne example pins official ZMK
`4de313367288aad5aeaa1243890e40dc813f7008`; the patch below still applies to
that revision.

The legacy Zephyr USB HID option used for interrupt-OUT is global to all HID
instances. Consequently, ZMK's normal `HID_0` also needs a callback that drains
its OUT endpoint when the module enables the option for `HID_1`.

Official ZMK main did not provide this callback as of commit
`268b1b1e82150460f00fd701bcd08583d5c75d29` (2026-08-06). Until an equivalent
change is merged upstream, apply
the module's versioned West patch metadata:

```shell
west update
west patch -sm zmk-feature-codex-micro apply
```

The patch is stored at
[`zephyr/patches/zmk/zmk-usb-hid-interrupt-out.patch`](zephyr/patches/zmk/zmk-usb-hid-interrupt-out.patch)
and integrity-checked by `zephyr/patches.yml`. This is a focused compatibility
patch, not a ZMK fork, and is intended to be removed after upstream adoption.

## Discovery and device identity

The transport and protocol implementation are reusable, but the current
ChatGPT desktop app does not expose a documented third-party device discovery
or certification mechanism. Interoperability tests indicate that discovery
currently depends on USB VID/PID and Bluetooth PnP VID/PID. The optional
`nxtkb-codex-micro-compat-identity` snippet supplies only those four fields;
user-visible product, manufacturer, model, and Bluetooth names remain owned by
the keyboard configuration.

The compatibility identifiers are not evidence of OpenAI certification or an
identifier assignment to NXTKB or third-party keyboard makers. Integrators are
responsible for determining whether they may distribute firmware using them.
The undocumented discovery behavior may change in future ChatGPT releases.

The reusable user-config workflow is kept in this module because it applies the
module's version-matched ZMK compatibility patch before building. Keyboard
repositories should pin the workflow to a reviewed commit so the module,
patch, and build procedure advance together without requiring a ZMK fork.

The workflow's `patch_modules` input selects newline-separated patch sources
and defaults to this module. A keyboard with additional core patches can set:

```yaml
patch_modules: |
  zmk-feature-codex-micro
  zmk-vfx-sweep-pro-display
```

Each module's `zephyr/patches.yml` is applied after `west update` and before
building. Use `.` for patches owned by the caller module being checked out;
this lets a module's own CI test its pending patches without publishing them
first. Leave the input unset for the ordinary Codex-only integration.

## Protocol behavior

The implementation currently supports:

- 64-byte reports with Report ID 6;
- channel 2 RPC fragmentation and newline-based reassembly;
- `sys.version` and `device.status` responses;
- acknowledgements for `v.oai.thstatus` and `v.oai.rgbcfg`;
- Agent identifiers `AG00` through `AG05`;
- Fast, Approve, Decline, Split, Push-to-talk, and Send;
- parsing all six thread statuses into stable public Agent state.

The key IDs are defined in `include/dt-bindings/nxtkb/codex.h`. `ACT11` is the
secondary half of the physical Codex Micro microphone key; normal keymaps
should bind `CODEX_MIC` (`ACT10`) only.

## Bluetooth and endpoint switching

The BLE snippet raises the ATT MTU and buffers required for the report. After
first enabling it, forget the old keyboard on the host, clear that ZMK profile,
and pair again because hosts cache HID report maps.

Codex actions and state follow ZMK's selected USB output or Bluetooth profile.
Other connected hosts remain connected and retain independent sessions. If one
computer connects over USB and Bluetooth simultaneously, firmware cannot know
that both transports terminate at the same host; only the selected endpoint
receives key actions.

## License

MIT. See [LICENSE](LICENSE).
