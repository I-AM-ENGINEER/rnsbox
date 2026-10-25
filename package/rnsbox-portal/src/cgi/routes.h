// routes.h — feature route handlers (dispatched from main.cpp).
// Pages are server-rendered (render::layout); form POSTs are POST-redirect-GET
// (render::redirect_flash); a few endpoints stay JSON for the pages' AJAX.
#ifndef RNSBOX_ROUTES_H
#define RNSBOX_ROUTES_H

#include "http.h"

namespace routes {

// dashboard tab
void dashboard_page(const http::Request&, http::Response&);        // GET  render (landing)
void dashboard_data(const http::Request&, http::Response&);        // GET  JSON (auto-refresh)

// donate tab
void donate_page(const http::Request&, http::Response&);           // GET  render

// network tab
void network_page(const http::Request&, http::Response&);          // GET  render
void wan_set(const http::Request&, http::Response&);               // POST PRG
void pf_add(const http::Request&, http::Response&);                // POST PRG
void pf_delete(const http::Request&, http::Response&);             // POST PRG
void op_add(const http::Request&, http::Response&);                // POST PRG
void op_delete(const http::Request&, http::Response&);             // POST PRG

// reticulum tab
void reticulum_page(const http::Request&, http::Response&);        // GET  render
void reticulum_status(const http::Request&, http::Response&);      // GET  JSON (?raw=1 = rnstatus)
void reticulum_rnstatus(const http::Request&, http::Response&);   // GET  JSON (parsed, cached rnstatus -a)
void reticulum_halow(const http::Request&, http::Response&);       // GET  JSON (live modem)
void reticulum_config_save(const http::Request&, http::Response&); // POST PRG
void reticulum_restart(const http::Request&, http::Response&);     // POST PRG
void reticulum_autorestart(const http::Request&, http::Response&); // POST PRG
void reticulum_rnsdwatchdog(const http::Request&, http::Response&); // POST PRG
void reticulum_autoupdate(const http::Request&, http::Response&);  // POST PRG
void reticulum_maintenance(const http::Request&, http::Response&); // POST PRG (one form: update+restart+watchdog)
void reticulum_checkupdate(const http::Request&, http::Response&); // POST JSON (python helper)
void reticulum_downloadupdate(const http::Request&, http::Response&); // POST JSON (fetch+verify wheel)
void reticulum_installupdate(const http::Request&, http::Response&);  // POST JSON (install+rollback)
void reticulum_applyupdate(const http::Request&, http::Response&); // POST JSON (legacy alias: install)
void reticulum_slip(const http::Request&, http::Response&);        // POST PRG
void reticulum_rnsh_save(const http::Request&, http::Response&);
void reticulum_rnsh_announce(const http::Request&, http::Response&); // POST JSON   // POST PRG
void reticulum_halow_add(const http::Request&, http::Response&);   // POST PRG
// HaLow firmware update: chunked upload (uhttpd caps a CGI body at 256 KiB),
// background flasher status, and the modem's own online check/install.
void reticulum_halow_ota_chunk(const http::Request&, http::Response&);  // POST raw
void reticulum_halow_ota_start(const http::Request&, http::Response&);  // POST JSON
void reticulum_halow_ota_status(const http::Request&, http::Response&); // GET JSON
void reticulum_halow_ota_online(const http::Request&, http::Response&); // POST JSON
void reticulum_halow_ota_releases(const http::Request&, http::Response&); // GET JSON (GitHub)
void reticulum_halow_ota_fetch(const http::Request&, http::Response&);    // POST JSON (download+start)
int halow_ota_cli(const std::string& path, const std::string& peer_ip); // CLI background
// TCP client interface editor ([[...]] type = TCPClientInterface stanzas)
void reticulum_tcpclient_add(const http::Request&, http::Response&);    // POST PRG
void reticulum_tcpclient_update(const http::Request&, http::Response&); // POST PRG
void reticulum_tcpclient_save(const http::Request&, http::Response&);   // POST PRG
void reticulum_tcpclient_delete(const http::Request&, http::Response&); // POST PRG
void reticulum_bluetooth(const http::Request&, http::Response&);        // POST PRG
void reticulum_bluetooth_raise(const http::Request&, http::Response&);  // POST PRG
void reticulum_bluetooth_scan(const http::Request&, http::Response&);   // GET  JSON (BLE discovery, rnode.py scan-any)
void reticulum_bluetooth_pair(const http::Request&, http::Response&);   // POST JSON (bluetoothctl pair, generic)
void reticulum_bluetooth_peers(const http::Request&, http::Response&);  // GET  JSON (connected BLE devices)
// Interface byte counters from the parsed rnstatus cache (pretty strings);
// false when no cached snapshot matches the needle (e.g. "BLE").
bool rnstatus_interface_traffic(const char* needle, std::string& tx, std::string& rx);
// Byte counters written by BLEInterface every 2 s (mesh + RNode NUS);
// false when the snapshot is missing or stale.
bool ble_traffic_stats(std::string& tx, std::string& rx);
// Rebuild the BLE peers/traffic cache now (rnsbox-mon and the CGI).
bool bt_peers_refresh();
// Rebuild the parsed rnstatus cache now (boot warm-up); false if rnstatus
// produced no parseable output.
bool rnstatus_prewarm();
// RNode modem over USB serial or BLE
void reticulum_rnode(const http::Request&, http::Response&);        // POST PRG (save + apply)
void reticulum_rnode_status(const http::Request&, http::Response&); // GET  JSON (S81rnode state)
// True when the RNode interface is up in the rnstatus snapshot (rnsd has
// actually attached the modem, not just written the config).
bool rnode_live_up(std::string* name = nullptr);
// rnsbox-mon: drop a stale BlueZ connection that keeps a bonded RNode from
// advertising, so RNodeInterface's scan-only discovery can find it again.
bool rnode_watch();
void reticulum_rnode_scan(const http::Request&, http::Response&);   // GET JSON (BLE discovery)
void rnode_scan_watch_cli(const std::string& path);                 // detached progressive scan
void reticulum_rnode_pair(const http::Request&, http::Response&);   // POST JSON (bluetoothctl pair)
void reticulum_rnode_forget(const http::Request&, http::Response&); // POST JSON (remove BlueZ bond)


// updates tab (A/B whole-OS updater)
void updates_page(const http::Request&, http::Response&);         // GET  render
void updates_data(const http::Request&, http::Response&);         // GET  JSON (AJAX)
void updates_config(const http::Request&, http::Response&);       // POST PRG
void updates_check(const http::Request&, http::Response&);        // POST PRG
void updates_apply(const http::Request&, http::Response&);        // POST PRG
void updates_auto(const http::Request&, http::Response&);         // POST PRG

// remote access (rnsh issued key files — cards on the Reticulum tab)
std::string access_cards(const std::string& base);                 // cards HTML for the Reticulum page
void access_page(const http::Request&, http::Response&);           // GET  -> redirect to the cards
void access_issue(const http::Request&, http::Response&);          // POST -> text/plain download
void access_revoke(const http::Request&, http::Response&);         // POST PRG
void access_reissue(const http::Request&, http::Response&);        // POST -> text/plain download

// terminal tabs (xterm.js + chunked HTTP stream to the local PTY broker).
// The full pages are the same terminal without the portal chrome, opened in
// their own browser tab ("Open in new tab" on the normal page).
void console_page(const http::Request&, http::Response&);          // GET  render
void console_full_page(const http::Request&, http::Response&);     // GET  render (no chrome)
void console_stream(const http::Request&, http::Response&);        // GET  raw VT output stream
void console_input(const http::Request&, http::Response&);         // POST raw key bytes -> PTY
void console_resize(const http::Request&, http::Response&);        // POST cols/rows -> PTY
void console_ws_token(const http::Request&, http::Response&);      // GET  mint WS token
void nomadnet_page(const http::Request&, http::Response&);         // GET  render
void nomadnet_full_page(const http::Request&, http::Response&);    // GET  render (no chrome)
void nomadnet_stream(const http::Request&, http::Response&);       // GET  raw VT output stream
void nomadnet_input(const http::Request&, http::Response&);        // POST raw key bytes -> PTY
void nomadnet_resize(const http::Request&, http::Response&);       // POST cols/rows -> PTY
void nomadnet_ws_token(const http::Request&, http::Response&);     // GET  mint WS token

// bundled Reticulum manual (Markdown rendered on demand)
void manual_page(const http::Request&, http::Response&);           // GET  render
void manual_check(const http::Request&, http::Response&);          // POST PRG
void manual_download(const http::Request&, http::Response&);       // POST PRG
void manual_progress(const http::Request&, http::Response&);     // GET  JSON progress

// RNode tab: rnodeconf + live counters + display + pairing
void rnode_page(const http::Request&, http::Response&);            // GET  render
void rnode_serial(const http::Request&, http::Response&);          // GET  JSON port list
void rnode_action(const http::Request&, http::Response&);          // GET  JSON action result

// RNode HaLow flasher tab (factory HGIC devices over raw Ethernet + RNode)
void flasher_page(const http::Request&, http::Response&);          // GET  render
void flasher_scan(const http::Request&, http::Response&);          // GET  JSON devices
void flasher_status(const http::Request&, http::Response&);        // GET  JSON progress
void flasher_action(const http::Request&, http::Response&);        // POST JSON (flash/reboot/ip/format)
void flasher_fetch(const http::Request&, http::Response&);         // POST JSON (GitHub download)
void flasher_autoupdate(const http::Request&, http::Response&);    // GET status / POST toggle

// Map tab: announced nodes with coordinates (offline render)
void map_page(const http::Request&, http::Response&);              // GET  render
void map_nodes(const http::Request&, http::Response&);             // GET  JSON nodes
void map_telemetry(const http::Request&, http::Response&);         // POST PRG (location + paths)
void map_nodes_cli();                                              // CLI  JSON nodes

// settings tab
void settings_page(const http::Request&, http::Response&);         // GET  render
void settings_password(const http::Request&, http::Response&);     // POST PRG
void hostname_set(const http::Request&, http::Response&);          // POST PRG
void time_browser(const http::Request&, http::Response&);          // POST PRG
void ntp_set(const http::Request&, http::Response&);               // POST PRG
void ntp_restart(const http::Request&, http::Response&);           // POST PRG
void rnstime_set(const http::Request&, http::Response&);           // POST PRG (rns-time config)
void rnstime_sync(const http::Request&, http::Response&);          // POST PRG (one-off client run)
void ssh_set(const http::Request&, http::Response&);               // POST PRG
void usb_mode_set(const http::Request&, http::Response&);          // POST PRG (USB role device|host)
void settings_uart(const http::Request&, http::Response&);         // POST PRG (UART functions)

// echo bot (cards on the Reticulum tab; /echobot redirects to the card)
std::string echobot_cards(const std::string& base);              // cards HTML for the Reticulum page
std::string echobot_dashboard_card(const std::string& base);     // compact Dashboard card (enabled only)
void echobot_page(const http::Request&, http::Response&);        // GET  -> redirect to the card
void echobot_peers(const http::Request&, http::Response&);       // GET  -> traffic table fragment (AJAX)
void echobot_toggle(const http::Request&, http::Response&);      // POST PRG (legacy Enable switch)
void echobot_save(const http::Request&, http::Response&);        // POST PRG
void echobot_announce(const http::Request&, http::Response&);    // POST PRG
void echobot_restart(const http::Request&, http::Response&);     // POST PRG

// HaLow modem web-UI reverse proxy (SLIP)
void modem_ui(const http::Request&, http::Response&);              // GET  proxy modem root
void modem_api(const http::Request&, http::Response&);             // GET/POST proxy /api/*

// wifi tab (W variant)
void wifi_page(const http::Request&, http::Response&);             // GET  render
void wifi_scan(const http::Request&, http::Response&);             // GET  JSON (AJAX)
void wifi_status(const http::Request&, http::Response&);           // GET  JSON (AJAX)
void wifi_connect(const http::Request&, http::Response&);          // POST PRG
void wifi_ap(const http::Request&, http::Response&);               // POST PRG
void wifi_mode(const http::Request&, http::Response&);             // POST PRG
void wifi_txpower(const http::Request&, http::Response&);          // POST PRG
void wifi_net_add(const http::Request&, http::Response&);          // POST PRG
void wifi_net_delete(const http::Request&, http::Response&);       // POST PRG
void wifi_net_connect(const http::Request&, http::Response&);      // POST PRG

}  // namespace routes
#endif
