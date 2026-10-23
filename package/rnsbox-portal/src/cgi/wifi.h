// wifi.h - onboard WiFi (AIC8800D80 over SDIO) management: config + `iw` parsing.
// Native C++ port of the former wifi.py. No-radio boards: present()==false.
#ifndef RNSBOX_WIFI_H
#define RNSBOX_WIFI_H

#include <string>
#include <map>
#include <vector>

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
struct StaStatus {
    bool connected = false;
    std::string ssid, signal, ip, width, txpower, band, standard, channel, security;
};
// bridge: the bridge the AP iface is a port of ("" = not bridged); ip: where
// its clients reach the box — the bridge's address (the port itself has none).
struct ApStatus  {
    bool active = false;
    std::string iface;
    int clients = 0;
    std::string bridge, ip, width, txpower, band, standard, channel, ssid, security;
};
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
std::string security_label(const std::string& s);

// What the radio can actually do, parsed from `iw phy` at request time: the
// portal renders only the bands/standards/widths/channels this board has
// (e.g. MT7628/mt7603e is 2.4 GHz 802.11n only; no 5 GHz, no ac/ax).
// known=false when iw is absent or the output has no Frequencies blocks; the
// UI then falls back to the full option set (unknown hardware, not a lie).
struct Caps {
    bool known = false;
    bool band24 = false, band5 = false, band6 = false;
    bool ht = false, vht = false, he = false;
    std::vector<int> ch24, ch5;
};
Caps caps();

// Additional STA networks after the primary wifi.conf one (wpa_supplicant
// tries them in order when the higher-priority primary is not available).
struct Network { std::string ssid, psk, security; bool hidden = false; };
std::vector<Network> read_extra_networks();
bool add_extra_network(const Network& n, std::string& err);
bool delete_extra_network(int idx, std::string& err);
bool write_extra_networks(const std::vector<Network>& list, std::string& err);
bool has_sta_networks();

std::string scan_json();     // JSON array of {ssid,signal,channel,band,security}; band "2.4"|"5"|"6"
void apply();                // S35wifi restart + S60routing restart

}  // namespace wifi
#endif
