#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

"""Fetches FilePan's bundled fonts into app/src/main/assets/fonts/.

Every source is pinned by commit and SHA-256, and instancing uses the fontTools from the dev
shell (pinned by flake.lock), so the output is reproducible:

    nix develop --command utils/fonts/fetch-fonts.py

- Archivo (variable wdth 62..125, wght 100..900) and JetBrains Mono (variable wght 100..800) are
  copied unchanged from google/fonts. Both are under the SIL Open Font License 1.1.
- Material Symbols Rounded is 15 MB with four axes. FilePan pins GRAD at 0, opsz at 24 and wght
  at 400, keeping only FILL, and drops the glyph names (post format 3; ligatures don't need
  them), which brings it under 2 MB. Keeping wght as well would make it 4.3 MB. It's under the
  Apache License 2.0.
"""

import hashlib
import io
import pathlib
import sys
import tempfile
import urllib.parse
import urllib.request

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

GOOGLE_FONTS = "https://raw.githubusercontent.com/google/fonts/9862abc066c9b83bf84c3e8ed1fc2d8d48f9bbf0"
MATERIAL_ICONS = (
    "https://raw.githubusercontent.com/google/material-design-icons/"
    "10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7"
)

# (output name, source URL, source SHA-256, axis limits to pin, or None to copy unchanged)
FONTS = [
    (
        "Archivo.ttf",
        f"{GOOGLE_FONTS}/ofl/archivo/Archivo[wdth,wght].ttf",
        "0e094a7d3c7c4c25cf1310c4b30014f1dae9332220b1c2c88f4fa996f0b05053",
        None,
    ),
    (
        "JetBrainsMono.ttf",
        f"{GOOGLE_FONTS}/ofl/jetbrainsmono/JetBrainsMono[wght].ttf",
        "48715a42ec242c21e9f02692891e147d022299a52e48d5e413e1a942193ffeda",
        None,
    ),
    (
        "MaterialSymbolsRounded.ttf",
        f"{MATERIAL_ICONS}/variablefont/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf",
        "7e74e36533ccee12c2b81e75bfcf3ccf123d3bb7e7e46d65d94445f23db0c97c",
        {"GRAD": 0, "opsz": 24, "wght": 400},
    ),
]

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT / "app/src/main/assets/fonts"


def fetch(url: str, sha256: str) -> bytes:
    quoted = urllib.parse.quote(url, safe=":/")
    with urllib.request.urlopen(quoted) as response:
        data = response.read()
    actual = hashlib.sha256(data).hexdigest()
    if actual != sha256:
        sys.exit(f"{url}: SHA-256 is {actual}, expected {sha256}")
    return data


def instance(data: bytes, limits: dict) -> bytes:
    font = TTFont(io.BytesIO(data))
    font = instancer.instantiateVariableFont(font, limits)
    font["post"].formatType = 3.0
    if "DSIG" in font:
        del font["DSIG"]
    with tempfile.TemporaryFile() as file:
        font.save(file)
        file.seek(0)
        return file.read()


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, url, sha256, limits in FONTS:
        print(f"{name}: fetching {url}", flush=True)
        data = fetch(url, sha256)
        if limits is not None:
            print(f"{name}: instancing at {limits}", flush=True)
            data = instance(data, limits)
        (OUTPUT_DIR / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes, SHA-256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
