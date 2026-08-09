# SPDX-License-Identifier: MIT

if(CONFIG_NXTKB_CODEX_MICRO)
  zephyr_include_directories(include)
  target_sources(app PRIVATE
    src/protocol.c
    src/state.c
    src/transport.c
    src/usb_hid.c
    src/behaviors/behavior_codex_key.c
  )

  if(CONFIG_NXTKB_CODEX_MICRO_BLE)
    target_sources(app PRIVATE src/ble_hid.c)
  endif()
endif()
