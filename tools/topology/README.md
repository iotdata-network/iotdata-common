# topology

A site-survey map for placing gateways, relays and sensors: draggable positions with dashed range
rings, terrain-aware link profiles between any two of them, and a receptivity overlay that paints
where a node could still be heard from.

Built for siting a real deployment before carrying hardware up a hill. The elevation it reasons
about is measured ground, not an API's guess, which is the difference between a link that closes on
paper and one that closes in the field.

## Three programs

| | |
|---|---|
| `topology` | The server and the map page. Node, no dependencies. |
| `lm-terrain-ground` | Fills the terrain cache with the ground model. Node, no dependencies. |
| `lm-terrain-canopy` | Adds the vegetation layer. Python — the payload is LAZ. |

Run the two cache builders once for a site, then the server reads only the local cache:

```sh
LM_USERPASS="user:pass" ./lm-terrain-ground  --state topology.json --radius 1000
LM_USERPASS="user:pass" ./lm-terrain-canopy  --state topology.json --radius 1000
MAPS_API_KEY=... ./topology --port 4096
```

`--help` on any of them lists the options. `lm-terrain-canopy` needs `pip install 'laspy[lazrs]' numpy requests`.

## Secrets, and which program holds which

Neither key is ever written to disk by these tools, and no single program needs both.

The **Maps JS key** goes to `topology` as `--key` or `MAPS_API_KEY`, and is injected into the page as
it is served, so `topology.html` stays committable. It necessarily reaches the browser — that is how
the Maps JS API works — so restrict it by HTTP referrer in the Google console rather than treating
it as a secret.

The **Lantmäteriet Geotorget credentials** go to the two cache builders as `LM_USERPASS`, and nowhere
else. The server reads only the cache, so it never holds the password and the browser never sees one.

## The terrain cache

**The cache ships empty.** Fill it for your own site with the two builders above; it is designed to
be committable, so for a private deployment that only has to happen once per block, ever.

Blocks are 1km squares on the SWEREF 99 TM national kilometre grid, which is what makes them
shareable: two sites a few hundred metres apart reuse the same blocks, a site straddling a
Lantmäteriet tile boundary is no special case, and a node 10km out pulls blocks around *itself*
rather than the whole bounding rectangle. Point several sites at one `--cache` to share them.

Ground is stored as int16 decimetres because 0.1m is exactly the uncertainty Lantmäteriet publishes
for the product, so the stored value is never the limiting error — and it compresses about 6x, which
is what keeps the cache committable at all.

## Where the data comes from

| Layer | Source | Resolution |
|---|---|---|
| Ground | Markhöjdmodell, read out of the cloud-optimised GeoTIFF by byte range (a few hundred KB per block, not the 267MB tile) | 1m grid, 0.1m height uncertainty |
| Canopy | Laserdata Skog point cloud, highest return per cell | 2m grid |

Both come from the **same** Lantmäteriet laser campaign — Markhöjdmodell is the ground-classified
subset, Laserdata Skog is every return including treetops — so they subtract into a canopy height
with no cross-dataset registration error.

Google Elevation stays selectable as a provider so the two can be compared directly, and the
comparison is the reason for all of the above: at this site Google runs up to **14m high**, behaving
like a canopy-inclusive surface averaged over ~100m rather than ground.

Cells with no laser return are stored as *unknown* rather than zero. Water absorbs the beam, and a
data gap must not read as "no trees here".

This makes the terrain side Sweden-only. Elsewhere the map, the rings and the geometry all work;
elevation falls back to Google, with the accuracy caveat above.

## The path physics

`lib/link.js` is loaded by both the server and the page, because the overlay evaluates thousands of
paths (only the server holds the terrain) while the profile panel evaluates one, in the browser. They
have to agree to the last decibel or the map and the panel would tell different stories about the
same link, so the maths lives in one file.

It computes free-space loss, ITU-R P.526 single knife-edge diffraction, Weissberger woodland loss
through the canopy layer, Fresnel zone radii, and earth curvature as a 4/3-radius bulge added to the
terrain so the sight line stays straight. On top of that sits the LoRa budget: SX127x/SX126x
sensitivity for SF7–SF12 at 125kHz, system gain as transmit power plus *both* antennas (gain is
reciprocal, so it belongs per device), the spreading factors that still close a given path, and an
EIRP figure for the regulatory check — EU868 allows 14dBm ERP on the common channels, and a 22dBm
module behind a 5dBi antenna is already past it.

The margin bands are deliberately coarse at the top (strong starts at 45dB) because an SF12 budget is
enormous next to free-space loss at these ranges — 151dB against 91dB at a kilometre — so finer bands
would paint most of a short-range disc one colour and hide the structure you are siting against. Drop
the budget towards SF7 to sharpen it.

## Library

| | |
|---|---|
| `lib/link.js` | The path physics above, shared verbatim by server and browser |
| `lib/blocks.js` | The cache: block keying, quantisation, deflate, memory ceiling |
| `lib/sweref.js` | WGS84 ↔ SWEREF 99 TM (EPSG:3006), accurate to millimetres anywhere in Sweden |
| `lib/cog.js` | Just enough TIFF to pull float32 elevations from a tiled, deflated, predictor-encoded COG over HTTP byte ranges. No GDAL on purpose |

Positions persist to `--persist` (default `./topology.json`), which is also what the cache builders
read to decide which blocks to fetch.
