#!/usr/bin/env python3
"""Show (and optionally apply) the PL base addresses from a fuzzsight block design.

The headers in include/ are verbatim copies of fuzzsight's src/*.h. The one
thing that cannot be copied is the base address: upstream ships a placeholder
(0x80000000) behind an #ifndef with a #pragma warning telling you to override
it for your own Vivado project.

Usage:
    ./scripts/bd_addrs.py ~/vivproj/fuzzsight/bd/fuzzsight_tri.tcl
    ./scripts/bd_addrs.py ~/vivproj/fuzzsight/bd/fuzzsight_tri.tcl --apply
"""

import argparse
import pathlib
import re
import shlex
import sys
import xml.etree.ElementTree as ET
import zipfile

# Block-design instance name -> the macro it feeds and the header holding it.
PERIPHERALS = {
    "axi_dma": ("DMA_BASE", "bitmap_dma.h"),
    "bitmap_tri_reader_br_0": ("BITMAP_READER_BASE", "bitmap_dma.h"),
    "decoder_stats_lut": ("DECODER_STATS_BASE_ETM", "decoder_stats.h"),
    "edge_extractor": ("EDGE_EXTRACTOR_BASE", "edge_extractor.h"),
    "decoder_axi_interface_0": ("DECODER_AXI_BASE", "decoder_axi.h"),
}

PLACEHOLDER = 0x80000000


# --------------------------------------------------------------------------
# Reading the block design
# --------------------------------------------------------------------------


def seg_to_instance(seg):
    """Turn a get_bd_addr_segs path into the flat instance name the .hwh uses."""
    parts = seg.strip().split("/")
    return "_".join(parts[:-2]) if len(parts) >= 3 else None


def collect_tcl(text):
    """Map instance name -> base address from assign_bd_address calls."""
    found = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith("assign_bd_address"):
            continue  # skips comments and exclude_bd_addr_seg alike
        try:
            toks = shlex.split(line)
        except ValueError:
            continue

        offset = None
        for i, t in enumerate(toks):
            if t == "-offset" and i + 1 < len(toks):
                offset = toks[i + 1]
                break

        seg_m = re.search(r"get_bd_addr_segs\s+([^\]\s]+)", line)
        if not (offset and seg_m):
            continue

        # Only the PS master's view defines where software finds a peripheral;
        # the DMA's own S2MM segments into DDR live in a different space.
        space_m = re.search(r"get_bd_addr_spaces\s+([^\]\s]+)", line)
        if space_m and not space_m.group(1).endswith("/Data"):
            continue

        inst = seg_to_instance(seg_m.group(1))
        if inst not in PERIPHERALS:
            continue
        base = int(offset, 16)
        if found.get(inst, base) != base:
            sys.exit(f"{inst}: conflicting addresses {found[inst]:#x} vs {base:#x}")
        found[inst] = base
    return found


def collect_hwh(path):
    """Map instance name -> base address from a built .xsa/.hwh."""
    path = pathlib.Path(path)
    if path.suffix == ".xsa":
        with zipfile.ZipFile(path) as z:
            # An .xsa also ships per-interconnect .hwh files, the top-level one
            # is named after the block design and has the full map.
            top = f"{path.stem}.hwh"
            if top not in z.namelist():
                cands = [n for n in z.namelist() if n.endswith(".hwh")]
                if len(cands) != 1:
                    sys.exit(f"cannot pick a top-level .hwh in {path}: {cands}")
                top = cands[0]
            text = z.read(top).decode()
    else:
        text = path.read_text()

    found = {}
    for m in ET.fromstring(text).iter("MEMRANGE"):
        inst = m.get("INSTANCE")
        if inst in PERIPHERALS and m.get("BASEVALUE") is not None:
            found[inst] = int(m.get("BASEVALUE"), 16)
    return found


# --------------------------------------------------------------------------
# Reading and rewriting the headers
# --------------------------------------------------------------------------


def macro_pattern(macro):
    return re.compile(
        rf"^([ \t]*#define[ \t]+{re.escape(macro)}[ \t]+)(0x[0-9A-Fa-f]+)", re.MULTILINE
    )


def current_value(header_text, macro):
    m = macro_pattern(macro).search(header_text)
    return int(m.group(2), 16) if m else None


def apply_to_header(header_text, macro, value):
    """Set the macro's literal, and any hex in the #pragma message beside it."""
    new_text, n = macro_pattern(macro).subn(rf"\g<1>0x{value:08X}", header_text)
    if not n:
        return header_text, 0

    def fix_pragma(m):
        return re.sub(r"0x[0-9A-Fa-f]{8}", f"0x{value:08X}", m.group(0))

    new_text = re.sub(
        rf"^[ \t]*#pragma message\([^\n]*{re.escape(macro)}[^\n]*\)",
        fix_pragma,
        new_text,
        flags=re.MULTILINE,
    )
    return new_text, n


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "bd",
        metavar="BD",
        help="block design .tcl (preferred), or the .xsa/.hwh from a build",
    )
    ap.add_argument(
        "-I",
        "--include-dir",
        default="include",
        help="directory holding the copied fuzzsight headers",
    )
    ap.add_argument(
        "--apply",
        action="store_true",
        help="rewrite the header literals instead of only reporting",
    )
    args = ap.parse_args()

    src = pathlib.Path(args.bd).expanduser()
    if not src.exists():
        sys.exit(f"no such block design source: {src}")
    design = src.stem

    found = collect_tcl(src.read_text()) if src.suffix == ".tcl" else collect_hwh(src)

    missing = [i for i in PERIPHERALS if i not in found]
    if missing:
        sys.exit(
            f"{design}: peripherals absent from the address map: {', '.join(missing)}"
        )

    inc = pathlib.Path(args.include_dir)
    print(f"block design: {design}  ({src})\n")
    print(f"  {'MACRO':<24} {'BLOCK DESIGN':<14} {'HEADER':<14} STATUS")

    edits, problems = {}, 0
    for inst, (macro, header) in PERIPHERALS.items():
        want = found[inst]
        path = inc / header
        text = path.read_text() if path.exists() else ""
        have = current_value(text, macro) if text else None

        if have is None:
            status, cur = "MACRO NOT FOUND", "-"
            problems += 1
        elif have == want:
            status, cur = "ok", f"0x{have:08X}"
        elif have == PLACEHOLDER:
            status, cur = "PLACEHOLDER -- not overridden", f"0x{have:08X}"
            problems += 1
        else:
            status, cur = "MISMATCH", f"0x{have:08X}"
            problems += 1

        print(f"  {macro:<24} 0x{want:08X}     {cur:<14} {status}")
        if have != want and text:
            edits.setdefault(path, text)
            edits[path], _ = apply_to_header(edits[path], macro, want)

    if not args.apply:
        print()
        if problems:
            print(f"[!] {problems} address(es) disagree with the block design.")
            print("    Check the values above, then re-run with --apply to write them.")
        else:
            print("[+] all addresses match the block design.")
        return 1 if problems else 0

    if not edits:
        print("\n[+] nothing to change.")
        return 0
    for path, text in edits.items():
        path.write_text(text)
        print(f"\n[+] updated {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
