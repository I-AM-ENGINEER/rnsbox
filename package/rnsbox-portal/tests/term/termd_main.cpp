// termd_main.cpp — host-side entry point for the broker under test.
//
// Usage: termd [ws-port] [bind]
//   ws-port 0 disables the WebSocket fast path (legacy-only smoke test).
#include <cstdlib>

#include "console.h"

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 0;
    const char* bind = (argc > 2) ? argv[2] : "127.0.0.1";
    return console::broker_main(true, port, bind);
}
