"""Build the co-op mod's SWF screens from ActionScript 2 sources.

Adapted from DAOTurnBased's tools/movie.py (GPLv3, Whocares66-Dev), the same
pipeline the author documents in that mod's dev/UI.md: no Flash editor, FFDec
compiles the frame script into a plain SWF 8 shell that imports the game's own
fonts from gfxfontlib.swf.

    python tools\\movie.py                     build every screen into ui/
    python tools\\movie.py coop_eventlog        that one alone
    python tools\\movie.py --ffdec C:\\Modding\\ffdec_26.3.0

FFDec: https://github.com/jindrapetrik/jpexs-decompiler (run with `java -jar`).
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "ui"
FFDEC = Path(r"C:\Modding\ffdec_26.3.0")

SWF_VERSION = 8  # the game's movies are SWF 8 (their CFX header's version byte)
FPS = 30
FONT_LIBRARY = "gfxfontlib.swf"
FONTS = ["$TitleFont", "$BodyFont"]


@dataclass(frozen=True)
class Movie:
    """One of the mod's screens: its name, its script's folder, its stage."""

    name: str  # built as ui/<name>.swf and deployed beside the plugin
    source: Path  # holds frame_1/DoAction.as
    stage: tuple[int, int]  # the shell's, in pixels

    @property
    def out(self) -> Path:
        return OUT_DIR / f"{self.name}.swf"

    @property
    def script(self) -> Path:
        return self.source / "frame_1" / "DoAction.as"


EVENTLOG = Movie("coop_eventlog", REPO / "ui" / "eventlog", (1280, 720))
MOVIES = (EVENTLOG,)


class Bits:
    """Big-endian bit packing, as a SWF RECT wants it."""

    def __init__(self) -> None:
        self.bits: list[int] = []

    def put(self, value: int, count: int) -> None:
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def bytes(self) -> bytes:
        padded = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, padded[i:i + 8])), 2) for i in range(0, len(padded), 8))


def rect(width_px: int, height_px: int) -> bytes:
    xmax, ymax = width_px * 20, height_px * 20  # twips
    nbits = max(xmax.bit_length(), ymax.bit_length()) + 1  # signed
    b = Bits()
    b.put(nbits, 5)
    for v in (0, xmax, 0, ymax):
        b.put(v, nbits)
    return b.bytes()


def tag(code: int, body: bytes) -> bytes:
    if len(body) < 0x3F:
        return struct.pack("<H", (code << 6) | len(body)) + body
    return struct.pack("<HI", (code << 6) | 0x3F, len(body)) + body


def shell(stage: tuple[int, int]) -> bytes:
    """A one-frame SWF with the game's two fonts imported and an empty script."""
    tags = bytearray()
    tags += tag(69, struct.pack("<I", 0))  # FileAttributes: AS1/2, no network
    for index, font in enumerate(FONTS):
        # ImportAssets2, as the game's own movies import the font library.
        body = (
            FONT_LIBRARY.encode()
            + b"\0"
            + bytes([1, 0])
            + struct.pack("<HH", 1, 1 + index)
            + font.encode()
            + b"\0"
        )
        tags += tag(71, body)
    tags += tag(12, b"\0")  # DoAction: just ActionEnd, for FFDec to fill
    tags += tag(1, b"")  # ShowFrame
    tags += tag(0, b"")  # End
    rest = rect(*stage) + struct.pack("<HH", FPS << 8, 1) + bytes(tags)
    return b"FWS" + bytes([SWF_VERSION]) + struct.pack("<I", 8 + len(rest)) + rest


def missing(source: str, compiled: str) -> list[str]:
    """Functions and plugin call names the source has and the compiled script lacks."""
    wanted = re.findall(r"^function (\w+)", source, re.MULTILINE)
    wanted += re.findall(r'"((?:COOP)_\w+)"', source)
    return sorted({name for name in wanted if name not in compiled})


def ffdec(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["java", "-jar", str(root / "ffdec-cli.jar"), *args],
                          capture_output=True, text=True, check=False)


def build(root: Path, movie: Movie) -> int:
    if not movie.script.exists():
        print(f"no {movie.script}")
        return 1
    out = movie.out
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        work_dir = Path(work)
        base = work_dir / "shell.swf"
        base.write_bytes(shell(movie.stage))
        scripts = work_dir / "scripts"
        shutil.copytree(movie.source, scripts)
        made = work_dir / out.name
        result = ffdec(root, "-importScript", str(base), str(made), str(scripts))
        if result.returncode != 0 or not made.exists():
            print(result.stdout, result.stderr)
            print(f"FFDec could not compile {movie.name}")
            return 1
        # The script must come back out, whole: a compile error can leave it
        # empty or short and the build otherwise quiet.
        back = work_dir / "back"
        ffdec(root, "-export", "script", str(back), str(made))
        exported = list(back.rglob("DoAction.as"))
        text = exported[0].read_text(encoding="utf-8", errors="replace") if exported else ""
        lost = missing(movie.script.read_text(encoding="utf-8"), text)
        if not text or lost:
            print(result.stdout, result.stderr)
            print(f"the compiled {movie.name}'s script is not the source's: "
                  + (", ".join(lost) if text else "it has none"))
            return 1
        shutil.copyfile(made, out)
    print(f"built {out} ({out.stat().st_size} bytes)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffdec", type=Path, default=FFDEC, help="FFDec's folder")
    parser.add_argument("movies", nargs="*", metavar="movie",
                        help="which to build: " + ", ".join(m.name for m in MOVIES) + " (all)")
    args = parser.parse_args()
    known = {movie.name: movie for movie in MOVIES}
    unknown = [name for name in args.movies if name not in known]
    if unknown:
        print(f"no movie named {', '.join(unknown)}: there are {', '.join(known)}")
        return 1
    if not (args.ffdec / "ffdec-cli.jar").exists():
        print(f"no FFDec at {args.ffdec}; see this script's head")
        return 1
    failed = 0
    for movie in MOVIES:
        if not args.movies or movie.name in args.movies:
            failed |= build(args.ffdec, movie)
    return failed


if __name__ == "__main__":
    sys.exit(main())
