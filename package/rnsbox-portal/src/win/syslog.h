// syslog.h — MinGW-w64 ships no <syslog.h>. The shared CGI code has exactly
// one syslog() call (the rnode-watch notice in routes_reticulum.cpp); the
// Windows build has no event-log path worth building for it, so the call
// becomes a no-op. Found via build-win.sh's -I<win dir>, ahead of any
// system syslog.h (there is none).
#pragma once

enum {
    LOG_EMERG = 0, LOG_ALERT, LOG_CRIT, LOG_ERR,
    LOG_WARNING, LOG_NOTICE, LOG_INFO, LOG_DEBUG,
};

static inline void openlog(const char*, int, int) {}
static inline void closelog() {}

static inline void syslog(int, const char* fmt, ...) { (void)fmt; }
