#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 HackRF Edge contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject HackRF firmware whose required M4 interrupt handlers were lost."""

import argparse
import pathlib
import struct
import subprocess
import tempfile


REQUIRED = {
    "hard_fault_handler": 3,
    "mem_manage_handler": 4,
    "bus_fault_handler": 5,
    "usage_fault_handler": 6,
    "sys_tick_handler": 15,
    "dma_isr": 16 + 2,
    "usb0_isr": 16 + 8,
}


def symbols_from_nm(nm: pathlib.Path, elf: pathlib.Path):
    output = subprocess.check_output([str(nm), "-n", str(elf)], text=True)
    symbols = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            symbols[fields[2]] = (address, fields[1])
    return symbols


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objcopy", required=True, type=pathlib.Path)
    parser.add_argument("--elf", required=True, type=pathlib.Path)
    args = parser.parse_args()

    nm = args.objcopy.with_name("nm")
    symbols = symbols_from_nm(nm, args.elf)
    defaults = {
        name: symbols[name][0]
        for name in ("blocking_handler", "null_handler")
        if name in symbols
    }
    if len(defaults) != 2:
        raise SystemExit(f"{args.elf}: missing interrupt default symbols")

    with tempfile.TemporaryDirectory(prefix="hackrf-isr-") as directory:
        vectors_path = pathlib.Path(directory) / "text.bin"
        subprocess.check_call([
            str(args.objcopy),
            "--dump-section",
            f".text={vectors_path}",
            str(args.elf),
        ])
        # The HackRF linker scripts place the input .vectors section first in
        # the output .text section at address zero.
        vectors = vectors_path.read_bytes()

    for name, vector_index in REQUIRED.items():
        if name not in symbols:
            raise SystemExit(f"{args.elf}: required handler {name} is missing")
        address, binding = symbols[name]
        if binding != "T":
            raise SystemExit(
                f"{args.elf}: {name} has binding {binding}, expected strong global T"
            )
        if address in defaults.values():
            raise SystemExit(f"{args.elf}: {name} aliases an interrupt default")
        offset = vector_index * 4
        if offset + 4 > len(vectors):
            raise SystemExit(f"{args.elf}: vector table is too short for {name}")
        vector_address = struct.unpack_from("<I", vectors, offset)[0]
        if (vector_address & ~1) != address:
            raise SystemExit(
                f"{args.elf}: vector {vector_index} for {name} points to "
                f"0x{vector_address:08x}, expected 0x{address | 1:08x}"
            )

    print(f"Verified required interrupt handlers and vectors in {args.elf.name}")


if __name__ == "__main__":
    main()
