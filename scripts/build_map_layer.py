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
from pathlib import Path
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ENDPOINT = "https://overpass-api.de/api/interpreter"
USER_AGENT = "SolarOS-map-layer/1 (+https://github.com/zerokelvin-dev/solar_os)"
MAGIC = b"SOMB"
VERSION = 1
E7 = 10000000.0
RING_OPEN = 0x80000000

# Each feature is a list of Overpass filters over ways.
FEATURES = {
    "coastline": ['way["natural"="coastline"]'],
    "water": ['way["natural"="water"]', 'way["waterway"="riverbank"]'],
    "rivers": ['way["waterway"="river"]'],
    "major-roads": ['way["highway"~"^(motorway|trunk|primary)$"]'],
    "roads": ['way["highway"~"^(motorway|trunk|primary|secondary|tertiary)$"]'],
    "rail": ['way["railway"="rail"]'],
    "buildings": ['way["building"]'],
    "boundary": ['way["boundary"="administrative"]["admin_level"~"^(2|4)$"]'],
}


def build_query(bbox: str, features: list[str], timeout: int) -> str:
    south, west, north, east = [part.strip() for part in bbox.split(",")]
    box = f"({south},{west},{north},{east})"
    clauses = []
    for feature in features:
        if feature not in FEATURES:
            raise SystemExit(f"unknown feature {feature!r}; "
                             f"choose from {', '.join(sorted(FEATURES))}")
        clauses += [f"  {filter_}{box};" for filter_ in FEATURES[feature]]
    body = "\n".join(clauses)
    return f"[out:json][timeout:{timeout}];\n(\n{body}\n);\nout geom;\n"


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


def rings(payload: dict, tolerance: int) -> list[tuple[list[tuple[int, int]], bool]]:
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
            out.append((ring, not closed))
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


def pack(entries: list[tuple[list[tuple[int, int]], bool]]) -> bytes:
    points = sum(len(ring) for ring, _ in entries)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHII", VERSION, 0, len(entries), points)
    for ring, is_open in entries:
        out += struct.pack("<I", len(ring) | (RING_OPEN if is_open else 0))
    for ring, _ in entries:
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
    query = build_query(args.bbox, features, args.timeout)
    if args.query_only:
        print(query)
        return 0

    print(f"querying overpass for {args.bbox}", file=sys.stderr)
    entries = rings(fetch(query), args.tolerance)
    if not entries:
        raise SystemExit("no geometry in that box for those features")
    data = pack(entries)
    args.output.write_bytes(data)
    points = sum(len(ring) for ring, _ in entries)
    longest = max(len(ring) for ring, _ in entries)
    print(f"{len(entries)} rings, {points} points, longest {longest}, "
          f"{len(data)} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
