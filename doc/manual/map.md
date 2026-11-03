+++
id = "map"
title = "Map"
section = "app"
summary = "Draw the world, fetched cells of OpenStreetMap, and the places kept for it"
keywords = "map coordinates latitude longitude bearing distance geojson layer cell fetch stored overpass openstreetmap"
packages_any = ["service_map", "app_map"]
+++
# Map

The `map` app draws the points kept by [Places](places.md) over whatever map
layers are loaded on a display.

Positions published by GNSS and MeshCore are held in RAM only and are lost
on reboot; their producers republish whenever they hear a position again.
MeshCore keeps its own copy with the contacts, so node positions come back
with them at startup rather than waiting out an advert interval.
Waypoints from `places add` are written to `.map/points.bin` on the card and
read back at startup, because a point somebody placed by hand is not
something a producer will say again.

## Quick start

```text
map base
map
```

`map base` fetches the world, once; `map` with no arguments opens the app,
which needs a display. The points the map draws - your position, mesh nodes,
waypoints - are kept by [Places](places.md): `places fix`, `places add`.

## No map ships with the firmware

SolarOS carries no map data. Every layer comes from the card, either
fetched by the device or built on a desktop and copied over, so there is no
layer zero and no world outline underneath what you load: layer zero is
the first layer you load. A device with no storage draws the points, the
reticle, the scale bar and nothing else.

## Cells

The world is divided into cells a quarter of a degree square, and a cell is
the unit a map is fetched, kept and loaded in. A cell is named for its lower
corner in hundredths of a degree, four digits of latitude behind `n` or `s`
and five of longitude behind `e` or `w`: `n4350w07950` is the cell running
from 43.50 N, 79.50 W to 43.75 N, 79.25 W.

The name is arithmetic rather than an entry in a list, so any position names
the cell holding it and any name says where its cell is. That is what lets a
file on the card, a name typed at the shell and an area on the screen all
refer to the same thing without an index to keep in step.

## Fetching a cell

