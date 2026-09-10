# esp32-tool

Work with an ESP32 over USB serial from a host that has **no IDF toolchain** — flash it, watch it,
drive its CLI, and pull its diagnostics off it.

Built for the split workflow: build on a fast toolchain host, then flash and watch on the host the
boards are actually plugged into. That host needs only python3 + pyserial, and esptool for
flashing — not the compiler, cmake or ninja.

```
esp32-tool flash    -p PORT -b BUILD_DIR [--pull HOST:PATH]   # write image to chip
esp32-tool monitor  -p PORT [command]                          # print console (interactive)
esp32-tool command  -p PORT <cmd>                              # one command, just its answer
esp32-tool diag     extract|decode|clear                       # blackbox diagnostics partition
esp32-tool deploy   -p PORT -b BUILD_DIR [--pull HOST:PATH]    # flash, then monitor
```

Everything the flash needs — chip, flash settings, the three binaries and their offsets — is read
from the build dir's `flasher_args.json`, so it works for any project without hardcoding anything.
`monitor` is the same coloured-log / reconnect / command-inject loop as
`iotdata-device/sds/tools/esp32-boot`; this tool is a superset.

## Typical two-host flow

Build on the toolchain host:

```sh
# on workshop
cd iotdata-example/simulator_sensor_lora_esp32 && make
```

Flash and watch on the host the boards live on (e.g. `iotdata-tst-3`):

```sh
# artifacts already local (shared FS / copied):
esp32-tool deploy -p /dev/ttyACM0 -b ../simulator_sensor_lora_esp32/build

# or pull the binaries straight off the build host, then flash + monitor:
esp32-tool deploy -p /dev/ttyACM1 \
    --pull workshop:/opt/iotdata/src/iotdata-example/simulator_sensor_lora_esp32/build
```

## Boards that sleep

A board that deep-sleeps between cycles is only enumerated for the few seconds it is awake, so
`/dev/ttyACM0` mostly does not exist and the flash window is easy to miss — losing it costs a whole
sleep interval. **`-w` waits for the port** instead of failing, on every subcommand that uses one:

```sh
esp32-tool deploy -p /dev/ttyACM0 -w      # sit until it wakes, flash it, then monitor
```

The build dir is resolved (and `--pull` done) *before* the wait, so the window is spent flashing
rather than on setup. `monitor` already survives the re-enumeration on its own.

## Driving the device CLI

`command` issues one command and prints only its response — the C-tagged lines — with log noise
filtered out, which is what makes it scriptable:

```sh
esp32-tool command -p /dev/ttyACM0 vers
esp32-tool command -p /dev/ttyACM0 "diag flush"
```

## Diagnostics

`diag` pulls the blackbox diagnostics partition off the chip with esptool and decodes it to CSV —
far faster than streaming the records over the console, and it works on a device that is not
talking. It implements the same record walk as the blackbox decoder rather than importing it, so
the two stay independent (see `iotdata-depend/blackbox/README.md`).

```sh
esp32-tool diag extract -p /dev/ttyACM0 --raw diag.bin --out diag.csv
esp32-tool diag decode diag.bin          # offline, re-decode an image pulled earlier
esp32-tool diag clear --confirm          # erase it
```

NB `diag` and `flash` reset the device; `monitor` does not unless you pass `-r`.

## Dependencies on the target host

None of these is the compiler toolchain:

| For | Needs | Install |
|---|---|---|
| `monitor`, `command` | python3, pyserial | already present (used by `esp32-boot`) |
| `flash`, `diag` | esptool | `pip install esptool` |
| `--pull` | rsync + ssh | usually already present |

## Notes

- **Two boards / stable ports.** `ttyACM` numbering can move across a reset or re-enumeration. To
  pin a physical board, address it by a stable path: `-p /dev/serial/by-path/<...>` (see
  `ls -l /dev/serial/by-path`).
- **Port default** is `$ESP32_PORT`, then `/dev/ttyACM0`.
- Exit the monitor with **Ctrl-C**.
