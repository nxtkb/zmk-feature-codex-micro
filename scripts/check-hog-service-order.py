#!/usr/bin/env python3
"""Check the real linked service order in a BLE-enabled Zephyr ELF (requires pyelftools)."""
import argparse
from elftools.elf.elffile import ELFFile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("elf")
args = parser.parse_args()
with open(args.elf, "rb") as stream:
    elf = ELFFile(stream)
    symbols = elf.get_section_by_name(".symtab")
    if symbols is None:
        raise SystemExit("An unstripped ELF with a symbol table is required")
    def address(name):
        matches = symbols.get_symbol_by_name(name)
        return matches[0]["st_value"] if matches else None
    keyboard = address("hog_svc")
    codex = address("nxtkb_codex_hog_svc")
    if codex is None:
        codex = address("codex_hog_svc")  # Also diagnoses old firmware builds.
    if keyboard is None or codex is None:
        raise SystemExit("Expected both the normal ZMK and Codex BLE HID services")
    print(f"normal keyboard service: 0x{keyboard:x}; Codex service: 0x{codex:x}")
    if keyboard >= codex:
        raise SystemExit("FAIL: vendor HID precedes the keyboard; first-service hosts cannot type")
    print("PASS: normal keyboard HID precedes Codex HID")
