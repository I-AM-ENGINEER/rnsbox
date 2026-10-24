// token.h — short-lived HMAC tokens for the terminal WebSocket.
//
// The CGI tier is the only place that authenticates a browser session.  After
// that it mints a token bound to the session name; the broker (which listens
// on a TCP port) accepts the token and never sees a portal cookie.  Both sides
// share a per-boot secret under /run, readable only by root.
#ifndef RNSBOX_TERM_TOKEN_H
#define RNSBOX_TERM_TOKEN_H

#include <string>

namespace term {

// Create the secret if it does not exist (0600).  Returns false when the file
// cannot be read or created.
bool ensure_secret(const std::string& path);

// Empty when the secret is unavailable.
std::string make_token(const std::string& secret_path, const std::string& session,
                       int ttl_sec);

// Constant-time validation of make_token()'s output.
bool check_token(const std::string& secret_path, const std::string& session,
                 const std::string& token);

}  // namespace term

#endif  // RNSBOX_TERM_TOKEN_H
