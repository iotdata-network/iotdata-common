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

## Boxes on an always-on link

The timer suits a box that is rarely reachable or pays for every wake. On broadband a box is always
reachable, and all a 12-hour timer buys is a slow answer to an invite. `watch` stays running instead
and asks every `watch-every` seconds (default 60, which is the invite record's TTL — asking faster
only re-reads the cached answer), so a `dialout-server wait` is answered in a minute or two.

Each round is an ordinary `dial` in its own process, so nothing about tunnels, `idle`, `hold-max` or
cleanup changes: the loop only schedules, and waits while a round holds a tunnel. It logs "not
invited" once rather than every minute, backs off to at most 10 minutes while dials fail, and refuses
to run with a `link-script` set — a watch would raise a modem every round.

`install --watch` installs `dialout-client-watch.service` and removes the timer; plain `install` does
the reverse, so a box is only ever in one mode. The timer refuses intervals under an hour and points
here instead, because its jitter, restart limit and per-wake logging are built for long gaps.

## Setting it up

**On the server** (needs `dropbear-bin`, `miniupnpc` if you want the hole punched, `curl`, `node`):

```sh
cp dialout.cfg dialout.$(hostname).cfg # the template becomes this box's whole config
$EDITOR dialout.$(hostname).cfg        # dns-domain, cloudflare-token, cloudflare-zone
dialout-server install                 # dialoutd account, host key, state dir, systemd unit
dialout-server run                     # in a terminal -- or: systemctl enable --now dialout-server
```

`run` is the same code path either way and logs to stdout, so running it by hand while you work on
a site is a first-class way to operate this, not a debug mode. It also reports tunnels coming and
going, which makes a terminal running it a passable fleet monitor.

**On each client** (needs `dnsutils`, `openssh-client`, `iproute2`, `coreutils`; a modem box also
wants a `link-script` — copy `dialout-link.example`):

```sh
dialout-client keygen                  # prints the public key
cp dialout.cfg dialout.$(hostname).cfg # the template becomes this box's whole config
$EDITOR dialout.$(hostname).cfg        # dns-domain at minimum
dialout-client install                 # service + timer, every 12h (--interval to change)
#   or, on an always-on link:
dialout-client install --watch         # stays running, dials whenever invited (--every to change)
```

`install` copies the client, `dialout.<hostname>.cfg` and the unit templates to
`install-dir` (default `/usr/local/lib/dialout`) and runs the service from there — never from the
checkout, which on a dev box may be a network share that is not mounted at boot. To update a box,
re-run `install` from the checkout; `status` says whether the installed copy differs.
`dialout-server install` does the same, for the same reason, and both take `--prefix DIR` to override
it. One thing that copy does *not* carry is `iotdata-conf`: if the Cloudflare token is reached that
way and that path is on a share, a server starting at boot comes up unable to publish DNS (the config
is read once), so on such a box put `cloudflare-token=` in `dialout.<hostname>.cfg` instead — install
copies that, mode 600.

Two traps worth knowing, both learned the hard way. `install` copies the per-host cfg **from wherever
it is run**, so running it from the checkout reverts a box-local edit of `dialout.<hostname>.cfg` —
keep per-box settings such as a modem's `link-script` in the checkout copy. And run the *installed*
client, not the one on the share: the config parse alone took 94 s over CIFS against 0.7 s locally on
a Pi Zero 1, which makes `install` look like it has hung.

Then hand that public key to the server once:

```sh
dialout-server enrol iotdata-tst-2 /path/to/its.pub     # or pipe it on stdin
```

### Tagging a box

A client is named after its MAC, which makes a fine token and a terrible label. Tag it:

```sh
dialout-server tag iotdata-rem-50411c64b8fc solar shed, Brannan
dialout-server tag 2878c0 bench pi zero + simcom modem     # a fragment of the name will do
dialout-server tag "solar shed"                            # no text clears it
```

The tag then shows as a column in `dialout-server clients`, appears in the `tunnel UP`/`DOWN` log
lines, and — the useful part — stands in for the name anywhere a client is named:

```sh
dialout-server wait --for "solar shed"
dialout-server invite modem
dialout-server revoke iotdata-rem-50411c64b8fc
```

Resolution accepts the exact name, the exact tag, or any unambiguous fragment of either; two matches
is an error that lists them rather than a guess. A string that matches nothing enrolled is passed
through unchanged, so you can still invite a box before enrolling it. `enrol --tag "..."` sets it at
the same time.

It is stored in the comment field of the client's `authorized_keys` line, since that file is already
the register of who is enrolled and a second list would only drift from it — which also means a
`revoke` takes the tag with it. Tokens and ports still derive from the real name, so tags are operator
sugar: rename or drop one whenever you like and no client notices.

### Is it still alive?

`dialout-server clients` has a LAST SEEN column. A client fills it in with a **ping**: on a round that
finds no invitation, it opens one short ssh to the server, whose only effect is that the forced command
behind its key (`dialout-seen`) records the time. Nothing is sent and nothing comes back — being able
to authenticate *is* the signal, and it is the signal worth having, because it says this box can reach
the rendezvous. A box that can reach DNS but not the server is broken in the way that matters, and
would look healthy under any scheme that had it write its liveness somewhere else.

```sh
dialout-server clients
NAME                 TOKEN   PORT   INVITED  TUNNEL  LAST SEEN
iotdata-rem-b827eb2878c0 6827cc  47432  no       -       7m
iotdata-rem-50411c64b8fc 78fadb  47239  no       -       1m
iotdata-rem-b827eb20b3d4 bfef5f  47355  no       -       -        # never pinged: older client
```

`ping-every` rations it, independently of how often rounds happen — the two have nothing to do with
each other. `0` means every uninvited round, which suits a timer box where a round is already a rare
event and the radio is up anyway. `12h`, the default, suits a watch box that comes round every 60s and
would otherwise announce itself 1440 times a day for no added insight. A tunnel is not pinged: `ssh -N`
opens no session, so the forced command cannot run on that path, and the server stamps those itself
when it sees the tunnel appear — a live tunnel being the strongest liveness there is.

It costs nothing in credentials, which is the point: it reuses the enrolled key, so a box someone
walks off with still holds exactly one capability, and the server still decides what that key may run.
The alternative — handing every remote box a Cloudflare token so it can write its own DNS record —
would give each of them the run of the whole zone, including the SRV endpoint every other client
trusts, because Cloudflare scopes DNS tokens to a zone and no finer.

`install` rewrites existing enrolments to point at `dialout-seen` (it only touches `command="..."`,
never the key). Older clients keep working untouched — they simply never ping, so they show `-`.

Check either end at any time with `dialout-client check` / `dialout-server status`, and skip DNS
entirely while testing with `dialout-client dial --endpoint host:port --force`.

## Configuration

A box's configuration is **one file**: `dialout.<hostname>.cfg` beside the program (or `--config`).
`dialout.cfg` is the committed, documented **template** you copy it from, and is never read at run
time — nothing is merged, so everything a box does can be read off its own file. The host file is
**not** committed, and is where the domain and the Cloudflare token go. With no host file a program
runs on built-in defaults, and anything that needs a domain or token says which file to create;
`status` / `check` print the file in use on their first line. Same file, same rules, both programs —
a box can be either end. Read `dialout.cfg` itself; every key is explained there rather than
duplicated here. When the template gains a key, add it to each host's file.

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
