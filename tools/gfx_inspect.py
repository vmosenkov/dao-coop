"""Extract a DAO .gfx movie from an ERF archive and inspect its SWF tags.

The game stores its Scaleform movies in ERF archives (packages/core/...).
A movie is a Flash movie: FWS (plain SWF), GFX (uncompressed SWF after an
8-byte header) or CFX (zlib-compressed SWF after the same header).

Usage:
  python tools/gfx_inspect.py list  <erf> [name-filter]
  python tools/gfx_inspect.py dump  <erf> <resource-name> <out.gfx>
  python tools/gfx_inspect.py tags  <gfx-or-swf> [out.json]
"""

import json
import struct
import sys
import zlib
from pathlib import Path


def read_erf(erf_path: Path):
    data = erf_path.read_bytes()
    if data[0:4] != b"ERF ":
        raise SystemExit(f"not an ERF: {erf_path}")
    languages, loc_size, entry_count, loc_off, key_off, res_off = struct.unpack_from("<6I", data, 8)
    entries = []
    for i in range(entry_count):
        base = res_off + i * 24
        resref = data[base:base + 16].split(b"\0", 1)[0].decode("latin-1")
        res_id, res_type, unused, off, size = struct.unpack_from("<IHHII", data, base + 16)
        entries.append((resref, res_type, off, size))
    return data, entries


def unpack_movie(raw: bytes) -> bytes:
    sig = raw[0:3]
    if sig == b"FWS":
        return raw
    if sig in (b"GFX", b"CFX"):
        (uncompressed,) = struct.unpack_from("<I", raw, 4)
        payload = raw[8:]
        if sig == b"CFX":
            payload = zlib.decompress(payload)
        if len(payload) != uncompressed:
            raise SystemExit(f"bad unpacked length {len(payload)} != {uncompressed}")
        return payload
    raise SystemExit(f"unknown movie signature {sig!r}")


def swf_tags(swf: bytes):
    if swf[0:3] != b"FWS":
        raise SystemExit("expected FWS after unpacking")
    version = swf[3]
    (file_length,) = struct.unpack_from("<I", swf, 4)
    pos = 8
    nbits = swf[pos] >> 3
    rect_bytes = (5 + 4 * nbits + 7) // 8
    pos += rect_bytes
    frame_rate, frame_count = struct.unpack_from("<HH", swf, pos)
    pos += 4
    tags = []
    while pos + 2 <= min(len(swf), file_length):
        code_len = struct.unpack_from("<H", swf, pos)[0]
        pos += 2
        code = code_len >> 6
        length = code_len & 0x3F
        if length == 0x3F:
            (length,) = struct.unpack_from("<I", swf, pos)
            pos += 4
        payload = swf[pos:pos + length]
        tags.append((code, payload))
        pos += length
        if code == 0:
            break
    return {"version": version, "fileLength": file_length, "frameRate": frame_rate,
            "frameCount": frame_count, "tags": tags}


FONT_TAGS = {10: "DefineFont", 48: "DefineFont2", 62: "DefineFontInfo", 75: "DefineFont3"}


def describe(tags):
    out = []
    for code, payload in tags:
        name = FONT_TAGS.get(code)
        if name:
            (font_id,) = struct.unpack_from("<H", payload, 0)
            info = {"tag": code, "name": name, "fontId": font_id, "bytes": len(payload)}
            if code in (48, 75):
                # DefineFont2/3: FontID, flags (bit4 hasLayout, bit3 shiftJIS, bit2 unicode, bit1 wideOffsets, bit0 wideCodes)
                flags = payload[2]
                pos = 3
                name_len = payload[pos]
                pos += 1
                info["fontName"] = payload[pos:pos + name_len].decode("latin-1")
                pos += name_len
                (num_glyphs,) = struct.unpack_from("<H", payload, pos)
                pos += 2
                info["numGlyphs"] = num_glyphs
                wide_offsets = bool(flags & 0x08)
                wide_codes = bool(flags & 0x02)
                if wide_offsets:
                    offsets = struct.unpack_from(f"<{num_glyphs + 1}I", payload, pos)
                    pos += 4 * (num_glyphs + 1)
                else:
                    offsets = struct.unpack_from(f"<{num_glyphs + 1}H", payload, pos)
                    pos += 2 * (num_glyphs + 1)
                info["offsetTable"] = list(offsets[:4])
                codes = []
                for _ in range(num_glyphs):
                    if wide_codes:
                        (c,) = struct.unpack_from("<H", payload, pos)
                        pos += 2
                    else:
                        c = payload[pos]
                        pos += 1
                    codes.append(c)
                info["codes"] = codes
                info["hasLayout"] = bool(flags & 0x10)
                if info["hasLayout"]:
                    # after glyph shapes: ascent, descent, leading, then advance table
                    # the glyph shapes start after the offsets (which we do not parse)
                    info["layoutBytesAfter"] = len(payload)
            out.append(info)
        elif code in (11, 37):
            (char_id,) = struct.unpack_from("<H", payload, 0)
            out.append({"tag": code, "name": "DefineText" if code == 11 else "DefineEditText",
                        "charId": char_id, "bytes": len(payload)})
        else:
            out.append({"tag": code, "name": f"Tag{code}", "bytes": len(payload)})
    return out



if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    if command == "list":
        erf = Path(sys.argv[2])
        data, entries = read_erf(erf)
        filt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for resref, res_type, off, size in entries:
            if filt in resref.lower():
                print(f"{resref:24s} type={res_type:3d} size={size}")
    elif command == "dump":
        erf = Path(sys.argv[2])
        want = sys.argv[3].lower()
        out = Path(sys.argv[4])
        data, entries = read_erf(erf)
        for resref, res_type, off, size in entries:
            if resref.lower() == want:
                out.write_bytes(data[off:off + size])
                print(f"wrote {out} ({size} bytes)")
                break
        else:
            raise SystemExit(f"{want} not found")
    elif command == "tags":
        raw = Path(sys.argv[2]).read_bytes()
        swf = unpack_movie(raw)
        parsed = swf_tags(swf)
        info = describe(parsed["tags"])
        summary = {"version": parsed["version"], "frameRate": parsed["frameRate"],
                   "frameCount": parsed["frameCount"], "tags": info}
        print(json.dumps(summary, indent=1)[:4000])
        if len(sys.argv) > 3:
            Path(sys.argv[3]).write_text(json.dumps(summary, indent=1))
            print(f"wrote {sys.argv[3]}")
    else:
        raise SystemExit(__doc__)

