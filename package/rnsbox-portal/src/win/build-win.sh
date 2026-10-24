#!/bin/bash
# build-win.sh — build the native Windows portal (rnsbox.exe) with MinGW-w64.
#
#   package/rnsbox-portal/src/win/build-win.sh [--static]
#
# Output (under win/build/):
#   rnsbox.exe                     the server (console app; no GUI)
#   root/                          the portal's filesystem root
#     etc/rnsbox/, etc/reticulum/  config (seeded by the exe on first start)
#     usr/share/rnsbox-portal/pages/  server-rendered page templates
#     www/                         static assets
#
# Run it:
#   ./rnsbox.exe --root ./root            # dev: root next to the exe
#   ./rnsbox.exe --setup                  # install Python/Reticulum if needed
#   ./rnsbox.exe                          # %ProgramData%\RNSBox + browser UI
#
# Requires MSYS2 UCRT64 g++ (x86_64-w64-mingw32-g++) and its OpenSSL, zlib.
set -e

# MSYS2 ships several environments and puts /mingw64/bin first; the UCRT64
# compiler then loads mingw64's runtime DLLs (different CRT) and dies with no
# diagnostic. Put the matching bin dir in front.
export PATH="/c/msys64/ucrt64/bin:$PATH"

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
OUT="$HERE/build"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
STATIC=0
[ "${1:-}" = "--static" ] && STATIC=1

mkdir -p "$OUT"

# The shared CGI core (also compiled into the router image) plus the Windows
# front end. Files that are Linux-only stay out of this list: wifi.cpp,
# console_broker.cpp, nftgen.cpp, conntrack.cpp, routes_wifi.cpp,
# routes_console.cpp, routes_modem.cpp, sysinfo.cpp, mon/.
SOURCES=(
    "$SRC/cgi/http.cpp"
    "$SRC/cgi/json.cpp"
    "$SRC/cgi/util.cpp"
    "$SRC/cgi/render.cpp"
    "$SRC/cgi/session.cpp"
    "$SRC/cgi/store.cpp"
    "$SRC/cgi/web.cpp"
    "$SRC/cgi/dispatch.cpp"
    "$SRC/cgi/updatecheck.cpp"
    "$SRC/cgi/routes_access.cpp"
    "$SRC/cgi/routes_dash.cpp"
    "$SRC/cgi/donate.cpp"
    "$SRC/cgi/routes_donate.cpp"
    "$SRC/cgi/routes_echobot.cpp"
    "$SRC/cgi/routes_net.cpp"
    "$SRC/cgi/routes_reticulum.cpp"
    "$SRC/cgi/routes_settings.cpp"
    "$SRC/cgi/routes_updates.cpp"
    "$SRC/cgi/routes_manual.cpp"
    "$SRC/cgi/routes_map.cpp"
    "$SRC/cgi/routes_rnode.cpp"
    "$SRC/win/compat.cpp"
    "$SRC/win/sysinfo_win.cpp"
    "$SRC/win/supervisor.cpp"
    "$SRC/win/stubs.cpp"
    "$SRC/win/main_win.cpp"
)

CFLAGS=(
    -std=gnu++17 -O2
    -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_POSIX_C_SOURCE=200809L
    -I"$SRC/cgi" -I"$SRC/win"
    -include "$SRC/win/compat.h"
    -ffunction-sections -fdata-sections
)
LDFLAGS=(-Wl,--gc-sections -Wl,-subsystem,console)
LIBS=(-lws2_32 -liphlpapi -lbcrypt -lz)
if [ "$STATIC" = "1" ]; then
    # One self-contained exe (no libcrypto/winpthread DLLs next to it).
    LIBS+=(-Wl,-Bstatic -lcrypto -lstdc++ -lwinpthread -Wl,-Bdynamic -lcrypt32 -luserenv -lgdi32 -lws2_32)
else
    LIBS+=(-lcrypto -lwinpthread)
fi

echo ">> compiling rnsbox.exe"
"$CXX" "${CFLAGS[@]}" "${SOURCES[@]}" -o "$OUT/rnsbox.exe" "${LDFLAGS[@]}" "${LIBS[@]}"

# --- runtime tree -----------------------------------------------------------
echo ">> staging the portal tree"
mkdir -p "$OUT/root/usr/share/rnsbox-portal/pages" \
         "$OUT/root/usr/lib/rnsbox" \
         "$OUT/root/www" \
         "$OUT/root/etc/rnsbox" "$OUT/root/etc/reticulum" \
         "$OUT/root/var/log" "$OUT/root/var/run" "$OUT/root/var/lib/rnsbox"

cp -f "$SRC"/cgi/pages/*.html "$OUT/root/usr/share/rnsbox-portal/pages/"
cp -f "$SRC"/cgi/www/style.css "$SRC"/cgi/www/index.html "$OUT/root/www/"
cp -f "$SRC"/cgi/www/term.js "$OUT/root/www/" 2>/dev/null || true
mkdir -p "$OUT/root/www/xterm" "$OUT/root/www/qr" "$OUT/root/www/flasher"
cp -f "$SRC"/cgi/www/xterm/* "$OUT/root/www/xterm/" 2>/dev/null || true
cp -f "$SRC"/cgi/www/qr/*.svg "$OUT/root/www/qr/" 2>/dev/null || true
cp -r "$SRC"/cgi/www/flasher/. "$OUT/root/www/flasher/" 2>/dev/null || true
cp -f "$SRC"/cgi/helpers/*.py "$OUT/root/usr/lib/rnsbox/" 2>/dev/null || true
cp -f "$SRC"/ca-certificates.crt "$OUT/root/usr/share/rnsbox-portal/" 2>/dev/null || true

if [ "$STATIC" != "1" ]; then
    for dll in libcrypto-3-x64.dll libwinpthread-1.dll; do
        p="$(command -v "$dll" 2>/dev/null || true)"
        [ -z "$p" ] && [ -f "/c/msys64/ucrt64/bin/$dll" ] && p="/c/msys64/ucrt64/bin/$dll"
        [ -n "$p" ] && cp -f "$p" "$OUT/" && echo "   + $(basename "$p")"
    done
fi

echo
echo "built: $OUT/rnsbox.exe"
echo "run:   cd \"$OUT\" && ./rnsbox.exe --root ./root"
echo "       then open http://127.0.0.1:8080/ (admin / admin)"