`map fetch` asks the OpenStreetMap [Overpass API](https://overpass-api.de/)
for one cell and writes it to the card. It takes either a cell name or a
latitude and longitude, which names the cell holding that position.

```text
map fetch n4350w07950
map fetch 43.6532 -79.3832
```

The request asks for water areas, rivers, railways, and roads down to
tertiary. Residential streets are left out: they cover a third of the screen
where the arterials cover a tenth, so which ways are fetched matters more
than where they stop being drawn. The roads that do arrive are packed in
three tiers - motorways and trunk roads as `highway`, primary and secondary
as `road`, tertiary as `minor road` - and each tier has a scale it appears
at, the same in every cell: motorways always, rail from 181 metres a pixel,
arterials from 91, tertiaries from 45. A regional view keeps the motorways
by themselves, a city view adds the arterials, and the tertiaries come in
near street level.

The answer is asked for deflated. A dense cell is about 13 MB of JSON and
under 2 MB on the wire, which is the difference between a fetch that
finishes inside its deadline on a slow link and one that does not, and a
seventh as much to write to the card while it arrives. The answer is a
means rather than the map: it is inflated as it is packed and then deleted.
What is kept is the packed geometry, a tenth of the JSON, which needs no
inflating to draw.

The answer arrives over tens of seconds. `map fetch` waits for it and prints
what happened; one fetch runs at a time, and a second one while the first is
running is refused. An answer larger than 8 MB deflated is refused rather
than stored, which is what `too much data for one cell` means. A cell nobody
has mapped is reported as `nothing mapped here` and nothing is written.

A fetched cell lands in `.map/layers/<cell>.bin` and is not loaded by the
fetch. Each element's tags decide its class, so water, railways and roads
are coloured as themselves: the tags arrive after the geometry, so the class
is filled in when the element closes. Anything the query does not explain
falls back to road.

In the app, pressing Enter fetches the cell under the reticle, which needs a
network path. The word drawn inside the cell outline says what a press will
do: `fetch` when there is no file, `load` when there is one, and `wait`
while a fetch is running. A cell already drawn says nothing, because the
map is its label; pressing it puts it away.

## The world, the first time

The first time the map app opens on a device with no layers, it downloads
the world: a coastline, its lakes, the borders between countries and the
borders within them, four files fetched from
[Natural Earth](https://www.naturalearthdata.com/) and packed on the way
in. Natural Earth is public domain and asks for no key and no account.
About 270 KB arrives over the wire and is then free for ever.

```text
map base
```

`map base` does the same from the shell, and is what the app suggests when
there is no network to do it over. It fetches only what is missing, so a
download interrupted halfway finishes rather than starting again, and once
all four are kept it loads them instead of fetching anything.

Nothing about this is required: the four files are ordinary layers in
`.map/layers`, so `map forget` removes one and `map load` puts a different
one in its place.

## What is kept, and what is loaded

```text
map stored
map load n4350w07950
map layers
map unload 0
```

`map stored` lists what is kept in `.map/layers`, with an index, a size, and
whether it is loaded. Knowing what is on the card is a directory listing;
reading one in is megabytes, so nothing loads at startup and nothing loads
until somebody asks for it. A card holds as many maps as it has room for,
and one listing shows 64 of them, saying how many more there are. Loading
by name or by number walks the directory, so a map is never out of reach
for being kept late.

Twelve layers may be loaded at once, but what actually runs out is memory,
not slots: a world map is about 0.6 MiB of geometry, a quarter-degree cell
of a dense city 1.2 to 1.7 MiB, and a rural one a fraction of that. Five
mebibytes may be held in total, which `map status` reports beside the
count. Loading past that puts away whichever layer has gone longest
without being on screen, so panning across a city loads cells as they
come into view and drops the ones behind without anybody managing it. A
world layers are pinned, so they are never the ones put away, and they
are drawn first whatever order things were loaded in: if nothing
else can go, the load is refused instead. Pinning happens when the world
is loaded as the world, by `map base` or by opening the app — a world
layer loaded by hand with `map load land.bin` is an ordinary layer.

`map load` takes whatever you have to hand, told apart by its first
character, so the answer never depends on what happens to be on the card:

| First character | What it means |
| --- | --- |
| `/` | a path to a file anywhere, copied in and kept like any other |
| `n` or `s` | a cell, named the way the app and `map stored` show it |
| a digit | a place in the `map stored` list |
| anything else | the name of a kept map, with or without the `.bin` suffix |

Loading imports: the file is copied into `.map/layers`, so afterwards it is
kept like any other and can be loaded again by name or number without
fetching it again. The directory is the index — what is in it is what loads
— so there is no separate list to keep in step with the files.

`map unload` gives back the memory and keeps the file, which is why putting
a map away does not cost somebody else's bandwidth to get it back. Indices
come from `map layers`, not from `map stored`. `map forget <name>` is the
other half: it deletes the kept copy, unloading it first if it is loaded, so
a full card can be cleared without leaving the shell.

```text
map forget n4350w07950.bin
```

Layers draw in the order they were loaded, and loading a name that is
already loaded replaces it rather than drawing it twice.

Two formats are read, told apart by the file's first bytes rather than by
its name. A packed layer (`.bin`) costs no parsing at all: its header says
how many rings and points follow, so its size is read out of it. A GeoJSON
file is parsed by a character scanner rather than into a document, so a
large file costs only its geometry in memory; polygons and lines are both
read. A GeoJSON file carries no class of its own, so `map load` tags every
ring land; the class is a parameter, which is how `map base` reads the same
format as water, borders and regions. Pack it with the script below to
colour a file of your own.

Either format may be stored deflated, and is recognised by its gzip header
rather than by a `.gz` suffix. Deflating is never required: a file kept in
the clear loads by the same command and reads to the same geometry. This is
for a map somebody copied onto the card already compressed — a fetched cell
is kept packed rather than deflated, because packed geometry only shrinks
by about a quarter again and would then cost an inflate on every load.

## Sourcing a layer on a desktop

Two scripts build packed layers, and neither needs an account or a key.

`build_map_basemap.py` takes [Natural Earth](https://www.naturalearthdata.com/),
which is public domain and covers the world at three levels. It writes one
file per feature into `--out-dir` — `land.bin`, `lakes.bin`, `borders.bin`
and `regions.bin` — because a layer is the unit the map loads, orders and
unloads. `--level` chooses 110m, 50m or 10m, `--tolerance` drops vertices
closer together than a given distance in 1e7 degrees, and `--no-lakes`,
`--no-borders` and `--no-provinces` leave a feature out.

```text
scripts/build_map_basemap.py --level 50m --out-dir layers/
map load /sdcard/layers/land.bin
```

Nothing is compiled into the firmware. A world map is four files on the
card, so a reader who wants a different world replaces them rather than
rebuilding.

`build_map_layer.py` takes a bounding box from the Overpass API, which is
where detail below a continent comes from, and gives a finer result than the
device's own fetch because it can ask for more and simplify it with more
room to work in.

```text
scripts/build_map_layer.py --bbox 43.58,-79.64,43.86,-79.12 \
    --features coastline,water,major-roads --output toronto.bin
```

Features are `coastline`, `water`, `rivers`, `major-roads`, `roads`,
`streets`, `rail`, `buildings` and `boundary`. `--tolerance` drops vertices
closer together than a given distance, three metres by default; there is no
point keeping detail finer than a pixel at the zoom the layer is for.
`--query-only` prints the Overpass query and stops.

Each feature tags its rings with a class, and a display with colour draws
each class in its own: land green, water blue, roads near-black, rail and
country boundaries grey, the borders within a country a lighter grey, and
buildings tan. Closed rings are filled and open ones drawn as lines. A
display without colour draws every class as a black outline, as the map did
before classes existed.

The script asks only for ways, so water that OpenStreetMap holds as a
relation, which is most large lakes, does not arrive. Rivers and smaller
water bodies are ways and do. The device's own `map fetch` asks for
relations as well, and keeps what it gets as it came: the pieces of the
lake's edge, islands among them, in no order and joined to nothing. They
are packed as a class of their own, `water edge`, and the map fills all of
a layer's pieces together - a scanline needs every edge of a closed curve,
not the curve in order - so the lake is filled, its islands are holes, and
nothing is clipped to the cell. The whole lake is on the card, and only
the rows on the screen are filled.

The map drops what it cannot show before drawing it: a ring whose bounds
fall outside the view is skipped without projecting a vertex, an area
smaller than a couple of pixels is skipped, a line is skipped when its own
class is too crowded on screen to tell one from another, and within a ring
vertices closer together than a couple of pixels are dropped. A city layer
seen from across the world is a few thousand segments rather than fifty
thousand, which is the difference between a map and a solid black area.

Getting the file onto the card: write it directly if the card is out of the
device, or fetch it over the network from the device itself.

```text
curl -o /sdcard/toronto.bin http://192.168.1.10:8000/toronto.bin
map load /sdcard/toronto.bin
```

## Keys

| Key | Action |
| --- | --- |
| Arrows, `h` `j` `k` `l` | Pan an eighth of a screen |
| `+` `-` | Zoom in and out |
| `Enter` | Fetch, load or unload the cell under the reticle |
| `Tab`, `n`, `p` | Select the next or previous point |
| `c` | Centre on the selected point, and follow it if it is your position |
| `f` | Fit every point on screen |
| `r` | Read the GNSS position now |
| `d` | Delete the selected point |
| `q`, `Esc` | Leave the app |

The sea is drawn as a pale ground under everything, so land drawn over it
reads as land. Paths are dashed lines. Your own position is a filled circle
inside a ring, a node is an open circle, and a waypoint is a cross. A small
cross sits at the centre of the view: panning moves the map under it, so it
is what you line up with a target when working down from a zoomed-out view.
Zooming holds the centre, so a target stays under the cross as you go in.
The cell the reticle sits in is outlined with a dashed line while it is
something you could act on — a file already kept, or a network to fetch one
over — and while it is between a quarter of the screen and a couple of
screens wide. The selected point is boxed and labelled, and the row under
the map gives its coordinates, and the distance and bearing from your own
position when one is known. With nothing selected that row gives the
reticle's own position and the scale.

## Quick reference

```text
map                                  open the app
map status                           layer count and bytes against the budget
map fetch <cell>                     ask OpenStreetMap for one cell
map fetch <latitude> <longitude>     the cell holding a position
map base                             download the world map, once
map stored                           the maps kept on the card, with indices
map load </path|cell|index|name>     load a map and keep a copy of it
map layers                           the loaded layers, in drawing order
map unload <index|all>               free a loaded layer, keeping its file
map forget <name>                    delete a kept map from the card
map help                             print the usage above
```

Points and paths are `places` commands: see [Places](places.md).

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

## Where the maps come from

A cell fetched by the device, and any layer built with
`scripts/build_map_layer.py`, comes from
[OpenStreetMap](https://www.openstreetmap.org/) through the
[Overpass API](https://overpass-api.de/), and is licensed under the
[Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/).
That licence asks for credit whenever the data is shown, so the application
draws `© OpenStreetMap contributors` over the map whenever the map is on
screen, whatever is loaded, shortened to `© OpenStreetMap` when the row is
too narrow for the long form. Crediting too often is the harmless
direction, and a line that comes and goes is worse to read than one that
stays. The full terms are at
<https://www.openstreetmap.org/copyright>, and the
[attribution guidelines](https://osmfoundation.org/wiki/Licence/Attribution_Guidelines)
say what is expected:

> The credit should be in a form and location that is reasonably calculated
> to make any Person that uses, views, accesses, interacts with, or is
> otherwise exposed to the Content aware that Content was obtained from
> OpenStreetMap.

Overpass is a free service run by volunteers, with a stated limit of about
10,000 requests and 1 GB a day across all of a user's traffic. It asks that
callers not stitch bounding boxes together to scrape wide areas, and not
point production applications at the public instance. That is why a fetch
takes one cell, only when somebody asks for it, and keeps what it gets:
`429` is Overpass rate limiting this address and `504` is the query giving
up; both mean come back later rather than ask again now. The refusal carries
a line of plain text saying which limit was hit, and that line is what the
app shows, as `openstreetmap busy, try later`. The status and the byte
count go to the log rather than the screen, where `log show` will find
them: a status number is nothing a reader can act on.

Layers built with `scripts/build_map_basemap.py` come from
[Natural Earth](https://www.naturalearthdata.com/), which is public domain:

> All versions of Natural Earth raster and vector map data found on this
> website are in the public domain.

Nothing need be credited for them.
