// stubs.cpp — routes that have no Windows backend, plus the wifi module the
// compiled pages still reference.
//
// The router-only features (802.11 radio, the PTY console, the HaLow modem
// proxy) are not ported: Windows has no iw/wpa_supplicant, no PTY broker, and
// the modem is a SLIP-over-serial device. The nav hides them (render.cpp) and
// these handlers answer 503 for a stale link or a typed URL, so the rest of
// the portal stays honest about what this build can do.
#ifdef _WIN32

#include "http.h"
#include "routes.h"
#include "web.h"
#include "wifi.h"

namespace {

void unavailable(http::Response& res, const char* what) {
    res.status = "503 Service Unavailable";
    res.content_type = "text/plain; charset=utf-8";
    res.body = std::string(what) + " is not available in the Windows build "
               "(no radio / PTY / SLIP device). The Reticulum and Dashboard "
               "tabs are fully functional.\n";
}

}  // namespace

namespace wifi {

bool present() { return false; }
Status status() { return Status{}; }
bool has_sta_networks() { return false; }

}  // namespace wifi

namespace routes {

void wifi_page(const http::Request&, http::Response& res)         { unavailable(res, "WiFi"); }
void wifi_scan(const http::Request&, http::Response& res)         { unavailable(res, "WiFi scan"); }
void wifi_status(const http::Request&, http::Response& res)       { unavailable(res, "WiFi status"); }
void wifi_connect(const http::Request&, http::Response& res)      { unavailable(res, "WiFi"); }
void wifi_ap(const http::Request&, http::Response& res)           { unavailable(res, "WiFi AP"); }
void wifi_mode(const http::Request&, http::Response& res)         { unavailable(res, "WiFi"); }
void wifi_txpower(const http::Request&, http::Response& res)      { unavailable(res, "WiFi"); }
void wifi_net_add(const http::Request&, http::Response& res)      { unavailable(res, "WiFi"); }
void wifi_net_delete(const http::Request&, http::Response& res)   { unavailable(res, "WiFi"); }
void wifi_net_connect(const http::Request&, http::Response& res)  { unavailable(res, "WiFi"); }

void console_page(const http::Request&, http::Response& res)      { unavailable(res, "The console"); }
void console_full_page(const http::Request&, http::Response& res) { unavailable(res, "The console"); }
void console_stream(const http::Request&, http::Response& res)    { unavailable(res, "The console"); }
void console_input(const http::Request&, http::Response& res)     { unavailable(res, "The console"); }
void console_resize(const http::Request&, http::Response& res)    { unavailable(res, "The console"); }
void console_ws_token(const http::Request&, http::Response& res)  { unavailable(res, "The console"); }
void nomadnet_page(const http::Request&, http::Response& res)     { unavailable(res, "NomadNet"); }
void nomadnet_full_page(const http::Request&, http::Response& res){ unavailable(res, "NomadNet"); }
void nomadnet_stream(const http::Request&, http::Response& res)   { unavailable(res, "NomadNet"); }
void nomadnet_input(const http::Request&, http::Response& res)    { unavailable(res, "NomadNet"); }
void nomadnet_resize(const http::Request&, http::Response& res)   { unavailable(res, "NomadNet"); }
void nomadnet_ws_token(const http::Request&, http::Response& res) { unavailable(res, "NomadNet"); }

void modem_ui(const http::Request&, http::Response& res)          { unavailable(res, "The HaLow modem page"); }
void modem_api(const http::Request&, http::Response& res)         { unavailable(res, "The HaLow modem API"); }

}  // namespace routes
#endif  // _WIN32
