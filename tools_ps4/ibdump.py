#!/usr/bin/env python3
"""Decode flight-recorder command streams (/data/GoldenEye64/flight/ib-N.bin,
written by the PS4 winsys with RADV_PS4_TRACE=1) into readable PM4.

  ibdump.py <ib-N.bin> [--mesa <mesa src>] [--all]

Prints every packet with register names (GFX7 register database from Mesa),
so the user-SGPR setup (SPI_SHADER_USER_DATA_*) before each draw/dispatch can
be checked against the fault address."""
import json
import os
import struct
import sys

MESA = os.path.expanduser("~/dolphinps4-build/src/mesa")
args = [a for a in sys.argv[1:]]
if "--mesa" in args:
    i = args.index("--mesa")
    MESA = args[i + 1]
    del args[i:i + 2]
show_all = "--all" in args
args = [a for a in args if a != "--all"]
path = args[0]

regs = {}
for f in ("gfx7.json",):
    db = json.load(open(os.path.join(MESA, "src/amd/registers", f)))
    for m in db["register_mappings"]:
        if m.get("map", {}).get("to") == "mm":
            regs[m["map"]["at"]] = m["name"]
pkt3 = {}
try:
    for k, v in json.load(open(os.path.join(MESA, "src/amd/registers/pkt3.json"))).items():
        pkt3[v] = k
except Exception:
    pass
OPS = {0x10: "NOP", 0x11: "SET_BASE", 0x12: "CLEAR_STATE", 0x15: "DISPATCH_DIRECT",
       0x16: "DISPATCH_INDIRECT", 0x27: "DRAW_INDEX_2", 0x2D: "DRAW_INDEX_AUTO",
       0x28: "CONTEXT_CONTROL", 0x2A: "INDEX_TYPE", 0x2F: "NUM_INSTANCES", 0x33: "INDIRECT_BUFFER_CONST",
       0x3F: "INDIRECT_BUFFER", 0x37: "WRITE_DATA", 0x3C: "WAIT_REG_MEM", 0x40: "COPY_DATA",
       0x46: "EVENT_WRITE", 0x47: "EVENT_WRITE_EOP", 0x49: "RELEASE_MEM", 0x50: "DMA_DATA",
       0x58: "ACQUIRE_MEM", 0x68: "SET_CONFIG_REG", 0x69: "SET_CONTEXT_REG", 0x76: "SET_SH_REG",
       0x79: "SET_UCONFIG_REG", 0x26: "INDEX_BASE", 0x13: "INDEX_BUFFER_SIZE", 0x34: "CP_DMA",
       0x41: "PFP_SYNC_ME", 0x44: "SURFACE_SYNC", 0x2E: "DRAW_INDEX_OFFSET_2", 0x35: "DRAW_INDEX_INDIRECT_MULTI",
       0x38: "DRAW_INDIRECT_MULTI", 0x32: "DRAW_INDIRECT", 0x3A: "DRAW_INDEX_MULTI_AUTO"}
for k, v in pkt3.items():
    OPS.setdefault(k, v)
BASES = {0x68: 0x2000, 0x69: 0xA000, 0x76: 0x2C00, 0x79: 0xC000}


def reg_name(byte_addr):
    return regs.get(byte_addr, "reg_%#x" % byte_addr)


data = open(path, "rb").read()
off = 0
n = 0
while off + 32 <= len(data):
    magic, depth, seq, va, num_dw, top = struct.unpack_from("<IIQQII", data, off)
    if magic != 0x43524249:
        print("bad record at %#x" % off)
        break
    off += 32
    dws = struct.unpack_from("<%dI" % num_dw, data, off)
    off += num_dw * 4
    print("=== IB seq %d top %d depth %d va %#x, %d dwords" % (seq, top, depth, va, num_dw))
    i = 0
    while i < len(dws):
        h = dws[i]
        if h >> 30 == 2:
            i += 1
            continue
        if h >> 30 != 3:
            print("  %5d: ?? %#010x" % (i, h))
            i += 1
            continue
        op = (h >> 8) & 0xFF
        body = ((h >> 16) & 0x3FFF) + 1
        b = dws[i + 1:i + 1 + body]
        name = OPS.get(op, "OP_%#x" % op)
        if op in BASES and b:
            start = BASES[op] + (b[0] & 0xFFFF) * 4
            for j, v in enumerate(b[1:]):
                print("  %5d: %-16s %-40s = %#010x" % (i, name, reg_name(start + 4 * j), v))
        elif op == 0x10 and not show_all:
            pass
        else:
            print("  %5d: %-16s %s" % (i, name, " ".join("%#x" % x for x in b[:8]) + (" ..." if len(b) > 8 else "")))
        i += 1 + body
    n += 1
print("%d IB records" % n)
