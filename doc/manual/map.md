+++
id = "map"
title = "Map"
section = "app"
summary = "Plot geo-tagged points from any source on a scrolling terminal map"
keywords = "map gps gnss position waypoint coordinates latitude longitude plot bearing distance"
packages_any = ["service_map", "app_map"]
+++
# Map

The map service holds geo-tagged points published by any part of the system,
the same way the inbox holds messages published by any source. The `map` app
draws those points on a scrolling, zoomable terminal map.

Points are held in RAM only and are lost on reboot.

## Quick start

```text
map fix
map add home 43.6532 -79.3832
map
```

`map fix` reads the first powered GNSS device and publishes your position.
`map add` stores a waypoint. `map` with no arguments opens the app.

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
| Arrows, `h` `j` `k` `l` | Pan a quarter of a screen |
| `+` `-` | Zoom in and out |
| `Tab`, `n`, `p` | Select the next or previous point |
| `c` | Centre on the selected point, and follow it if it is your position |
| `f` | Fit every point on screen |
| `r` | Read the GNSS position now |
| `d` | Delete the selected point |
| `q`, `Esc` | Leave the app |

Points are drawn as `@` for your own position, `o` for a node, and `+` for a
waypoint. The selected point is shown inverted with its label, and the info
row below the map gives its coordinates, and the distance and bearing from
your own position when one is known.

## Quick reference

```text
map                                  open the app
map status                           point count and capacity
map list                             every point, newest first
map add <label> <latitude> <longitude>
map remove <id>
map clear [source]                   all points, or one source
map fix                              publish the current GNSS position
```

Coordinates are decimal degrees, either signed (`-79.3832`) or with a
hemisphere suffix (`79.3832W`).

## Projection

The map uses an equirectangular projection around the view centre, which is
accurate at the scales a terminal can show and wrong near the poles. A
terminal cell is about twice as tall as it is wide, so a row covers twice the
ground distance of a column. Distances and bearings in the info row are
great-circle values and do not come from the projection.
