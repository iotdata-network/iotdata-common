# iotdata-common/tools

Host-side helpers. One directory per tool; each carries its own README.

| Tool | What it is |
|---|---|
| [`esp32-tool`](esp32-tool/README.md) | Flash, monitor, drive and diagnose an ESP32 over USB serial, on a host with no IDF toolchain. Built for the split workflow: build on the fast host, flash and watch on the host the boards are plugged into. |
| [`dialout`](dialout/README.md) | Reverse-tunnel phone-home for boxes you cannot reach (CGNAT, mobile APN, someone else's router): the node dials out on a timer and leaves an ssh tunnel behind. Standalone — nothing in it depends on iotdata. |

