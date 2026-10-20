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
void reticulum_checkupdate(const http::Request&, http::Response&); // POST PRG (python helper)
void reticulum_applyupdate(const http::Request&, http::Response&); // POST PRG (python helper)
void reticulum_slip(const http::Request&, http::Response&);        // POST PRG
void reticulum_rnsh_save(const http::Request&, http::Response&);   // POST PRG
void reticulum_halow_add(const http::Request&, http::Response&);   // POST PRG
// TCP client interface editor ([[...]] type = TCPClientInterface stanzas)
void reticulum_tcpclient_add(const http::Request&, http::Response&);    // POST PRG
void reticulum_tcpclient_update(const http::Request&, http::Response&); // POST PRG
void reticulum_tcpclient_delete(const http::Request&, http::Response&); // POST PRG
void reticulum_bluetooth(const http::Request&, http::Response&);        // POST PRG
void reticulum_bluetooth_raise(const http::Request&, http::Response&);  // POST PRG
void reticulum_bluetooth_scan(const http::Request&, http::Response&);   // GET  JSON (BLE discovery, rnode.py scan-any)
void reticulum_bluetooth_pair(const http::Request&, http::Response&);   // POST JSON (bluetoothctl pair, generic)
void reticulum_bluetooth_peers(const http::Request&, http::Response&);  // GET  JSON (connected BLE devices)
// Interface byte counters from the parsed rnstatus cache (pretty strings);
// false when no cached snapshot matches the needle (e.g. "BLE").
bool rnstatus_interface_traffic(const char* needle, std::string& tx, std::string& rx);
// RNode modem over USB serial or BLE
void reticulum_rnode(const http::Request&, http::Response&);        // POST PRG (save + apply)
void reticulum_rnode_status(const http::Request&, http::Response&); // GET  JSON (S81rnode state)
void reticulum_rnode_scan(const http::Request&, http::Response&);   // GET  JSON (BLE discovery, rnode.py)
void reticulum_rnode_pair(const http::Request&, http::Response&);   // POST JSON (bluetoothctl pair)


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

// console tab (authenticated one-shot root shell)
void console_page(const http::Request&, http::Response&);          // GET/POST render+run

// settings tab
void settings_page(const http::Request&, http::Response&);         // GET  render
void settings_password(const http::Request&, http::Response&);     // POST PRG
void hostname_set(const http::Request&, http::Response&);          // POST PRG
void time_browser(const http::Request&, http::Response&);          // POST PRG
void ntp_set(const http::Request&, http::Response&);               // POST PRG
void ntp_restart(const http::Request&, http::Response&);           // POST PRG
void ssh_set(const http::Request&, http::Response&);               // POST PRG
void usb_mode_set(const http::Request&, http::Response&);          // POST PRG (USB role device|host)

// echo bot (cards on the Reticulum tab; /echobot redirects to the card)
std::string echobot_cards(const std::string& base);              // cards HTML for the Reticulum page
void echobot_page(const http::Request&, http::Response&);        // GET  -> redirect to the card
void echobot_peers(const http::Request&, http::Response&);       // GET  -> traffic table fragment (AJAX)
void echobot_toggle(const http::Request&, http::Response&);      // POST PRG (Enable switch)
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

}  // namespace routes
#endif
