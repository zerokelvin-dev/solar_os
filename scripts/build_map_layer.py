#!/usr/bin/env python3
"""Pack an OpenStreetMap area into a SolarOS map layer.

Natural Earth covers the world but stops being useful below a continent.
This fetches a bounding box from the Overpass API, which needs no account
and no key, and packs it into the layer format `map load` reads.

OpenStreetMap data is ODbL: whatever you publish from it owes attribution
to OpenStreetMap contributors. Overpass is a free shared service, so keep
the boxes small and the queries few.

    scripts/build_map_layer.py --bbox 43.58,-79.64,43.86,-79.12 \\
        --features coastline,water,major-roads --output toronto.bin

Then put the file on the card and, on the device:

    map load /sdcard/toronto.bin
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ENDPOINT = "https://overpass-api.de/api/interpreter"
USER_AGENT = "SolarOS-map-layer/1 (+https://github.com/zerokelvin-dev/solar_os)"
MAGIC = b"SOMB"
VERSION = 2
E7 = 10000000.0
RING_OPEN = 0x80000000
CLASS_SHIFT = 24

CLASSES = {"land": 0, "water": 1, "road": 2, "rail": 3, "building": 4,
           "boundary": 5}

# Each feature is a list of Overpass filters over ways, and the class the
# rings it returns are tagged with.
FEATURES = {
    "coastline": (["natural=coastline"], "land"),
    "water": (["natural=water", "waterway=riverbank"], "water"),
    "rivers": (["waterway=river"], "water"),
    "major-roads": (["highway~^(motorway|trunk|primary)$"], "road"),
    "roads": (["highway~^(motorway|trunk|primary|secondary|tertiary)$"], "road"),
    "rail": (["railway=rail"], "rail"),
    "buildings": (["building"], "building"),
    "boundary": (["boundary=administrative", "admin_level~^(2|4)$"], "boundary"),
}


def overpass_filter(spec: str) -> str:
    """A tag spec such as natural=water becomes ["natural"="water"]."""
    if "~" in spec:
        key, value = spec.split("~", 1)
        return f'["{key}"~"{value}"]'
    if "=" in spec:
        key, value = spec.split("=", 1)
        return f'["{key}"="{value}"]'
    return f'["{spec}"]'



def build_query(bbox: str, features: list[str], timeout: int) -> str:
    """One query per feature, so each answer's class is known."""
    south, west, north, east = [part.strip() for part in bbox.split(",")]
    box = f"({south},{west},{north},{east})"
    specs, _ = FEATURES[features]
    clauses = "".join(overpass_filter(spec) for spec in specs)
    return (f"[out:json][timeout:{timeout}];\n"
            f"way{clauses}{box};\nout geom;\n")


def fetch(query: str, retries: int = 3) -> dict:
    data = urllib.parse.urlencode({"data": query}).encode()
    request = urllib.request.Request(ENDPOINT, data=data,
                                     headers={"User-Agent": USER_AGENT})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                return json.loads(response.read())
        except urllib.error.HTTPError as error:
            # Overpass answers 429 and 504 when it is busy; it asks callers
            # to back off rather than hammer it.
            if error.code not in (429, 504) or attempt == retries - 1:
                raise
            delay = 5 * (attempt + 1)
            print(f"overpass busy ({error.code}), retrying in {delay}s",
                  file=sys.stderr)
            time.sleep(delay)
    raise SystemExit("overpass did not answer")


def rings(payload: dict, tolerance: int,
          klass: int) -> list[tuple[list[tuple[int, int]], bool, int]]:
    out = []
    for element in payload.get("elements", []):
        geometry = element.get("geometry")
        if not geometry:
            continue
        ring = [(round(node["lat"] * E7), round(node["lon"] * E7))
                for node in geometry]
        closed = len(ring) > 2 and ring[0] == ring[-1]
        if closed:
            ring.pop()
        ring = simplify(ring, tolerance)
        if len(ring) >= 2:
            out.append((ring, not closed, klass))
    return out


def simplify(ring: list[tuple[int, int]], tolerance: int) -> list[tuple[int, int]]:
    """Drop vertices closer than the tolerance, in 1e7 degree units."""
    if tolerance <= 0 or len(ring) < 3:
        return ring
    kept = [ring[0]]
    for point in ring[1:]:
        last = kept[-1]
        if abs(point[0] - last[0]) + abs(point[1] - last[1]) >= tolerance:
            kept.append(point)
    if len(kept) < 2:
        return ring
    return kept


def resolution(entries: list[tuple[list[tuple[int, int]], bool, int]]) -> int:
    """Median distance between neighbouring vertices, in hundreds of metres.

    A renderer zoomed in past this knows the outline is no longer placed
    that precisely and can stop drawing it. Survey-grade OSM geometry
    rounds to zero here, which means no limit.
    """
    spans = []
    for ring, _, _ in entries:
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


def pack(entries: list[tuple[list[tuple[int, int]], bool, int]]) -> bytes:
    points = sum(len(ring) for ring, _, _ in entries)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHII", VERSION, resolution(entries),
                       len(entries), points)
    for ring, is_open, klass in entries:
        if len(ring) > 0xFFFFFF:
            raise SystemExit("a ring longer than 16 million points")
        word = len(ring) | (klass << CLASS_SHIFT)
        out += struct.pack("<I", word | (RING_OPEN if is_open else 0))
    for ring, _, _ in entries:
        lats = [lat for lat, _ in ring]
        lons = [lon for _, lon in ring]
        out += struct.pack("<iiii", min(lats), max(lats), min(lons), max(lons))
    for ring, _, _ in entries:
        for lat, lon in ring:
            out += struct.pack("<ii", lat, lon)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bbox", required=True,
                        help="south,west,north,east in degrees")
    parser.add_argument("--features", default="coastline,water,major-roads",
                        help=f"comma separated: {', '.join(sorted(FEATURES))}")
    parser.add_argument("--tolerance", type=int, default=300,
                        help="drop vertices closer than this, in 1e7 degrees "
                             "(300 is about 3 metres)")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--query-only", action="store_true",
                        help="print the Overpass query and stop")
    args = parser.parse_args()

    features = [name.strip() for name in args.features.split(",") if name.strip()]
    for feature in features:
        if feature not in FEATURES:
            raise SystemExit(f"unknown feature {feature!r}; "
                             f"choose from {', '.join(sorted(FEATURES))}")
    if args.query_only:
        for feature in features:
            print(build_query(args.bbox, feature, args.timeout))
        return 0

    entries = []
    for feature in features:
        print(f"querying overpass for {feature} in {args.bbox}", file=sys.stderr)
        klass = CLASSES[FEATURES[feature][1]]
        entries += rings(fetch(build_query(args.bbox, feature, args.timeout)),
                         args.tolerance, klass)
    if not entries:
        raise SystemExit("no geometry in that box for those features")
    data = pack(entries)
    args.output.write_bytes(data)
    points = sum(len(ring) for ring, _, _ in entries)
    longest = max(len(ring) for ring, _, _ in entries)
    print(f"{len(entries)} rings, {points} points, longest {longest}, "
          f"{len(data)} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
