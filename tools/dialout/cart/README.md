# cart — the Pi side of a switched-rail window

The node (an ESP32 relay) owns the 5V rail and the schedule; see
`iotdata-common/include/device/d_interface_cart.h` for that side, the switch circuit and the
numbers quoted below. This directory is the **cart**: what a Pi Zero has to do between being given
power and giving it back.

The contract is two lines and one idea. The node switches `CART_PWR`; the cart oscillates
`CART_BEAT`. **Beating means alive, and beating is also the hold-open** — there is no "stay up"
request and no "I'm finished" message. Stopping is how the cart says it is done, and it is also what
a crashed cart does, which is exactly why the action is the same for both: cut the rail.

Everything the cart does follows from that. Nothing on this side needs to be clever; it needs to be
hard to get wrong while the power is being taken away mid-sentence.

## The shape: one service that never stops, one that runs once

**`cartbeat.service` — the beat.** Starts as early in boot as it practically can, toggles the pin at
1 Hz, and never exits (`Restart=always`). It is not conditional on anything: not on the modem, not
on the network, not on the payload succeeding. It is the proof that this board's scheduler is still
running, and the moment it becomes conditional on something else it stops being that.

Deliberately a **software** loop, even though a PWM pin could produce a jitter-free beat in hardware.
A beat that outlives the software it is vouching for is worthless — that is the whole argument in the
node's header for an edge rate over a level, one layer down.

**`cartdial.service` — the work, then the halt.** `Type=oneshot`, started once per boot, runs the
payloads and then powers the board off. This is the "rc.local" in the sketch, and the ordering below
is the part worth getting right.

