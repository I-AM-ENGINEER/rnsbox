// updatecheck.h — cheap read-only view of the rnsd (rns package) update state,
// for the Dashboard + Reticulum page renders. The EXPENSIVE parts (querying
// PyPI, applying a pip update) stay in the python helper
// /usr/lib/rnsbox/updatecheck.py, invoked on-demand by the Check/Apply buttons.
// display() does NO network + NO python spawn: it reads the installed version
// from RNS/_version.py (else the highest rns-* dist-info/egg-info dir name —
// stale metadata can sit next to the real one) and the last check's result
// from the state file that the cron helper writes.
#ifndef RNSBOX_UPDATECHECK_H
#define RNSBOX_UPDATECHECK_H

#include <string>

namespace updatecheck {

struct State {
    std::string installed;          // "1.5.2", or "" if not determinable
    std::string latest;             // last-checked PyPI version, or "" if never checked
    bool update_available = false;  // latest > installed
    std::string error;              // last check's error class, "" if none
    long checked_at = 0;            // epoch of last check, 0 if never
};

State display();

}  // namespace updatecheck
#endif
