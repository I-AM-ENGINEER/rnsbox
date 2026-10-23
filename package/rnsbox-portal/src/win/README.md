# RNSBox for Windows (native build)

A single `rnsbox.exe` that serves the same admin UI as the router image,
straight from the shared C++ route handlers. No VM, no WSL, no GUI: start it
(or install it as a Windows service) and open the portal in a browser.

```
rnsbox.exe --setup                 # find/install Python + Reticulum (asks first)
rnsbox.exe                         # serve on http://127.0.0.1:8040/
rnsbox.exe --root D:\rnsbox        # own config tree
rnsbox.exe --listen 0.0.0.0:8040   # expose to the LAN (change the password!)
```

Default login: **admin / admin**.

## What works

| Tab | Windows build |
|---|---|
| Dashboard | yes — RAM/CPU/uptime, adapter table, default route, DNS, rnsd status |
| Reticulum | yes — config editor, `rnstatus`, restart, TCP client / interface editing |
| Settings | yes — portal password, hostname, clock source |
| Echo Bot | yes (needs `lxmf`) |
| Access / Donate | yes |
| Network | read-only — the adapter table; routing/NAT is not part of this build |
| WiFi, Bluetooth, HaLow/SLIP, Console, NomadNet, Updates | hidden (503) — no radio, no PTY, no nftables |

## How it runs Reticulum

The exe does **not** bundle Python. It uses the system interpreter:

1. `RNSBOX_PYTHON`, then `python.exe` on `PATH`, then the standard
   `%LOCALAPPDATA%\Programs\Python\Python3*` / `%ProgramFiles%\Python3*`
   installs, then the `py` launcher.
2. `--setup` checks `import RNS` and offers `pip install rns lxmf nomadnet`.
   If Python itself is missing it offers `winget install Python.Python.3.13`.
   Nothing is installed without a `y`.
3. `rnsd` is started as a child (`python -m RNS.Utilities.rnsd --config …`).
   If another rnsd already owns the shared instance (port 37428 — NomadNet, a
   service install), the exe **joins it** instead of starting a second daemon.
4. `--doctor` prints the resolved interpreter, RNS version, root and pages.

## Files

Everything lives in one root, mirroring the router's tree, so the shared code
works unchanged:

```
%ProgramData%\RNSBox\
  etc\rnsbox\           portal configs (wan.conf, openports.conf, …)
  etc\reticulum\        Reticulum config + identity (edited by the UI)
  usr\share\rnsbox-portal\pages\   page templates
  www\                  static assets
  var\log\              portal.log (service), rnsd.log
  var\run\               rnsd.pid
```

Override with `--root` or `RNSBOX_ROOT`. `/proc`, `/sys`, `/run` and `/dev`
are mapped under the root and stay absent — the UI degrades exactly like a
board without that hardware.

## Build

Requirements: MSYS2 **UCRT64** (`x86_64-w64-mingw32-g++`), its OpenSSL and
zlib. The script puts `C:\msys64\ucrt64\bin` first in `PATH`; without that the
compiler picks the MINGW64 runtime DLLs and dies silently.

```bash
cd package/rnsbox-portal/src
./win/build-win.sh              # win/build/rnsbox.exe + win/build/root/
./win/build-win.sh --static     # single self-contained exe (static libcrypto)
```

The output tree already contains `pages/`, `www/` and the Python helpers.
Then:

```bash
cd win/build
./rnsbox.exe --root ./root
```

## Background service

```powershell
# elevated prompt
.\rnsbox.exe --root C:\ProgramData\RNSBox --install-service
sc start rnsbox
```

The service runs without a console and logs to
`<root>\var\log\portal.log`. `--uninstall-service` stops and removes it.
The exe implements the SCM handshake itself, so no NSSM/sc.exe wrapper is
needed. Browsing to `http://127.0.0.1:8040/` works while the service runs.

## Notes and limits

- The default port is **8040** — one random pick in 80xx, deliberately outside
  8080-8089 (where every dev server, proxy and container defaults). It is
  compiled in, so every machine gets the same URL; `--listen ADDR:PORT`
  overrides it.
- The listener binds `127.0.0.1` by default. `--listen 0.0.0.0:8040` exposes
  the admin UI to the network; the portal's Host allowlist (IP literals and
  `localhost` only) and same-origin/CSRF checks still apply.
- Opening TCP 4242 and UDP 29716/29717 in Windows Firewall is what lets peers
  reach rnsd; the exe does not touch firewall rules.
- AutoInterface works; BLE does not (Reticulum has no Windows BLE transport).
- The A/B image updater, WiFi, HaLow and the PTY consoles are router-only.