**It refuses to halt while somebody is logged in** (capped by `CART_LINGER_MAX`, an hour, inside the
node's two-hour backstop). Without that a cart is unusable on a bench: give it power, it dials, finds
no invitation, and halts about a minute later — under your fingers, while you are reading the journal
that would have told you why. `cartbeat` keeps beating throughout, which is what holds the rail on.

Shutdown stops `cartbeat` as an ordinary part of systemd's teardown. The silence that follows *is*
the signal; the node then waits `silent_ms` (15s) + `settle_ms` (30s) before cutting, which is the
filesystem's protection and is ample for a Zero's halt. Nothing stops the beat explicitly — doing so
before the halt would only start the clock early.

## Ordering: do NOT wait for the network

The instinct is `After=network-online.target`. On a cart that is wrong, and expensively so: a cart in
the field has no network *until the payload dials*, so wait-online sits out its full timeout (120s by
default) and spends the boot budget doing nothing. The modem comes up inside the dial, through
`dialout-link`.

So: `After=basic.target`, nothing about the network, and the payload brings up its own link.

For the same reason a cart does not use dialout's **timer or watch mode** — the node's
`interval_ms` (12h) *is* the schedule. Install the client for the files, then disable the timer:

```sh
dialout-client install            # copies client + cfg to /usr/local/lib/dialout
systemctl disable --now dialout-client.timer
```

## Failure: halt, but retry the dial a few times first

The sketch says: if the session fails, halt; if not, stay up until it ends, then halt. Right — with
one refinement. A modem that has not finished registering fails a dial attempted 20 seconds after
power-on, and halting on that first failure throws away the whole window for a modem that would have
been ready at 45 seconds. So the payload retries against a deadline (three attempts over ~5 minutes),
then halts either way.

Both outcomes end in `poweroff`. There is nothing to be gained by staying up with no tunnel, and
power is the thing being spent.

`dialout-client dial` maps onto "stay alive until the session has ended" exactly: it blocks while the
tunnel is held and returns when the invite is withdrawn or `idle` expires. The cart's window length
is therefore the client's own business, which is where the node's header wants it — the cart is the
only end that knows whether anybody is still using the tunnel.

## Numbers that have to agree with the node

The node's backstop (`limit_ms`, 2h) is protection against a wedged cart, not a schedule, and a cart
that hits it is being hard-cut. **The current fleet dialout config would hit it:** `hold-max=21600`
is six hours. A cart wants:

```
hold-max=5400      # 90 min, comfortably inside the 2h backstop
idle=600           # 10 min of silence ends the session; 30 is expensive on solar
ping-every=0       # every window reports in; a window is already a rare event
require-invite=1   # an uninvited window costs ~60s of rail and goes back to sleep
```

The leading allowance is `boot_ms` = 2 minutes from power-on to the **second** beat
(`CART_BEATS_MIN` is 2 — one edge is what a board that drives the pin and then wedges produces). A
Zero W reaches `sysinit` in 15–25s, so starting the beat there leaves most of the budget spare. That
margin is worth keeping: being cut mid-boot is also *filed as a failure*, which engages the node's
back-off and costs the next window too.

## The pin

**GPIO17, physical pin 11.** Nothing in `config.txt`, nothing to remember.

Measured on a Zero W, which is why it is 17 and not something tidier:

```
17: ip -- | lo      ← idles LOW, undriven
 4: ip -- | hi      ← idles HIGH
```

That matters only during the Pi's own boot, before `cartbeat` takes the pin — but an undriven pin
sitting near the node's input threshold beside a switching supply is how you invent edges you never
sent, and two of those is "alive". 17 is low from reset, so the first edge the node ever sees is a
real beat. GPIO4 (pin 7) would have made a tidier harness, one 5-way hop with the UART, at the cost
of a `gpio=4=op,dl` line in `config.txt` to park it — not worth depending on.

Two overlays to keep off a cart image, both for the same reason: `dtoverlay=w1-gpio` (default pin
GPIO4) and `dtoverlay=gpio-poweroff`. Each is a kernel driver that CLAIMS its line when it probes,
after which the beating process cannot drive the same pin. `install.sh` warns if it finds either.

## What is in here

```
cartbeat              the beat: toggles GPIO17 at 1 Hz, forever, conditional on nothing
cartdial              the window: payloads, then linger while logged in, then halt
window.d/10-dialout   the payload: dial, retrying because the first attempt races the modem
systemd/*.service     cartbeat, cartdial, cart-halt
install.sh            copies all of it to /usr/local/lib/cart, enables NOTHING by default
```

```sh
./install.sh                    # install; it refuses to enable anything on its own
systemctl start cartbeat.service
/usr/local/lib/cart/cartdial --no-halt     # a window that will not take the board away from you
./install.sh --enable           # when the board really is wired to a node
```

`install.sh` does not enable the units on its own, because enabling `cartdial` on a board that is
not actually a cart means it runs its payloads and powers itself off, and only somebody standing
next to it can undo that. It also checks the things that bite: `pinctrl` present, GPIO17 not in an
alt mode, neither claiming overlay enabled, and the dialout settings below.

**The bench escape hatch**, honoured before any halt:

```sh
touch /etc/cart/inhibit        # this board stays up, across reboots
touch /run/cart-inhibit        # this boot only
```

Settings live in `/etc/cart/cart.conf` (written commented; the defaults are in the scripts):
`CART_BEAT_PIN`, `CART_BEAT_INTERVAL`, `CART_LINGER_MAX`, `CART_DEADLINE`, `CART_DIAL_TRIES`,
`CART_DIAL_GAP`.

### Details worth knowing before you debug it

- **`who` is not enough to detect a session.** On these images sshd is dropbear, which does not write
  utmp: with an ssh session open, `who` returns 0 while `ss -tn state established '( sport = :22 )'`
  returns 1. `cartdial` counts both, and the `ss` half is the one doing the work — on its own, `who`
  would have halted the board under the person logged into it. An operator arriving through the
  reverse tunnel also lands as a connection to `127.0.0.1:22`, so the same check covers them.
- **`cartbeat` drives the pin low when it is stopped.** Tidiness, not a signal; the absence of beats
  is the signal.
- **No libgpiod on these images** (`gpioset`/`gpioinfo` absent), so the beat uses `pinctrl`, which
  speaks BCM numbering directly. The `/sys/class/gpio` route also exists but its export numbers are
  the kernel's global GPIO numbers, not BCM — a trap worth avoiding for one fork per second.
- **`cart-halt.service` is only reached through `OnFailure`**, so it cannot turn an operator's
  `reboot` into a poweroff. An `ExecStopPost=` on `cartdial` would have done exactly that.
- **No network filesystems on a cart**, and read-only root as soon as the overlay work lands: a hung
  `umount` outlives `silent_ms + settle_ms` and gets the rail cut mid-shutdown, which is the one way
  this design can still cost you a filesystem.

## What you can tell afterwards, for free

The node cannot distinguish "finished" from "dead" — both are silence. But its counters separate
*never beat* from *beat and then stopped*, and the dialout server's LAST SEEN says whether the phone
home happened. Together:

| node counters | server LAST SEEN | what happened |
|---|---|---|
| never beat | stale | did not boot — rail, card, wiring |
| beat, then stopped | fresh | the window worked |
| beat, then stopped | stale | booted, could not phone home — modem, APN, carrier |
| hit the backstop | either | the cart failed to close itself; look at the payload |

No extra plumbing on the cart, which matters for a board that is powered off almost all of the time
and may end up with a read-only root and volatile logs.
