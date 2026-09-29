+++
id = "map"
title = "Map"
section = "app"
summary = "Plot geo-tagged points over a world coastline that ships with SolarOS"
keywords = "map gps gnss position waypoint coordinates latitude longitude plot bearing distance geojson layer coastline"
packages_any = ["service_map", "app_map"]
+++
# Map

The map service is a producer-neutral sink for geo-tagged points, the same
way the inbox is one for messages. The `map` app draws them over a world
coastline on a display.

Points are held in RAM only and are lost on reboot.

## Quick start

```text
map fix
map add home 43.6532 -79.3832
map
```

`map fix` reads the first powered GNSS device and publishes your position.
`map add` stores a waypoint. `map` with no arguments opens the app, which
needs a display.

## The world ships with SolarOS

The built-in coastline is Natural Earth 1:110m, which is public domain, so
the map needs no tile service, no account, no network, and no storage. It is
about 40 KB of land outlines compiled into the firmware and is always
present as layer zero.

At that scale a coastline is accurate to a kilometre or so, which suits a
world or continental view and not a street. Load a finer layer for detail.

## Layers

`map load` adds geometry on top of the built-in world; it never replaces it.
Layers draw in the order they were loaded, and up to four can be loaded at
once.

```text
map load /sdcard/coast_50m.geojson
map layers
map unload 1
map unload all
```

Two formats are read. A GeoJSON file is parsed by a character scanner rather
than into a document, so a large file costs only its geometry in memory;
polygons and lines are both drawn as outlines. A packed layer costs no
parsing at all and is worth preparing for geometry that is loaded often.

## Sourcing a layer

Two scripts build packed layers, and neither needs an account or a key.

`build_map_basemap.py` takes Natural Earth, which is public domain and
covers the world at three levels. The 1:110m level is the one already in
the firmware; 1:50m is the useful step up while still being a world file.

```text
scripts/build_map_basemap.py --level 50m --output coast50m.bin
```

`build_map_layer.py` takes a bounding box from the OpenStreetMap Overpass
API, which is where detail below a continent comes from. Overpass is a free
shared service, so keep the boxes small; OpenStreetMap data is ODbL, so
anything published from it owes attribution to its contributors.

```text
scripts/build_map_layer.py --bbox 43.58,-79.64,43.86,-79.12 \
    --features coastline,water,major-roads --output toronto.bin
```

Features are `coastline`, `water`, `rivers`, `major-roads`, `roads`,
`rail`, `buildings` and `boundary`. `--tolerance` drops vertices closer
together than a given distance; there is no point keeping detail finer than
a pixel at the zoom the layer is for.

Match a layer to the zoom it is for. A file holding every road in a city is
tens of thousands of line segments, and the map redraws all of them every
second, so a layer that is far denser than the screen can show costs time
for nothing.

Getting the file onto the card: write it directly if the card is out of the
device, or fetch it over the network from the device itself.

```text
curl -o /sdcard/toronto.bin http://192.168.1.10:8000/toronto.bin
map load /sdcard/toronto.bin
```

## Point sources

Each point carries a source name and a key that is unique within that source,
so republishing the same key moves the point rather than adding a second one.

| Source | Kind | Published by |
| --- | --- | --- |
| `gnss` | self | `map fix`, and the app every five seconds while it is open |
| `meshcore` | node | chat adverts that carry a GPS position |
| `user` | waypoint | `map add` |

When the store is full the oldest node point is evicted. Waypoints and your
own position are never evicted; a publish that would need to evict one fails
instead.

## Keys

| Key | Action |
| --- | --- |
| Arrows, `h` `j` `k` `l` | Pan an eighth of a screen |
| `+` `-` | Zoom in and out |
| `Tab`, `n`, `p` | Select the next or previous point |
| `c` | Centre on the selected point, and follow it if it is your position |
| `f` | Fit every point on screen |
| `r` | Read the GNSS position now |
| `d` | Delete the selected point |
| `q`, `Esc` | Leave the app |

Coastlines are drawn as outlines rather than filled areas. Your own
position is a filled circle inside a ring, a node is an open circle, and a
waypoint is a cross. The selected point is boxed and labelled,
and the row under the map gives its coordinates, and the distance and
bearing from your own position when one is known.

## Quick reference

```text
map                                  open the app
map status                           point and layer counts
map list                             every point, newest first
map add <label> <latitude> <longitude>
map remove <id>
map clear [source]                   all points, or one source
map fix                              publish the current GNSS position
map layers                           list the built-in world and loaded layers
map load <path>                      add a GeoJSON or packed layer on top
map unload <index|all>               remove a loaded layer
```

Coordinates are decimal degrees, either signed (`-79.3832`) or with a
hemisphere suffix (`79.3832W`).

## Projection

The map is Web Mercator, the projection every slippy map uses. Longitude
sets x and latitude sets y with no term from the view centre, so panning
only ever shifts the picture and shapes stay locally correct at every
latitude. The cost is the familiar one: area grows towards the poles, and
the projection stops short of them at about 85 degrees.

The scale in the footer is the ground a pixel covers at the centre of the
view, so it changes as you pan north or south even though the zoom has not.
Distances and bearings in the info row are great-circle values and do not
come from the projection.
