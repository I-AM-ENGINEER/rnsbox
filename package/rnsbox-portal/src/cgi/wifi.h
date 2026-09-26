// wifi.h - onboard WiFi (brcmfmac) management: config + `iw` parsing.
// Native C++ port of the former wifi.py. No-radio boards: present()==false.
#ifndef RNSBOX_WIFI_H
#define RNSBOX_WIFI_H

#include <string>
#include <map>

namespace wifi {

constexpr const char* CONF   = "/etc/rnsbox/wifi.conf";
constexpr const char* STA_IF = "wlan0";

bool present();                                       // any wlan* in /sys/class/net
std::map<std::string, std::string> read_conf();       // keys + defaults
// Merge + atomic write, mode 0600 (the file holds sta_psk/ap_psk; an existing
// stricter mode is kept). Returns false — and writes NOTHING — if any value
// fails conf_value_ok() or the write fails.
bool write_conf(const std::map<std::string, std::string>& updates);

// Can `s` round-trip through wifi.conf as one value? Both readers (read_conf()
// and S35wifi's cfg()) cut a value at '#', drop '"' and trim surrounding
// whitespace, and a CR/LF (or any control char) would inject a whole extra
// key=value line — so any of those would silently change an SSID/password.
bool conf_value_ok(const std::string& s);

// Hotspot WPA2 passphrase policy — mirrored by S35wifi's ap_psk_ok(): 8..63
// printable ASCII, storable (conf_value_ok), and not the old published factory
// default (anyone could read it in the source).
constexpr const char* OLD_DEFAULT_AP_PSK = "changeme123";
bool ap_psk_ok(const std::string& psk);

// Live radio status (parsed from `iw`). Used both server-side (wifi_page render)
// and, serialized, by the /wifi/status AJAX endpoint.
struct StaStatus { bool connected = false; std::string ssid, signal, ip, width, txpower; };
// bridge: the bridge the AP iface is a port of ("" = not bridged); ip: where
// its clients reach the box — the bridge's address (the port itself has none).
struct ApStatus  { bool active = false; std::string iface; int clients = 0; std::string bridge, ip, width, txpower; };
struct Status {
    bool present = false;
    std::string mode;
    bool concurrent = false;
    std::string tx_power;
    StaStatus sta;
    ApStatus ap;
};
Status status();             // gather live status
std::string status_json();   // status() serialized to JSON (present/mode/sta/ap/...)

std::string scan_json();     // JSON array of {ssid,signal,channel,band,security}; band "2.4"|"5"|"6"
void apply();                // S35wifi restart + S60routing restart

}  // namespace wifi
#endif
