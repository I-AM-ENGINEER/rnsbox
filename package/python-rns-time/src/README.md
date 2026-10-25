# rns-time

**Authenticated time distribution over [Reticulum](https://reticulum.network/).**
Time for off-grid mesh nodes, over TCP, I2P, or LoRa transports, with no
internet anywhere in the chain.

```
              ┌─ GPS satellites ─> stratum-1 chrony (OpenTimeCard) ─┐
time sources ─┤                                                     ├─> rns-time server (RNS node)
              └─ ordinary NTP servers (pool.ntp.org, …) ────────────┘          │  4-timestamp exchange
                                                                               v   over an RNS Link
                                                          mesh clients (LoRa / TCP / I2P)
```

## Two server models

* **Reference (GPS/chrony).** The original deployment: a host whose local
  clock is disciplined by chrony from a stratum-1 GPS source. The server
  reports chrony's stratum and root dispersion.
* **Minimal (upstream NTP).** No chrony, no GPS, no root. The server asks a
  few ordinary NTP servers for the time every few minutes and keeps the
  measured offset against its local clock. It adds that offset to the two
  timestamps it serves, so **the host's own clock does not have to be
  right** — a box that is days off still hands out correct time. This is the
  mode the RNSBox router image uses as a *client*, and it is also how a
  cheap always-on VPS can provide time to a mesh.

Both speak protocol v1 and run next to `rnsd` through the **shared instance**
by default (no interfaces are opened by rns-time itself).

## Why

Off-grid nodes have terrible clocks and no way to set them: no NTP, no cell
network, maybe no GPS. But wrong time breaks TLS-style cert validity windows,
log ordering, scheduling, and any cryptographic protocol with timestamps.
rns-time gives a mesh node time that is:

- **Authenticated**, an RNS Link is end-to-end encrypted to the *server's
  identity*; if you know the server's destination hash, nobody can spoof its
  time answers (contrast: classic NTP is plaintext and trivially spoofed)
- **Latency-tolerant**, the NTP-style 4-timestamp exchange cancels *path
  delay*; residual error is path *asymmetry*. Multi-second LoRa RTTs are fine.
- **Small**, the server is a few hundred lines of Python and depends only on
  `rns` (which vendors msgpack); the client is the same.

## Expected accuracy

| Transport | typical RTT | expected offset error |
|---|---|---|
| TCP / local | 10-100 ms | ~1-20 ms |
| I2P | 0.5-5 s | ~50-500 ms |
| LoRa (fast preset) | 1-6 s | ~0.2-2 s |

For a node that was days or weeks adrift, every row of that table is a win.

## Protocol (v1)

Client opens an RNS **Link** to the server destination (`rnstime`/`time`),
then for each round sends a msgpack map `{"v":1,"t1":<client send time>}`.
The server replies immediately with:

```
{"v":1, "t1":..., "t2":<server recv>, "t3":<server send>,
 "stratum":<stratum>, "root_disp_ms":<error bound>, "leap":"Normal"}
```

Client stamps `t4` on arrival and computes (RFC 5905 style):

```
offset = ((t2-t1)+(t3-t4))/2      delay = (t4-t1)-(t3-t2)
```

Several rounds are run; the sample with the **lowest delay** wins (least
queuing, least asymmetry). The server includes its stratum and root
dispersion so the client knows the *quality* of what it's syncing to.

In the minimal model `t2`/`t3` are the server's local clock **plus the
measured upstream offset**, and the advertised stratum is the upstream
stratum + 1. When no fresh upstream sample exists the server answers with
stratum 16 (holdover) and uncorrected local time; clients ignore anything at
or above `MAX_HEALTHY_STRATUM` (9). An announce is only emitted while the
sample is healthy, so a server in holdover also disappears from the mesh.

## Minimal server (NTP -> Reticulum)

```bash
# runs against the rnsd shared instance (default)
python3 -m rns_time.server \
    --config /etc/reticulum \
    --ntp pool.ntp.org,time.google.com \
    --identity /var/lib/rns-time/identity \
    --hash-file /run/rns-time.destination

rns-time destination: 4f2a…   # the trust anchor for clients
```

Options: `--interval` (upstream sample period, default 300 s), `--announce`
(default 1800 s), `--max-age` (holdover after this, default 1800 s),
`--timeout` (per NTP server), `--standalone` (open interfaces from
`--config` instead of requiring rnsd). The server creates the identity if it
does not exist.

`rnstime-server.service` is a ready systemd unit. It needs the same RNS
config directory as `rnsd` (so it can find the shared instance socket) and
does **not** need root for the clock. Run it as the same user as `rnsd`; on a
typical install that is root.

## Client

Measurement only:

```bash
python3 -m rns_time.client <destination_hash> --config /etc/reticulum --shared
```

Machine-readable (what the RNSBox init script consumes):

```bash
python3 -m rns_time.client <hash> --json --quiet
```

Step the clock and record the source (root for the step):

```bash
python3 -m rns_time.client <hash> --apply \
    --config /etc/reticulum --shared \
    --source-file /run/clock-source \
    --synced-file /run/clock-synced \
    --lock /run/ntpsync.step.lock \
    --rtc /dev/rtc0 \
    --min-offset 0.5 --respect-source
```

`--apply` takes the shared clock-step lock, re-checks the source under it,
steps with `clock_settime` (falling back to `date -u -s`), writes
`source=rns` and the synced marker, and writes the RTC with `hwclock`.
`--respect-source` leaves the clock alone when standard NTP has already
synced it (`source=ntp`). With `--json` the result is one object:

```json
{"healthy": true, "offset": -0.0312, "delay": 0.004, "stratum": 3,
 "root_disp_ms": 34.5, "leap": "Normal", "applied": false, ...}
```

Exit codes: `0` ok/applied/skipped, `1` no Reticulum or no path/link,
`2` no samples, `3` server unhealthy (holdover), `4` could not set the
clock, `5` clock-step lock busy.

## RNSBox integration

The [RNSBox](https://github.com/I-AM-ENGINEER/rnsbox) router image ships
this module as `python-rns-time` and runs `/etc/init.d/S88rnstime`:

* config: `/etc/rnsbox/rnstime.conf` — `enabled`, `servers` (one or more
  destination hashes), `interval`, `step_threshold`, `upstream`, `serve`;
* the client polls in the background, applies only healthy samples, and
  records `source=rns` (which the portal shows next to `ntp`/`browser`/`rtc`);
* `serve=yes` additionally runs the minimal server, so a router with an
  uplink can give time to the rest of the mesh;
* Settings → Time in the portal edits the same file and offers *Sync now*.

No internet is required for the client path: once `rnsd` has a path to the
server (LoRa, HaLow, LAN or TCP), the exchanges work.

## Files

- `rns_time/protocol.py` — wire format + offset math (shared)
- `rns_time/server.py` — serves time; upstream-NTP mode by default
- `rns_time/ntp.py` — minimal SNTP query (stdlib only)
- `rns_time/config.py` — tiny `KEY=VALUE` reader
- `rns_time/client.py` — N-round exchange, best-sample selection,
  `--set`/`--apply`, JSON output
- `rnstime-server.service` — systemd unit for the minimal server
- `setup.py` — installs the `rns-time-client` / `rns-time-server` commands
- `beacon/` — experimental LXMF store-and-forward time beacon stub (not v1)

## Security notes

- The Link is encrypted and authenticated **to the server identity**:
  distributing the server's destination hash out-of-band (QR, config bake-in)
  is the trust anchor. Back up the identity file: a new identity is a new
  hash, and every client must be reconfigured.
- Time answers include quality metadata; clients ignore answers with
  `stratum >= 10` (holdover).
- The server never trusts the client for anything; it only echoes timestamps.
- The minimal server trusts its upstream NTP servers to the same degree
  classic NTP does (plaintext, unauthenticated). Use servers you trust (a
  local chrony, your provider's, the pool) or the GPS/chrony model when
  authenticity matters more than convenience.
