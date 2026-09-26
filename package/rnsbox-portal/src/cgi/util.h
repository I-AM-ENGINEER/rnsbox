// util.h — filesystem + SAFE subprocess helpers for the portal CGI.
//
// Shell-outs NEVER go through /bin/sh: run() takes an argv vector and
// execve()s directly, so attacker-controlled form values can't inject
// commands even if one is passed as an argument.
#ifndef RNSBOX_UTIL_H
#define RNSBOX_UTIL_H

#include <string>
#include <vector>

namespace util {

// Read a whole file (up to max_bytes) into a string; empty string if missing.
std::string read_file(const std::string& path, size_t max_bytes = 1 << 20);

// Atomic write: write to path.tmp then rename. Returns true on success.
bool write_file_atomic(const std::string& path, const std::string& data, int mode = 0644);

struct RunResult {
    int  exit_code = -1;   // process exit status, or -1 on spawn failure / timeout kill
    bool timed_out = false;
    std::string out;       // captured stdout (+stderr merged), capped
};

// Run argv[0] with argv, feeding `input` on stdin, killing it after timeout_s.
// NO shell. stdout+stderr merged, capped at out_cap bytes. argv[0] should be an
// absolute path (no PATH search of attacker-influenced names).
RunResult run(const std::vector<std::string>& argv,
              const std::string& input = "",
              int timeout_s = 10,
              size_t out_cap = 256 * 1024);

// Trim ASCII whitespace.
std::string trim(const std::string& s);

// Read one `key = value` (or `key=value`) line from a flat config file.
std::string conf_get(const std::string& path, const std::string& key);

// True if s contains no CR/LF/NUL/control chars (defense for values we write
// back into flat config files — blocks config-injection).
bool is_single_line_clean(const std::string& s);

// Lowercase hex of raw bytes.
std::string to_hex(const std::string& raw);

// n random bytes from /dev/urandom, hex-encoded (2n chars); empty on failure.
std::string random_hex(size_t nbytes);

// Constant-time string equality (avoids a timing oracle on token/hash compares).
bool const_time_eq(const std::string& a, const std::string& b);

}  // namespace util
#endif
