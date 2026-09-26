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
void reticulum_halow(const http::Request&, http::Response&);       // GET  JSON (live modem)
void reticulum_config_save(const http::Request&, http::Response&); // POST PRG
void reticulum_restart(const http::Request&, http::Response&);     // POST PRG
void reticulum_autorestart(const http::Request&, http::Response&); // POST PRG
void reticulum_autoupdate(const http::Request&, http::Response&);  // POST PRG
void reticulum_checkupdate(const http::Request&, http::Response&); // POST PRG (python helper)
void reticulum_applyupdate(const http::Request&, http::Response&); // POST PRG (python helper)
void reticulum_slip(const http::Request&, http::Response&);        // POST PRG
void reticulum_halow_add(const http::Request&, http::Response&);   // POST PRG

// settings tab
void settings_page(const http::Request&, http::Response&);         // GET  render
void settings_password(const http::Request&, http::Response&);     // POST PRG
void hostname_set(const http::Request&, http::Response&);          // POST PRG
void time_browser(const http::Request&, http::Response&);          // POST PRG
void ntp_set(const http::Request&, http::Response&);               // POST PRG
void ntp_restart(const http::Request&, http::Response&);           // POST PRG

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
