+++
id = "places"
title = "Places"
section = "app"
summary = "Keep points and paths: your position, nodes, and waypoints"
keywords = "places points waypoint position gnss fix meshcore node path self"
packages_any = ["service_places"]
+++

# Places

Places is the store of positions: your own, read from a GNSS device, and the
waypoints you keep. Anything that knows where something is publishes a point
here, and anything that wants to show or use a position reads it from here.

## Quick start

```text
places fix
places add home 43.6532 -79.3832
places list
```

`places fix` reads the first powered GNSS device and publishes your position.
`places add` stores a waypoint.

Waypoints are written to `.map/points.bin` on the card and come back after a
restart; every other point lives in RAM only, because its producer republishes
as soon as it hears a position again and a stored copy would be a stale claim.

## Quick reference

```text
places status                        point and path counts
places list                          every point, newest first
places add <label> <latitude> <longitude>
places remove <id>
places clear [source]                all points, or one source
places fix                           publish the current GNSS position
places path add <from-id> <to-id> [label]
places path list
places path remove <id>
places path clear <source>
places help                          print the usage above
```

Coordinates are decimal degrees, signed or with an `N`/`S`/`E`/`W` suffix.
An id names a point for as long as this boot lasts and no longer: the store
hands out ids from one, so a point restored from the card is not the point it
was before the restart. Nothing should write an id down.

## Paths

A path is a line between two points, held as references rather than as
coordinates, so it follows its endpoints as they move. That is what a mesh
hop is: a link between two nodes whose positions keep changing.

```text
places path add <from-id> <to-id> [label]
places path list
places path remove <id>
places path clear <source>
```

A path is removed with either of its endpoints, including when a point is
evicted to make room. A line drawn to somewhere the places no longer know is
worse than no line.

## Point sources

Each point carries a source name and a key that is unique within that source,
so republishing the same key moves the point rather than adding a second one.

| Source | Kind | Published by |
| --- | --- | --- |
| `gnss` | self | `places fix` |
| `meshcore` | node | chat adverts that carry a GPS position, a route refreshed for a node that had one, and the positions read back with the contacts at startup |
| `user` | waypoint | `places add`, and kept on the card |

When the store is full the oldest node point is evicted. Waypoints and your
own position are never evicted; a publish that would need to evict one fails
instead, and `places fix` says the places are full rather than blaming the GNSS.

