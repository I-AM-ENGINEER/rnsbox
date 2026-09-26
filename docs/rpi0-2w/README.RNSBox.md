# RNSBox — a Reticulum router for the Raspberry Pi Zero 2 W

RNSBox turns a **Raspberry Pi Zero 2 W** (BCM2710A1, quad Cortex-A53, 512 MB,
aarch64) into a small, USB-powered [Reticulum](https://reticulum.network/)
router and transport node with an OpenWrt-style web admin UI.

It is distributed as a **patch series on top of upstream Buildroot**, so this
tree contains only the RNSBox delta — Buildroot itself and the Raspberry Pi
Foundation kernel are fetched at build time.

- **Upstream base:** [`buildroot/buildroot`](https://github.com/buildroot/buildroot) at commit `679b9ead7620bbf193620d1ebf56f53c1764d37a` (2026.02.3 LTS)
- **Apply + build:** see [APPLYING.md](APPLYING.md)

## What it does

- **`rnsd`** (Reticulum, the latest release at build time) runs as the
  long-lived service: a transport node with a `TCPServerInterface` on
  `0.0.0.0:4242`, an `AutoInterface` on the USB LAN, and two public
  RNS-testnet uplinks preconfigured.
- **USB gadget = LAN.** The micro-USB OTG port presents a CDC-NCM network
  interface (`usb0 = 10.42.0.1/24`, dnsmasq DHCP + DNS). The DVD build
  additionally exposes a read-only mass-storage "disc" pre-loaded with
  Reticulum client apps for a zero-download quick start.
- **WiFi = WAN**, DHCP via dhcpcd, with NAT masquerade plus per-rule port
  forwarding and open-port management. A USB-Ethernet adapter, if plugged in,
  appears as `eth0` and can take over as the wired WAN instead.
- **`rnsbox-portal`** — a compact C++/CGI admin UI (uhttpd, ~0 resident RAM)
  on `http://10.42.0.1/` for network, Reticulum, WiFi and system settings —
  including setting the clock from your browser (handy on a board with no
  RTC). It is the single source of truth for the generated `nftables`
  ruleset (boot and live-apply both call the same module).
- Optional **NomadNet** LXMF / pages node (off by default), **lxmd** LXMF
  propagation node (off by default) and **SLIP-over-UART link to an external
  WiFi-HaLow (RNode) modem** (off by default).

The design goal throughout is a minimal OS: headless (no display stack,
`gpu_mem=16`), Bluetooth disabled so the hardware UART is free, and nearly all
of the 512 MB RAM available to Linux and the router data plane.

## Hardware

- Raspberry Pi Zero 2 W: quad Cortex-A53, 512 MB, onboard 2.4 GHz WiFi
  (brcmfmac; Bluetooth is disabled by RNSBox), one micro-USB OTG port
  (power + gadget data), no Ethernet, no RTC.
- microSD: the DVD build's size tracks the current client releases (~3.3 GB
  now; use a card comfortably larger, e.g. 8 GB); the lite build is ~400 MB.
  The rootfs auto-grows to fill the card on first boot.

## WiFi on the Zero 2 W

The board has one 2.4 GHz radio, driven from the portal's **WiFi** tab. It has
four modes: **off**, **STA** (join an upstream network — the WAN uplink),
**AP** (run a hotspot), and **STA + AP** (both at once: `wlan0` client +
`uap0` hotspot).

Because it is a **single radio**, the STA and AP time-share it, and the
hotspot is treated as the priority so local clients stay served:

- If the upstream network drops or you carry the device **out of range**,
  RNSBox stops the client-side scan that would otherwise hop the radio off
  the hotspot's channel — so devices connected to the RNSBox AP keep working,
  with no blips.
- It retries the upstream automatically, but **only while nobody is connected
  to the hotspot**, so a reconnect scan can never interrupt an active client.
- When the upstream returns, the AP re-syncs to its channel and normal
  concurrent operation resumes.

A small background watchdog (`S36wifiwatch`) enforces this and is active only
in **STA + AP** mode; in STA-only or AP-only mode it does nothing.

## Default credentials

| Service | User  | Password |
|---------|-------|----------|
| SSH     | root  | admin    |
| Web UI  | admin | admin    |

Change these on first use. The web password is stored hashed (pbkdf2) in
`/etc/rnsbox/auth.json` after first login; the session-signing key is
generated at runtime and never stored in the source tree.

## Build variants

```
./build-rnsbox.sh lite    # NCM-only image, no clients disc (~400 MB)
./build-rnsbox.sh dvd     # full image + read-only clients disc (auto-sized)
```

See [APPLYING.md](APPLYING.md) for the full flow.

## Licensing

- The **RNSBox code in this patch series** (the `rnsbox-portal` app, init
  scripts, Buildroot package recipes, build scripts and configuration) is
  released under the **MIT License** — see [LICENSE](LICENSE).
- The **upstream base** (Buildroot, the Raspberry Pi Foundation kernel and
  firmware) remains under its own respective licenses.
- The bundled Reticulum client applications are **downloaded at build time**,
  not redistributed in this tree. They carry their own licenses, some of which
  are non-commercial (e.g. Sideband, CC BY-NC-SA) or copyleft (e.g. Ratspeak,
  AGPL-3.0); review them before redistributing any built image.

## Known follow-ups

- `fetch-clients.sh` downloads a subset of the client apps and does not yet
  verify pinned checksums or cover every app the DVD build stages from
  `clients-stick/`. Treat the clients disc as optional and review that
  directory before relying on it. The `lite` build does not need it.

## Connecting a WiFi-HaLow (RNode) modem over SLIP

RNSBox can use an external **WiFi-HaLow** RNode modem — a Taixin TXW-series
HaLow bridge running I-AM-ENGINEER's
[RNode_Halow_Firmware](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware) —
as a long-range sub-GHz Reticulum interface, connected over a 3-wire UART using
**SLIP** (IP-over-serial). Optional and **off by default**.

### Wiring — pinout

Use the **hardware UART** (PL011) = `/dev/ttyAMA0`, on the GPIO header.
RNSBox disables Bluetooth and keeps the serial console **off** this UART, so
the port is fully free. All three lines are **3.3 V TTL** — never wire a 5 V
or RS-232 level.

TX and RX must **cross over** — the box's TX goes to the modem's RX and vice
versa:

```
  Raspberry Pi Zero 2 W (UART0 / ttyAMA0)          HaLow RNode modem
  ─────────────────────────────────                ─────────────────
   GPIO14 (physical pin 8)  ·  TX ───────────────►  RX
   GPIO15 (physical pin 10) ·  RX ◄───────────────  TX
   GND  (e.g. physical pin 6) ──────────────────── GND   (required)
```

| Pi pin | GPIO    | Function | Wire to modem |
|--------|---------|----------|---------------|
| 8      | `GPIO14` | UART0_TX | RX            |
| 10     | `GPIO15` | UART0_RX | TX            |
| 6      | —       | GND      | GND           |

Keep the leads short — at 2 Mbaud, long unshielded jumpers invite frame errors.

### Baud

Both ends must run the **same baud**. The PL011 has headroom to 3 Mbaud, so
the modem's **2000000** default works as-is — preconfigured in
`/etc/rnsbox/slip.conf` and the portal card; no need to slow the modem down
(unlike the LicheeRV Nano-e build).

If `rx_over_errors` climbs under load (`ip -s link show sl0`), drop both ends
to `1000000` or `921600`.

### Bring it up

1. **Configure + wire** the modem as above; note its SLIP IPs (defaults: modem
   `192.168.7.2`, host `192.168.7.1`).
2. **Enable it**: portal → Reticulum tab → *HaLow modem (SLIP)* → tick
   *Enable*, set device/baud/IPs to match, *Save & apply*. This sets the line
   speed (`stty`), attaches SLIP (`ldattach`), and brings up a point-to-point
   `sl0` to the modem — equivalent to editing `/etc/rnsbox/slip.conf` (applied
   by `S31slip`). `ping 192.168.7.2` should now answer.
3. **Add the Reticulum interface** in the config editor, then Restart rnsd:

   ```
   [[HaLow]]
     type = TCPClientInterface
     enabled = yes
     target_host = 192.168.7.2
     target_port = 8001
   ```

   RNode_Halow_Firmware serves its RNode TCP endpoint on **port 8001** (not the
   `4242` the standalone Reticulum docs use). Confirm the port with
   `curl http://192.168.7.2/api/tcp_server_cfg`.

### Managing the modem from the portal

With SLIP up, the modem's own web UI is reachable only from the box. The
portal reverse-proxies it behind the login — *Reticulum tab → Open modem web
UI* (or browse to `/modem/`) opens the modem's full config/stats page through
your portal session, so nothing is exposed unauthenticated on the LAN.
(Firmware OTA is the exception: flash the modem over its own Ethernet UI.)

The SLIP link is a point-to-point host route to the modem only; it doesn't
touch the `wlan0` WAN or `usb0` LAN. Requires `CONFIG_SLIP` (kernel) and
`ldattach` (util-linux) — both shipped.

## Support RNSBox

RNSBox is free and open source (MIT). If it's useful to you, you can support
ongoing development, test hardware, and hosting for the Reticulum testnet
uplinks with a crypto donation. The web UI also has a **Donate** page
(sidebar → Donate) with a scannable QR code for each wallet.

| Coin | Address |
| --- | --- |
| Bitcoin (BTC) | `bc1q559qfr8nlqr2s6p07hgm03x357mncydehntdmlj7hu2qaud8wkqsgycagh` |
| Ethereum (ETH) | `0x5bc9b408d67c4b8294290e1dd281526be5913864` |
| Solana (SOL) | `HjpiqhDdLd3p2ZcutYFptjX9TFiNYMWFGZUGVQnfesxM` |
| Litecoin (LTC) | `ltc1qaatf3kken6peg8z6s840w4kjad643y9gptt9775zgwkhpzuyxl3qjrpss2` |
| Dogecoin (DOGE) | `9umb1Mqvq5bYg7AG8fFghvzsFHZo9fnBmf` |
