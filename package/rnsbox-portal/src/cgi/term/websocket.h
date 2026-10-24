// websocket.h — minimal RFC 6455 server endpoint used by the fast terminal.
//
// Only what the terminal needs: one handshake per connection, binary/text
// messages, ping/pong, close, and client-side masking.  No extensions, no
// fragmentation on the server side (a message is one frame).
#ifndef RNSBOX_TERM_WEBSOCKET_H
#define RNSBOX_TERM_WEBSOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace term {

std::string url_decode(const std::string& s);
std::string query_param(const std::string& query, const std::string& name);

class WsConn {
public:
    enum class State { HTTP, READY, OPEN, DENIED, FAILED };

    struct Msg {
        bool binary = false;
        std::string data;
    };

    State state = State::HTTP;
    std::string out;        // handshake response followed by queued frames
    size_t out_off = 0;
    std::string path;
    std::string query;
    std::string host;
    std::string origin;
    std::vector<Msg> messages;

    // Feed bytes received from the socket.  Returns false on a protocol error.
    bool feed(const char* p, size_t n);

    // Called by the owner after the HTTP request has been validated.
    void accept();
    void deny(int status, const char* reason);

    bool send_binary(const char* p, size_t n);
    bool send_text(const char* p, size_t n);
    bool send_ping();
    bool send_close(uint16_t code, const char* reason);

    bool open() const { return state == State::OPEN; }
    bool denied() const { return state == State::DENIED; }
    bool closing() const { return close_sent_; }
    bool ready() const { return state == State::READY; }

private:
    bool parse_http();
    bool parse_frames();
    bool send_frame(uint8_t opcode, const char* p, size_t n);
    void fail();

    std::string http_in_;
    std::string frame_in_;
    std::string accept_key_;
    std::string frag_;
    bool frag_active_ = false;
    bool frag_binary_ = false;
    bool close_sent_ = false;
};

}  // namespace term

#endif  // RNSBOX_TERM_WEBSOCKET_H
