#!/usr/bin/env python3
"""Builds data/earth.bin, the Earth pictures the TRACE module draws its map from
(and the door its ANSI map), from NASA's public-domain imagery:

  Blue Marble Next Generation (topography and bathymetry, December 2004)
    https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73909/world.topo.bathy.200412.3x5400x2700.jpg
  Black Marble 2016 (city lights)
    https://eoimages.gsfc.nasa.gov/images/imagerecords/144000/144898/BlackMarble_2016_01deg.jpg

Both equirectangular, shrunk to 1024x512 and stored as JPEGs:
  "EAR1", u32 day length, u32 night length, day JPEG (colour), night JPEG (grey)

Usage: make_earth.py <day.jpg> <night.jpg> [out]
"""
import io
import struct
import sys

from PIL import Image, ImageEnhance

W, H = 1024, 512


def jpeg(img, quality):
    b = io.BytesIO()
    img.save(b, "JPEG", quality=quality, optimize=True)
    return b.getvalue()


def main():
    day_path, night_path = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else "data/earth.bin"
    day = Image.open(day_path).convert("RGB").resize((W, H), Image.LANCZOS)
    day = ImageEnhance.Color(day).enhance(1.12)
    night = Image.open(night_path).convert("L").resize((W, H), Image.LANCZOS)
    # Keep only the lights: Black Marble also records moonlit snow and ice as a
    # dull grey, which would glow like cities. Cut that floor off, then curve.
    night = night.point(lambda v: 0 if v < 70 else int(255 * min(1.0, (v - 70) / 150) ** 1.3))
    d, n = jpeg(day, 86), jpeg(night, 82)
    with open(out, "wb") as f:
        f.write(b"EAR1" + struct.pack("<II", len(d), len(n)) + d + n)
    print(f"{out}: day {len(d)} bytes, night {len(n)} bytes")


if __name__ == "__main__":
    main()
