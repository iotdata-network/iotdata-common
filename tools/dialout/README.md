# dialout

Reach a box that cannot be reached.

A node behind CGNAT, on a mobile APN, or on somebody else's router has no inbound path: there is
nothing to port-forward and nothing to ssh to. So it dials **out** on a timer, and leaves a reverse
tunnel behind that you ssh back through.

Two programs, and they are independent of iotdata — nothing here imports it, and the only touch
point is an optional read of `iotdata.conf` for a Cloudflare token you may already have.

| | | |
|---|---|---|
| `dialout-client` | POSIX shell | on the unreachable box, under a systemd timer |
| `dialout-server` | node, no deps | on a box you can reach, in a terminal or under systemd |

## The idea

The server publishes three DNS records through Cloudflare and the client reads them back:

```
_dialout._tcp.example.com         SRV   0 0 47022 dialout.example.com    where to dial
dialout.example.com               A     203.0.113.9                      the address
_dialout-invite._tcp.example.com  TXT   "v=1 920092 2facba:1757500000"   who is wanted
```

**SRV**, not a bare hostname, because it carries the port as well as the host: a client needs only
the domain to learn the whole endpoint, and the server can change host or port without anybody
touching a single client.

**The invite list is what makes this cheap.** A client that is not on it does two DNS lookups and
goes back to sleep, having opened no socket at all — so the resting state of a fleet costs nothing
at either end, and a dial-out only happens when someone is actually waiting. Entries are the
6-hex-digit md5 token of the client name, not the name: the record is world-readable, and a public
roll-call of your field sites is nobody else's business. An entry may carry `:<expiry>`; the token
`*` invites everybody, which is what you want while commissioning.

Each client gets one loopback port on the server, **derived** from its name — `md5(name)`, first
six hex digits, modulo the span, plus the base. Both ends compute it from the name alone, so there
is nothing to register, distribute, or keep in sync.

## The session

In a screen session on the server:

```
$ dialout-server wait --for iotdata-tst-2
14:22:01Z invited iotdata-tst-2 (920092) -- TXT updated
14:22:01Z waiting for iotdata-tst-2 (token 920092) on port 47102 -- Ctrl-C to stop and withdraw
...
14:41:36Z *** iotdata-tst-2 IS UP ***

    ssh -p 47102 <user>@127.0.0.1        # from this host
```

It rings the terminal bell when the tunnel appears. **Leave it running while you work**: Ctrl-C
withdraws the invitation, and a withdrawn invitation is what tells the client to hang up. The
screen session *is* the invite, so there is nothing to remember to turn off — but equally, do not
dismiss it the moment the bell goes and then wonder where your tunnel went. `wait --keep` stops
watching without withdrawing, if you would rather manage the invitation yourself
(`dialout-server uninvite <name>`).

## Hanging up

Three things end a held tunnel, and none of them cuts off a session in progress.

**The invitation is withdrawn.** The client rechecks the TXT record every `invite-recheck` seconds
and hangs up when it is no longer listed. This is the main mechanism: the tunnel lasts as long as
someone is waiting on it, rather than as long as a timer says — which is what stops a 4G box
burning power on a tunnel nobody wants. If a session is open it waits for that to close first. A
withdrawn record and a dead resolver look identical from the client, so it confirms DNS is still
answering before acting on an empty reply; a hiccup must not drop a working tunnel.

**Idle.** Nothing has used it for `idle` seconds (default 30 minutes). Deliberately generous: an
invitation that still stands means someone still wants in, and they may be doing something else
between logins. "Used" means the tunnel is carrying an established connection, sampled every
`poll` seconds.

**`hold-max`.** The absolute cap, busy or not. Keep it well under the timer interval so two runs
can never fight over the same port.

## Boxes whose network *is* the dial-out

A node with a 4G modem powered down between windows has no DNS to resolve over until something
brings the modem up. Set `link-script` and it is called with `up` before anything else happens and
`down` the moment the tunnel is finished — including when the run ends early because there was no
invitation, so an uninvited check costs seconds of modem rather than minutes.

A failed `up` aborts the run rather than dialing into a dead link. The script must be idempotent,
`down` runs even if `up` failed, and `link-timeout` (default 60s) guards it — because the failure
mode of a modem script is not "returns an error", it is "sits there", and a client wedged on a
`wwan0` that will never come up has silently left the fleet. Start from `dialout-link.example`.

## Setting it up

**On the server** (needs `dropbear-bin`, `miniupnpc` if you want the hole punched, `curl`, `node`):

```sh
dialout-server install                 # dialoutd account, host key, state dir, systemd unit
$EDITOR dialout.$(hostname).cfg        # dns-domain, cloudflare-token, cloudflare-zone
dialout-server run                     # in a terminal -- or: systemctl enable --now dialout-server
```

`run` is the same code path either way and logs to stdout, so running it by hand while you work on
a site is a first-class way to operate this, not a debug mode. It also reports tunnels coming and
going, which makes a terminal running it a passable fleet monitor.

**On each client** (needs `dnsutils`, `openssh-client`, `iproute2`, `coreutils`; a modem box also
wants a `link-script` — copy `dialout-link.example`):

```sh
dialout-client keygen                  # prints the public key
$EDITOR dialout.$(hostname).cfg        # dns-domain at minimum
dialout-client install                 # service + timer, every 12h (--interval to change)
```

Then hand that public key to the server once:

```sh
dialout-server enrol iotdata-tst-2 /path/to/its.pub     # or pipe it on stdin
```

Check either end at any time with `dialout-client check` / `dialout-server status`, and skip DNS
entirely while testing with `dialout-client dial --endpoint host:port --force`.

## Configuration

`dialout.cfg` is the committed, documented baseline. `dialout.<hostname>.cfg` is read after it and
overrides key by key, is **not** committed, and is where the domain and the Cloudflare token go.
Same file, same rules, both programs — a box can be either end. Read `dialout.cfg` itself; every
key is explained there rather than duplicated here.

Cloudflare credentials fall back to `network-dns-cloudflare-key` / `-zone` in `iotdata.conf` if the
dialout config leaves them empty, purely as a convenience on a box that already has one. Set
`iotdata-conf=` empty to sever even that.

## What the client key can and cannot do

The account is `dialoutd` — deliberately **not** `dialout`, which on Debian is the serial-port
group (gid 20); an ssh account that landed in it would hand every client key `/dev/tty*`.

- dropbear runs with `-w` (no root), `-s -g` (no passwords), and `-j` (**no local forwarding**), so
  a stolen client key cannot be used to reach into the server's LAN.
- No `-a`, so forwarded ports bind to loopback only: you must already be on the server (or through
  its own sshd) to use one.
- Every `authorized_keys` line carries `no-pty,no-agent-forwarding,no-X11-forwarding` and
  `command="/bin/false"`. The account's shell is `/bin/sh` and not `nologin` only because dropbear
  checks the shell against `/etc/shells` and refuses the login outright otherwise — the lock is in
  `authorized_keys`, where it works.
- Its own host key and its own `authorized_keys`, sharing nothing with the host's real sshd.

What it does **not** do: stop one enrolled client from listening on another's port. Ports are
derived, not enforced. `dialout-server clients` warns on a collision; pin one with `tunnel-port=`.

## Testing without any of the DNS

Everything above works on loopback with two terminals and no domain at all:

```sh
dialout-server --config test.cfg run                                   # bind=127.0.0.1
dialout-client --config test.cfg dial --endpoint 127.0.0.1:47222 --force
```
