// dispatch.h — the shared request dispatcher.
//
// Both front ends use it: the CGI binary (uhttpd exec's it per request) and the
// native Windows server (win/main_win.cpp). It applies the request policy
// (Host allowlist, POST-only routes, same-origin check, modem port gate) and
// then calls the route handler, filling `res`. It never writes to the wire.
#ifndef RNSBOX_DISPATCH_H
#define RNSBOX_DISPATCH_H

#include "http.h"

namespace dispatch {

// Fill `res` for `req`. The caller sends it (CGI: stdout; Windows: socket).
void handle(const http::Request& req, http::Response& res);

}  // namespace dispatch
#endif
