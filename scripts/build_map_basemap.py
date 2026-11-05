#!/usr/bin/env python3
"""Pack Natural Earth outlines into SolarOS map layers.

Natural Earth is public domain, so a world map costs no tile service, no
account and no key. Nothing is compiled into the firmware: each layer is
written as its own file, to be copied to the card and loaded like any other
map. A first run of the map app fetches them; a reader who wants a different
world replaces the files.

One file per layer rather than one file for all of them, because a layer is
the unit the map loads, orders and unloads. Concatenating them produced a
file whose header described only the first, so only the first was ever read.

Lakes come from a second dataset because the land polygons carry almost no
holes: at 1:110m one hole exists in the whole world, so a lake drawn from
the land outline alone would be land. Lakes are classed as water and load
after land, and the renderer draws layers in order, so a lake lands on top
of the continent that holds it.

    scripts/build_map_basemap.py --level 110m --out-dir layers/
    scripts/build_map_basemap.py --level 50m --out-dir layers/
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import struct
import sys
import urllib.request

SOURCE = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
          "master/geojson/ne_{level}_{feature}.geojson")
LEVELS = ("110m", "50m", "10m")
# Province lines are the United States alone at 1:110m and a beta dataset at
# 1:10m, so they are taken from the one scale in between.
PROVINCE_LEVEL = "50m"
MAGIC = b"SOMB"
VERSION = 2
E7 = 10000000.0
CLASS_SHIFT = 24
RING_OPEN = 0x80000000
CLASS_LAND = 0
CLASS_WATER = 1
CLASS_BOUNDARY = 5
CLASS_REGION = 6


def fetch(level: str, feature: str, cache: Path) -> dict:
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / f"ne_{level}_{feature}.geojson"
    if not path.exists():
        url = SOURCE.format(level=level, feature=feature)
        print(f"fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=120) as response:
            path.write_bytes(response.read())
    return json.loads(path.read_text())


def rings(collection: dict) -> list[list[tuple[int, int]]]:
    """Outer rings only: a hole is drawn from the lakes dataset instead."""
    out = []
    for feature in collection["features"]:
        geometry = feature["geometry"]
        polygons = (geometry["coordinates"]
                    if geometry["type"] == "MultiPolygon"
                    else [geometry["coordinates"]])
        for polygon in polygons:
            if not polygon:
                continue
            ring = [(round(lat * E7), round(lon * E7)) for lon, lat in polygon[0]]
            # A closed ring repeats its first point; the renderer closes it.
            if len(ring) > 1 and ring[0] == ring[-1]:
                ring.pop()
            if len(ring) >= 3:
                out.append(ring)
    return out


def lines(collection: dict) -> list[list[tuple[int, int]]]:
    """Open lines, for things that bound rather than enclose."""
    out = []
    for feature in collection["features"]:
        geometry = feature["geometry"]
        strings = (geometry["coordinates"]
                   if geometry["type"] == "MultiLineString"
                   else [geometry["coordinates"]])
        for string in strings:
            line = [(round(lat * E7), round(lon * E7)) for lon, lat in string]
            if len(line) >= 2:
                out.append(line)
    return out


def simplify(ring: list[tuple[int, int]], tolerance: int) -> list[tuple[int, int]]:
    """Drop vertices closer than the tolerance, in 1e7 degree units."""
    if tolerance <= 0 or len(ring) < 4:
        return ring
    kept = [ring[0]]
    for point in ring[1:]:
        last = kept[-1]
        if abs(point[0] - last[0]) + abs(point[1] - last[1]) >= tolerance:
            kept.append(point)
    return kept if len(kept) >= 3 else ring


def resolution(ring_list: list[tuple[int, list[tuple[int, int]]]]) -> int:
    """Median distance between neighbouring vertices, in hundreds of metres.

    A renderer zoomed in past this is looking at a shape the source never
    claimed to place that precisely, so it can stop believing the outline.
    """
    spans = []
    for _, _, ring in ring_list:
        for (lat_a, lon_a), (lat_b, lon_b) in zip(ring, ring[1:]):
            d_lat = (lat_b - lat_a) / E7 * 111320.0
            d_lon = ((lon_b - lon_a) / E7 * 111320.0 *
                     math.cos(math.radians((lat_a + lat_b) / 2.0 / E7)))
            span = math.hypot(d_lat, d_lon)
            if span > 0.0:
                spans.append(span)
    if not spans:
        return 0
    return min(65535, round(statistics.median(spans) / 100.0))


def pack(ring_list: list[tuple[int, bool, list[tuple[int, int]]]]) -> bytes:
    points = sum(len(ring) for _, _, ring in ring_list)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHII", VERSION, resolution(ring_list),
                       len(ring_list), points)
    for klass, is_open, ring in ring_list:
        word = len(ring) | (klass << CLASS_SHIFT)
        out += struct.pack("<I", word | (RING_OPEN if is_open else 0))
    for _, _, ring in ring_list:
        lats = [lat for lat, _ in ring]
        lons = [lon for _, lon in ring]
        out += struct.pack("<iiii", min(lats), max(lats), min(lons), max(lons))
    for _, _, ring in ring_list:
        for lat, lon in ring:
            out += struct.pack("<ii", lat, lon)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--level", choices=LEVELS, default="110m")
    parser.add_argument("--tolerance", type=int, default=0,
                        help="drop vertices closer than this, in 1e7 degrees")
    parser.add_argument("--out-dir", type=Path, required=True,
                        help="write one packed layer per file into this "
                             "directory")
    parser.add_argument("--no-lakes", dest="lakes", action="store_false",
                        help="pack land only, as releases before lakes did")
    parser.add_argument("--no-borders", dest="borders", action="store_false",
                        help="pack without country boundaries")
    parser.add_argument("--no-provinces", dest="provinces",
                        action="store_false",
                        help="pack without state and province boundaries")
    parser.add_argument("--cache", type=Path,
                        default=Path(".cache/natural-earth"))
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    features = [(args.level, "land", CLASS_LAND, False, "land")]
    if args.lakes:
        features.append((args.level, "lakes", CLASS_WATER, False, "lakes"))
    if args.borders:
        features.append((args.level, "admin_0_boundary_lines_land",
                         CLASS_BOUNDARY, True, "borders"))
    if args.provinces:
        features.append((PROVINCE_LEVEL, "admin_1_states_provinces_lines",
                         CLASS_REGION, True, "regions"))

    parts: list[tuple[str, bytes]] = []
    for level, feature, klass, is_open, label in features:
        shapes = (lines(fetch(level, feature, args.cache)) if is_open
                  else rings(fetch(level, feature, args.cache)))
        packed = [(klass, is_open, simplify(shape, args.tolerance))
                  for shape in shapes]
        data = pack(packed)
        print(f"{level} {label}: {len(packed)} shapes, "
              f"{sum(len(ring) for _, _, ring in packed)} points, "
              f"{len(data)} bytes", file=sys.stderr)
        parts.append((label, data))

    for label, data in parts:
        path = args.out_dir / f"{label}.bin"
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)", file=sys.stderr)

    total = sum(len(data) for _, data in parts)
    print(f"{len(parts)} layers, {total} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
