// halow_ota.cpp — firmware update over HTTP for the HaLow (RNode) modem.
//
// The modem's own web UI (and the RNode_Halow_OTA_Flasher reference) uploads a
// firmware image in 512-byte base64 chunks:
//   POST /api/ota_fw_begin {size, crc32}
//   POST /api/ota_fw_chunk {off, b64}   (repeated)
//   POST /api/ota_fw_end   {}
//   POST /api/reboot       {}
// `ota_fw_end` re-reads the flash and verifies the CRC, so a truncated upload
// is refused instead of booting a corrupt image.
//
// This runs as `rnsbox-portal halow-ota <file>` in the background: the CGI
// assembles the upload under /run in 128 KiB pieces (uhttpd's CGI body cap is
// 256 KiB) and returns immediately; the browser polls the status file. The
// image may be a raw *.bin or an ota.tar with a fw.bin entry (www/ entries are
// ignored — this path updates the firmware only).
#include "routes.h"
#include "modem.h"
#include "store.h"
#include "util.h"
#include "http.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <zlib.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace routes {

namespace {

constexpr const char* OTA_FW_FILE = "/run/rnsbox-halow-ota.fw";
constexpr const char* OTA_STATE   = "/run/rnsbox-halow-ota.json";
constexpr const char* OTA_LOCK    = "/run/rnsbox-halow-ota.lock";
constexpr size_t OTA_CHUNK        = 512;          // the modem UI's chunk size
constexpr uint64_t OTA_MAX        = 8ull * 1024 * 1024;

void status(const char* state, const char* stage, uint64_t done, uint64_t total,
            const std::string& error, const std::string& version) {
#ifdef _WIN32
    (void)state; (void)stage; (void)done; (void)total; (void)error; (void)version;
#else
    uint64_t pct = total ? (done * 100 / total) : 0;
    std::string j =
        "{\"state\":\"" + std::string(state) +
        "\",\"stage\":\"" + http::json_escape(stage) +
        "\",\"done\":" + std::to_string(done) +
        ",\"total\":" + std::to_string(total) +
        ",\"pct\":" + std::to_string(pct) +
        ",\"error\":\"" + http::json_escape(error) +
        "\",\"version\":\"" + http::json_escape(version) + "\"}\n";
    util::write_file_atomic(OTA_STATE, j, 0644);
#endif
}

std::string b64(const unsigned char* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];  o += t[v & 63];
    }
    if (i < n) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

struct Range { uint64_t off = 0, size = 0; };

bool ends_with(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Raw image, or the fw.bin member of an ota.tar (ustar at offset 257).
bool firmware_range(FILE* f, uint64_t file_size, Range& out) {
    char magic[6];
    if (file_size < 512 || fseeko(f, 257, SEEK_SET) != 0 ||
        fread(magic, 1, 5, f) != 5) {
        out.off = 0; out.size = file_size; return file_size > 0;
    }
    magic[5] = '\0';
    if (strncmp(magic, "ustar", 5) != 0) {
        out.off = 0; out.size = file_size;
        return file_size > 0;
    }
    uint64_t pos = 0;
    char hdr[512];
    while (pos + 512 <= file_size) {
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0) return false;
        if (fread(hdr, 1, 512, f) != 512) return false;
        if (hdr[0] == '\0') break;
        char name[101];
        memcpy(name, hdr, 100);
        name[100] = '\0';
        char size_s[13];
        memcpy(size_s, hdr + 124, 12);
        size_s[12] = '\0';
        uint64_t sz = strtoull(size_s, nullptr, 8);
        if (ends_with(name, "fw.bin") || ends_with(name, "/fw.bin")) {
            out.off = pos + 512;
            out.size = sz;
            return sz > 0 && out.off + sz <= file_size;
        }
        pos += 512 + ((sz + 511) & ~511ull);
    }
    return false;
}

bool crc_of(FILE* f, const Range& r, uint32_t& crc) {
    unsigned char buf[4096];
    uLong c = crc32(0L, Z_NULL, 0);
    uint64_t left = r.size;
    if (fseeko(f, (off_t)r.off, SEEK_SET) != 0) return false;
    while (left) {
        size_t n = left < sizeof(buf) ? (size_t)left : sizeof(buf);
        if (fread(buf, 1, n, f) != n) return false;
        c = crc32(c, buf, (uInt)n);
        left -= n;
    }
    crc = (uint32_t)c;
    return true;
}

