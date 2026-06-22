#!/usr/bin/env python3
"""
Generate a UF2 file that writes a CEC physical address config to the
last flash sector of the RP2040, without touching the firmware.

Usage:
    python3 make-pa-uf2.py 1.0.0.0     → pico-cec-pa-1.0.0.0.uf2
    python3 make-pa-uf2.py 2.0.0.0     → pico-cec-pa-2.0.0.0.uf2

Drop the generated UF2 onto the RPI-RP2 drive (hold BOOTSEL while
plugging in). The Pico reboots with the new physical address active.
The firmware default (if no config is written) is 1.0.0.0.
"""

import sys
import struct

FLASH_SIZE       = 2 * 1024 * 1024          # 2MB
FLASH_SECTOR     = 4096
FLASH_PAGE       = 256
CONFIG_OFFSET    = FLASH_SIZE - FLASH_SECTOR # last sector
CONFIG_MAGIC     = 0xCEC0CAFE
XIP_BASE         = 0x10000000

UF2_MAGIC0       = 0x0A324655  # "UF2\n"
UF2_MAGIC1       = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILYID = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56


def parse_pa(s):
    parts = s.strip().split('.')
    if len(parts) != 4:
        raise ValueError("expected X.X.X.X")
    nibbles = [int(p) for p in parts]
    if any(n < 0 or n > 15 for n in nibbles):
        raise ValueError("each nibble must be 0-15")
    return (nibbles[0] << 12) | (nibbles[1] << 8) | (nibbles[2] << 4) | nibbles[3]


def make_config_page(pa):
    """Build a 256-byte config page matching flash_config_t in main.c."""
    page = bytearray(FLASH_PAGE)
    struct.pack_into('<I', page, 0, CONFIG_MAGIC)  # magic
    struct.pack_into('<H', page, 4, pa)             # pa
    # rest is zero padding
    return bytes(page)


def make_uf2_block(target_addr, data, block_no, total_blocks):
    """Pack 256 bytes of data into a 512-byte UF2 block."""
    assert len(data) == 256
    # UF2 block layout (512 bytes total):
    #   32 bytes header (8 x uint32)
    #  476 bytes data area (256 payload + 220 padding)
    #    4 bytes end magic
    header = struct.pack('<IIIIIIII',
        UF2_MAGIC0,
        UF2_MAGIC1,
        UF2_FLAG_FAMILYID,
        target_addr,
        256,             # payload size
        block_no,
        total_blocks,
        RP2040_FAMILY_ID,
    )
    assert len(header) == 32
    payload_area = data + b'\x00' * (476 - 256)
    assert len(payload_area) == 476
    end = struct.pack('<I', UF2_MAGIC_END)
    block = header + payload_area + end
    assert len(block) == 512
    return block


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    pa_str = sys.argv[1]
    try:
        pa = parse_pa(pa_str)
    except ValueError as e:
        print(f"Error: {e}")
        sys.exit(1)

    a = (pa >> 12) & 0xF
    b = (pa >> 8)  & 0xF
    c = (pa >> 4)  & 0xF
    d =  pa        & 0xF

    config_page = make_config_page(pa)

    # We write one full sector (16 pages × 256 bytes). The first page
    # holds the config; the rest are 0xFF (erased flash state) to avoid
    # corrupting anything adjacent. But since we can only write via UF2
    # (which doesn't erase), we write the config page and leave the rest.
    # The firmware erases the whole sector before programming anyway.
    #
    # UF2 writes 256 bytes per block. We only need 1 block.
    target_addr = XIP_BASE + CONFIG_OFFSET
    total_blocks = 1

    uf2 = make_uf2_block(target_addr, config_page, 0, total_blocks)

    out = f"pico-cec-pa-{a}.{b}.{c}.{d}.uf2"
    with open(out, 'wb') as f:
        f.write(uf2)

    print(f"Written: {out}")
    print(f"  Physical address : {a}.{b}.{c}.{d} (0x{pa:04X})")
    print(f"  Flash offset     : 0x{CONFIG_OFFSET:06X}")
    print(f"  Target address   : 0x{target_addr:08X}")
    print()
    print("Hold BOOTSEL, plug in Pico, copy this file to RPI-RP2.")


if __name__ == '__main__':
    main()
