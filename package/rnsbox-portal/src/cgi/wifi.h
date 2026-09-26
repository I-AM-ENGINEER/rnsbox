// wifi.h — onboard WiFi (brcmfmac) management: config + `iw` parsing.
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
void write_conf(const std::map<std::string, std::string>& updates);  // merge + atomic write

// Live radio status (parsed from `iw`). Used both server-side (wifi_page render)
// and, serialized, by the /wifi/status AJAX endpoint.
struct StaStatus { bool connected = false; std::string ssid, signal, ip, width, txpower; };
struct ApStatus  { bool active = false; std::string iface; int clients = 0; std::string ip, width, txpower; };
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

std::string scan_json();     // JSON array of {ssid,signal,channel,band,security}
void apply();                // S35wifi restart + S60routing restart

}  // namespace wifi
#endif
