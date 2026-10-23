// supervisor.h — the Windows replacement for the /etc/init.d/S* scripts.
//
// On the router the portal restarts rnsd, lxmd, nomadnet and the echo bot by
// exec'ing an init script. Windows has no sysvinit: one supervisor process
// (this one) owns those children, keeps their PIDs, and answers the same
// "start / stop / restart / kick" verbs. Only rnsd is supervised for now; the
// others are started lazily by the routes that need them.
#ifndef RNSBOX_WIN_SUPERVISOR_H
#define RNSBOX_WIN_SUPERVISOR_H

#include <string>

namespace supervisor {

// Path of the running child's log file (under <root>/var/log).
std::string log_path(const char* name);

// Start rnsd if it is not already running. Returns true when it is running.
// `cfg_dir` is the absolute (unmapped) Reticulum config directory.
bool rnsd_start(std::string* error = nullptr);
bool rnsd_stop(std::string* error = nullptr);
bool rnsd_restart(std::string* error = nullptr);

struct RnsdState {
    bool running = false;
    long pid = 0;
    long uptime_s = -1;
};
RnsdState rnsd_state();

// Map one of the router's init-script verbs onto the supervisor. `name` is an
// S* script name ("S82rnsd"); anything else is a no-op success (there is no
// such service on Windows, and the route only reports "applied").
bool run_init(const char* name, const char* arg);

// Install the environment children need (PYTHONUNBUFFERED, RNSBOX_ROOT).
void prepare_env();

// The interpreter Reticulum runs under ("" when none was found). Filled at
// startup by the bootstrap.
extern std::string python;

}  // namespace supervisor
#endif
