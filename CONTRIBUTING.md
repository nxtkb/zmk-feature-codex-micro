# Contributing

Contributions that improve keyboard independence, ZMK compatibility, protocol
robustness, documentation, or test coverage are welcome.

## Principles

- Keep keyboard matrices, layers, displays, LEDs, and pointing devices outside
  the transport/protocol core.
- Keep the `nxtkb` namespace for public symbols to avoid collisions with ZMK
  and other modules; the namespace does not restrict supported hardware.
- Do not add third-party USB or Bluetooth identifiers without proof that the
  project is authorized to distribute them.
- Prefer a small upstream ZMK change over maintaining a ZMK fork.
- Preserve normal ZMK keyboard, consumer, pointing, Studio, USB, and Bluetooth
  behavior.

## Validation

Before opening a change:

1. Build the Corne example with `nxtkb-codex-micro-usb`.
2. Build it again with both USB and BLE snippets.
3. Build at least one split peripheral without the module when changing shared
   ZMK or split behavior.
4. Run `git diff --check`.
5. For protocol changes, test against the current ChatGPT desktop app and note
   its version and host operating system in the pull request.

Do not attach or publish UF2 files built with a local interoperability identity.