std::string version_from(const std::string& json) {
    size_t p = json.find("\"ver\"");
    if (p == std::string::npos) return std::string();
    p = json.find(':', p);
    if (p == std::string::npos) return std::string();
    p = json.find('"', p);
    if (p == std::string::npos) return std::string();
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return std::string();
    return json.substr(p + 1, e - p - 1);
}

}  // namespace

int halow_ota_cli(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return 1;
#else
    store::SlipConfig sl = store::read_slip();
    std::string peer = sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip;

    // One updater at a time.
    int lock = open(OTA_LOCK, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (lock < 0) return 1;
    close(lock);

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        status("failed", "open", 0, 0, "cannot open the uploaded image", "");
        unlink(OTA_LOCK);
        return 1;
    }
    fseeko(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(f);
    fseeko(f, 0, SEEK_SET);

    Range r;
    if (size == 0 || size > OTA_MAX || !firmware_range(f, size, r) || r.size == 0) {
        fclose(f);
        status("failed", "image", 0, size,
               "no firmware image found (use a raw .bin, or an ota.tar with fw.bin)", "");
        unlink(OTA_LOCK);
        unlink(path.c_str());
        return 1;
    }

    status("running", "checksum", 0, r.size, "", "");
    uint32_t crc = 0;
    if (!crc_of(f, r, crc)) {
        fclose(f);
        status("failed", "checksum", 0, r.size, "cannot read the firmware image", "");
        unlink(OTA_LOCK); unlink(path.c_str());
        return 1;
    }

    std::string resp, err;
    status("running", "start", 0, r.size, "", "");
    if (!modem::post(peer, "ota_fw_begin",
                     "{\"size\":" + std::to_string(r.size) +
                     ",\"crc32\":" + std::to_string(crc) + "}", resp, 10)) {
        fclose(f);
        status("failed", "start", 0, r.size,
               "the modem did not accept ota_fw_begin (is the link up?)", "");
        unlink(OTA_LOCK); unlink(path.c_str());
        return 1;
    }

    status("running", "upload", 0, r.size, "", "");
    unsigned char buf[OTA_CHUNK];
    uint64_t off = 0;
    int last_pct = -1;
    if (fseeko(f, (off_t)r.off, SEEK_SET) != 0) {
        fclose(f);
        status("failed", "upload", 0, r.size, "seek failed", "");
        unlink(OTA_LOCK); unlink(path.c_str());
        return 1;
    }
    while (off < r.size) {
        size_t n = (size_t)((r.size - off) < OTA_CHUNK ? (r.size - off) : OTA_CHUNK);
        if (fread(buf, 1, n, f) != n) {
            fclose(f);
            status("failed", "upload", off, r.size, "read failed", "");
            unlink(OTA_LOCK); unlink(path.c_str());
            return 1;
        }
        std::string json = "{\"off\":" + std::to_string(off) +
                           ",\"b64\":\"" + b64(buf, n) + "\"}";
        bool sent = false;
        for (int attempt = 0; attempt < 3 && !sent; ++attempt) {
            if (attempt) usleep(80000u << attempt);
            sent = modem::post(peer, "ota_fw_chunk", json, resp, 6);
        }
        if (!sent) {
            fclose(f);
            status("failed", "upload", off, r.size,
                   "chunk at offset " + std::to_string(off) + " was not accepted", "");
            unlink(OTA_LOCK); unlink(path.c_str());
            return 1;
        }
        off += n;
        int pct = (int)(off * 100 / r.size);
        if (pct != last_pct && (pct % 1 == 0)) {
            status("running", "upload", off, r.size, "", "");
            last_pct = pct;
        }
    }
    fclose(f);

    status("running", "verify", r.size, r.size, "", "");
    if (!modem::post(peer, "ota_fw_end", "{}", resp, 120)) {
        status("failed", "verify", r.size, r.size,
               "the modem rejected the image (CRC mismatch or flash write error)", "");
        unlink(OTA_LOCK); unlink(path.c_str());
        return 1;
    }

    status("running", "reboot", r.size, r.size, "", "");
    std::string ignored;
    (void)modem::post(peer, "reboot", "{}", ignored, 2);

    // The modem answers again once the new firmware has booted. Its HTTP
    // server is not reachable during the reboot, so poll.
    std::string version;
    for (int i = 0; i < 60; ++i) {
        usleep(2000000);
        std::string r2;
        if (modem::post(peer, "get_stat", "{}", r2, 3) ||
            modem::get(peer, "get_stat", r2, 3)) {
            version = version_from(r2);
            if (!version.empty()) break;
        }
    }
    status("done", "finished", r.size, r.size, "", version);
    unlink(OTA_LOCK);
    unlink(path.c_str());
    return 0;
#endif
}

}  // namespace routes
