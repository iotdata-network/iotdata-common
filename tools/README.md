# iotdata-common/tools

Host-side helpers. One directory per tool; each carries its own README with the full detail.

## [`esp32-tool`](esp32-tool/README.md)

Flash, monitor, drive and diagnose an ESP32 over USB serial from a host with **no IDF toolchain** —
it needs python3, pyserial and esptool, not the compiler, cmake or ninja.

Built for the split workflow: build on a fast toolchain host, flash and watch on the host the boards
are actually plugged into, with `--pull` fetching the binaries between the two. Everything a flash
needs — chip, flash settings, the three binaries and their offsets — comes out of the build dir's own
`flasher_args.json`, so it works for any project without hardcoding anything.

Two parts of it earn their keep beyond convenience. `-w` waits for a port to appear rather than
failing, because a board that deep-sleeps between cycles is only enumerated for the few seconds it is
awake, and a missed window costs a whole sleep interval. And `diag` pulls the blackbox partition off
the chip with esptool and decodes it to CSV — far faster than streaming the records over the console,
and it still works on a device that has stopped talking.

## [`dialout`](dialout/README.md)

Reach a box that cannot be reached. A node behind CGNAT, on a mobile APN, or on somebody else's
router has no inbound path — nothing to port-forward, nothing to ssh to — so it dials **out** on a
timer and leaves a reverse ssh tunnel behind for you to come back through. Standalone: nothing in it
imports iotdata.

The server publishes an SRV record, which carries port as well as host, so one domain tells a client
the whole endpoint and the server can move without anybody touching a client. Beside it sits an
invite list, and that is what makes the scheme cheap: a client not on the list does two DNS lookups
and goes back to sleep having opened no socket at all, so a resting fleet costs nothing at either end
and a dial-out happens only when someone is actually waiting. Entries are 6-hex-digit md5 tokens
rather than names, because the record is world-readable and a public roll-call of your field sites is
nobody else's business; each client's loopback port is derived from its name the same way, so there is
nothing to register, distribute or keep in sync.

One ergonomic consequence worth knowing before you use it: the waiting session *is* the invitation.
Ctrl-C withdraws it, and a withdrawn invitation is precisely what tells the client to hang up.

## [`topology`](topology/README.md)

A site-survey map for placing gateways, relays and sensors: draggable positions with dashed range
rings, terrain-aware link profiles between any two of them, and a receptivity overlay showing where a
node could still be heard from.

Elevation is measured ground from Lantmäteriet's 1m laser model, held in a local cache of 1km blocks
keyed on the national grid, because Google's figures at the site this was built for run up to **14m
high** — what it returns there behaves like a canopy-inclusive surface averaged over ~100m rather than
ground. A second pass adds canopy height from the same laser campaign, so the two subtract without
registration error, and the path physics — knife-edge diffraction, woodland loss, Fresnel zones,
4/3-earth curvature, and the LoRa budget by spreading factor — is shared verbatim by the server and
the page, so the overlay and the profile panel cannot tell different stories about the same link.

The terrain side is Sweden-only; elsewhere the map and the geometry work and elevation falls back to
Google, with the accuracy caveat above. The cache ships empty.
