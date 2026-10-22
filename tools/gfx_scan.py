"""Scan a DAO archive for embedded Scaleform movies (CFX/GFX/FWS) and inspect them.

The game's packages are ERF V2 archives; their header is not the classic ASCII
one, so this scanner finds movies by their signatures instead. A movie is:
  FWS  - a plain SWF
  GFX  - an uncompressed SWF after an 8-byte header (signature, version, u32 length)
  CFX  - a zlib-compressed SWF after the same header

Usage:
  python tools/gfx_scan.py <erf> [--save dir] [--limit N] [--filter name]
"""

import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gfx_inspect import describe, swf_tags  # noqa: E402


def scan(erf_path: Path, save_dir: Path | None, limit: int):
    data = erf_path.read_bytes()
    found = []
    for sig in (b"CFX", b"GFX"):
        start = 0
        while True:
            pos = data.find(sig, start)
            if pos < 0:
                break
            start = pos + 3
            if pos + 8 > len(data):
                continue
            version = data[pos + 3]
            (uncompressed,) = struct.unpack_from("<I", data, pos + 4)
            if not (100 <= uncompressed <= 40 * 1024 * 1024):
                continue
            payload = data[pos + 8:]
            try:
                if sig == b"CFX":
                    dec = zlib.decompressobj()
                    swf = dec.decompress(payload, uncompressed)
                else:
                    swf = payload[:uncompressed]
            except zlib.error:
                continue
            if swf[0:3] != b"FWS" or len(swf) != uncompressed:
                continue
            found.append((sig.decode(), pos, version, uncompressed, swf))
    print(f"candidate movies: {len(found)}")
    for i, (sig, pos, version, size, swf) in enumerate(found[:limit]):
        try:
            parsed = swf_tags(swf)
            info = describe(parsed["tags"])
        except Exception as exc:  # noqa: BLE001 - a reference scan, report and continue
            print(f"[{i}] {sig} @{pos} size={size} unparsable: {exc}")
            continue
        fonts = [t for t in info if t["name"].startswith("DefineFont")]
        print(f"[{i}] {sig} @{pos} size={size} swf=SWF{parsed['version']} fps={parsed['frameRate'] >> 8} "
              f"tags={len(info)} fonts={[f['fontName'] for f in fonts if 'fontName' in f]}")
        if save_dir is not None and fonts:
            save_dir.mkdir(parents=True, exist_ok=True)
            out = save_dir / f"movie_{i}_{sig}.swf"
            out.write_bytes(swf)
            meta = save_dir / f"movie_{i}_{sig}.json"
            meta.write_text(json.dumps({"pos": pos, "sig": sig, "version": version,
                                        "tags": info[:80]}, indent=1))
            print(f"    saved {out} and {meta}")
    return found


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    erf = Path(sys.argv[1])
    save = None
    limit = 12
    args = sys.argv[2:]
    for i, arg in enumerate(args):
        if arg == "--save" and i + 1 < len(args):
            save = Path(args[i + 1])
        if arg == "--limit" and i + 1 < len(args):
            limit = int(args[i + 1])
    scan(erf, save, limit)
