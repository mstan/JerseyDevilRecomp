#!/usr/bin/env python3
"""Emit psxrecomp static-overlay capture records for Jersey Devil's PS-X EXEs.

Jersey Devil chains from the boot executable into secondary PS-X EXEs. They are
not overlays in the narrow loader-table sense, but they have the same runtime
property: different code images occupy RAM after boot. compile_overlays.py
already provides the content-validated static dispatch path we need.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
from pathlib import Path


PSX_HEADER_SIZE = 0x800
DEFAULT_EXES = ("MAIN.EXE", "PLAYER.EXE", "OPTIONS.EXE", "SAVE.EXE", "CREDITS.EXE")
DEFAULT_ANALYSIS = {
    "MAIN.EXE": Path("build-widescreen-next/main-analysis/analysis.json"),
    "PLAYER.EXE": Path("build-widescreen-next/player-analysis/analysis.json"),
}

# Runtime-observed callable prefixes that the static analyzer currently reports
# one or more instructions late. Keeping these as dispatch entries is still
# byte-validated by compile_overlays.py before native dispatch.
EXTRA_DISPATCH_ENTRIES = {
    "MAIN.EXE": {
        0x8002E2C0,
        0x80040618,
        0x80040738,
        0x80040EC8,
        0x80040F30,
    },
    "PLAYER.EXE": {
        0x80040568,
        0x80040688,
        0x80040998,
        0x80040A00,
    },
    "live-low-render-main": {
        0x80010218,
        0x80011658,
        0x80011B24,
    },
}


def u32le(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def scan_prologues(data: bytes, base: int) -> list[int]:
    out: list[int] = []
    for off in range(0, len(data) - 4, 4):
        word = u32le(data, off)
        if (word >> 16) == 0x27BD and (word & 0x8000):
            out.append(base + off)
    return out


def parse_psx_exe(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    if len(raw) < PSX_HEADER_SIZE or raw[:8] != b"PS-X EXE":
        raise ValueError(f"{path} is not a PS-X EXE")
    entry = u32le(raw, 0x10)
    load_addr = u32le(raw, 0x18)
    size = u32le(raw, 0x1C)
    if size <= 0:
        raise ValueError(f"{path} declares empty text")
    end = PSX_HEADER_SIZE + size
    if len(raw) < end:
        raise ValueError(
            f"{path} is truncated: header declares 0x{size:X} bytes, "
            f"file has 0x{max(0, len(raw) - PSX_HEADER_SIZE):X}"
        )
    return entry, load_addr, raw[PSX_HEADER_SIZE:end]


def analysis_entries(path: Path, base: int, size: int) -> list[int]:
    if not path.exists():
        return []
    doc = json.loads(path.read_text(encoding="utf-8"))
    entries = []
    for item in doc.get("functions", []):
        if item.get("is_data"):
            continue
        addr = int(item["addr"])
        if base <= addr < base + size:
            entries.append(addr)
    return entries


def record(path: Path, analysis_path: Path | None) -> dict[str, object]:
    entry, load_addr, body = parse_psx_exe(path)
    name = path.name.upper()
    seed_set = {entry, *scan_prologues(body, load_addr)}
    if analysis_path:
        seed_set.update(analysis_entries(analysis_path, load_addr, len(body)))
    seeds = sorted(seed_set)
    dispatch = sorted({
        *seeds,
        *(addr for addr in EXTRA_DISPATCH_ENTRIES.get(name, set())
          if load_addr <= addr < load_addr + len(body)),
    })
    encoded = [f"0x{addr:08X}" for addr in seeds]
    dispatch_encoded = [f"0x{addr:08X}" for addr in dispatch]
    return {
        "schema": "psxrecomp overlay capture v2",
        "producer_name": path.name,
        "load_addr": f"0x{load_addr:08X}",
        "size": len(body),
        "guard_bytes": 0,
        "bytes_b64": base64.b64encode(body).decode("ascii"),
        "executed_pcs": [],
        "dispatch_entry_pcs": dispatch_encoded,
        "static_dispatch_entry_pcs": dispatch_encoded,
        "function_entry_pcs": encoded,
        "seeds": encoded,
    }


def with_extra_entries(record: dict[str, object]) -> dict[str, object]:
    name = str(record.get("producer_name", ""))
    load_addr = int(str(record["load_addr"]), 0)
    size = int(record["size"])
    extras = sorted(
        addr for addr in EXTRA_DISPATCH_ENTRIES.get(name, set())
        if load_addr <= addr < load_addr + size
    )
    if not extras:
        return record

    encoded = [f"0x{addr:08X}" for addr in extras]
    for key in ("dispatch_entry_pcs", "static_dispatch_entry_pcs",
                "function_entry_pcs", "seeds"):
        values = set(str(v) for v in record.get(key, []))
        values.update(encoded)
        record[key] = sorted(values, key=lambda v: int(v, 0))
    return record


def preserved_records(out_path: Path, owned_names: set[str]) -> list[dict[str, object]]:
    if not out_path.exists():
        return []
    records = json.loads(out_path.read_text(encoding="utf-8"))
    if not isinstance(records, list):
        raise ValueError(f"{out_path} is not a capture record list")
    preserved = []
    for item in records:
        if not isinstance(item, dict):
            continue
        name = str(item.get("producer_name", ""))
        if name.upper() in owned_names:
            continue
        preserved.append(with_extra_entries(item))
    return preserved


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--exe-dir",
        type=Path,
        default=Path("build-widescreen-next/disc-exes"),
        help="directory containing extracted Jersey Devil PS-X EXEs",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("generated/static_exe_captures.json"),
        help="capture JSON path consumed by compile_overlays.py --static",
    )
    ap.add_argument(
        "--exe",
        action="append",
        default=[],
        help="specific EXE filename to include; repeatable. Defaults to gameplay/menu EXEs.",
    )
    ap.add_argument(
        "--analysis",
        action="append",
        default=[],
        metavar="EXE=PATH",
        help="analysis.json supplying additional function starts for one EXE",
    )
    args = ap.parse_args()

    analysis = dict(DEFAULT_ANALYSIS)
    for item in args.analysis:
        name, sep, value = item.partition("=")
        if not sep:
            raise ValueError("--analysis must be EXE=PATH")
        analysis[name.upper()] = Path(value)

    names = args.exe or list(DEFAULT_EXES)
    records = []
    owned_names = {name.upper() for name in names}
    for name in names:
        path = args.exe_dir / name
        records.append(with_extra_entries(record(path, analysis.get(name.upper()))))
    records.extend(preserved_records(args.out, owned_names))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    for item in records:
        print(
            f"{item['producer_name']}: load={item['load_addr']} "
            f"size=0x{item['size']:X} funcs={len(item['function_entry_pcs'])} "
            f"dispatch={len(item['dispatch_entry_pcs'])}"
        )
    print(f"wrote {len(records)} records to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
