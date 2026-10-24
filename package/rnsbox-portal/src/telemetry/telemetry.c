/*
 * rnsbox-telemetry — minimal MQTT telemetry proxy for the HaLow (RNode) modem.
 *
 * The modem's telemetry publisher (RNode_Halow_Firmware src/telemetry.c) is a
 * plain MQTT 3.1.1 client: it connects to <host>:<port> with
 * user/password, publishes one JSON object (QoS 1, retain 0) on
 * "rnode-halow/telemetry" and disconnects. When the Map tab enables the
 * "Publish via HaLow modem" path, the portal points that host at this box.
 *
 * This daemon is the receiving broker for that one flow and the forwarding
 * client to the real map server:
 *   modem --(SLIP or LAN)--> rnsbox-telemetry:1883 --> telemetry.rnode-halow.ru
 *
 * For every PUBLISH it
 *   1. writes a normalized snapshot to /run/rnsbox/telemetry/halow.json
 *      (RAM only — this is a proxy, not a data logger; the Map tab reads it
 *      to draw the real node), and
 *   2. forks a short-lived child that forwards the original packet upstream
 *      with the same topic/payload, so rnode-halow.ru sees exactly what the
 *      modem would have sent directly.
 *
 * No TLS, no JSON library, no threads: one small process with a fixed set of
 * ~4 KB buffers. Memory is the binding constraint on the CVITEK boards.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define CONF_FILE       "/etc/rnsbox/telemetry.conf"
#define STATE_DIR       "/run/rnsbox/telemetry"
#define STATE_FILE      STATE_DIR "/halow.json"
#define BROKER_PORT     1883
#define MAX_PACKET      (64 * 1024)
#define MAX_CLIENTS     4
#define MQTT_ID_LEN     64

struct conf {
    int enabled;
    char upstream[128];
    char upstream_port[8];
    char user[32];
    char pass[32];
    char topic[64];
};

static int cfg_bool(const char *v) {
    return v && (!strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
                 !strcmp(v, "1") || !strcasecmp(v, "on"));
}

static void read_conf(struct conf *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->upstream, sizeof(c->upstream), "telemetry.rnode-halow.ru");
    snprintf(c->upstream_port, sizeof(c->upstream_port), "1883");
    snprintf(c->user, sizeof(c->user), "rnode-halow");
    snprintf(c->pass, sizeof(c->pass), "rnode-halow");
    snprintf(c->topic, sizeof(c->topic), "rnode-halow/telemetry");

    FILE *f = fopen(CONF_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = s, *v = eq + 1;
        char *e = k + strlen(k);
        while (e > k && isspace((unsigned char)e[-1])) *--e = '\0';
        while (*v == ' ' || *v == '\t' || *v == '"' || *v == '\'') v++;
        e = v + strlen(v);
        while (e > v && isspace((unsigned char)e[-1])) *--e = '\0';
        while (e > v && (e[-1] == '"' || e[-1] == '\'')) *--e = '\0';

        if (!strcmp(k, "halow_mqtt")) c->enabled = cfg_bool(v);
        else if (!strcmp(k, "halow_upstream") && *v) {
            snprintf(c->upstream, sizeof(c->upstream), "%s", v);
        } else if (!strcmp(k, "halow_upstream_port") && *v) {
            snprintf(c->upstream_port, sizeof(c->upstream_port), "%s", v);
        } else if (!strcmp(k, "halow_user") && *v) {
            snprintf(c->user, sizeof(c->user), "%s", v);
        } else if (!strcmp(k, "halow_pass") && *v) {
            snprintf(c->pass, sizeof(c->pass), "%s", v);
        }
    }
    fclose(f);
}

/* ---------------- tiny JSON readers (the modem's payload shape) ----------- */

static const char *json_find(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int json_str(const char *json, const char *key, char *out, size_t n) {
    const char *p = json_find(json, key);
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case 'u': {
                /* Keep non-ASCII names readable-ish: decode the 4 hex digits
                 * only when they fit in a byte, otherwise emit '?'. */
                unsigned v = 0;
                int ok = 1;
                for (int k = 1; k <= 4; k++) {
                    if (!isxdigit((unsigned char)p[k])) { ok = 0; break; }
                    v = v * 16 + (unsigned)(isdigit((unsigned char)p[k])
                              ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10));
                }
                if (ok && v && v < 0x80) out[i++] = (char)v;
                else out[i++] = '?';
                p += 4;
                break;
            }
            default: out[i++] = *p; break;
            }
            p++;
        } else {
            if ((unsigned char)*p < 0x20) { p++; continue; }
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return 1;
}

static int json_num(const char *json, const char *key, double *out) {
    const char *p = json_find(json, key);
    if (!p) return 0;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

/* "864.5 MHz" / "1 MHz" -> Hz. Anything unexpected yields 0. */
static double mhz_to_hz(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (strncasecmp(end, "MHz", 3) == 0) return v * 1000000.0;
    if (strncasecmp(end, "kHz", 3) == 0) return v * 1000.0;
    if (strncasecmp(end, "Hz", 2) == 0) return v;
    return 0;
}

static void json_escape_into(char *out, size_t n, const char *s) {
    size_t i = 0;
    for (; s && *s && i + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
        else if (c < 0x20) { i += (size_t)snprintf(out + i, n - i, "\\u%04x", c); }
        else out[i++] = (char)c;
    }
    out[i] = '\0';
}

/* ---------------- state file ------------------------------------------------ */

static int store_state(const char *source, const char *payload) {
    char name[128] = "";
    char fw[64] = "";
    char loc[64] = "";
    char freq_s[64] = "";
    char bw_s[64] = "";
    char lxmf[64] = "";
    double lat = 0, lon = 0, uptime = 0;

    json_str(payload, "name", name, sizeof(name));
    json_str(payload, "fw", fw, sizeof(fw));
    json_str(payload, "loc", loc, sizeof(loc));
    json_str(payload, "freq", freq_s, sizeof(freq_s));
    json_str(payload, "bw", bw_s, sizeof(bw_s));
    json_str(payload, "lxmf", lxmf, sizeof(lxmf));
    if (!json_num(payload, "uptime", &uptime)) uptime = 0;

    int have_loc = 0;
    if (loc[0]) {
        char *comma = strchr(loc, ',');
        if (comma) {
            char *end = NULL;
            double la = strtod(loc, &end);
            double lo = strtod(comma + 1, &end);
            if (la >= -90 && la <= 90 && lo >= -180 && lo <= 180 &&
                !(la == 0 && lo == 0)) {
                lat = la; lon = lo; have_loc = 1;
            }
        }
    }

    char esc_name[256], esc_fw[128], esc_lxmf[128];
    json_escape_into(esc_name, sizeof(esc_name), name);
    json_escape_into(esc_fw, sizeof(esc_fw), fw);
    json_escape_into(esc_lxmf, sizeof(esc_lxmf), lxmf);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"received\":%lld,\"source\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\","
        "\"lat\":%.6f,\"lon\":%.6f,\"has_loc\":%s,"
        "\"freq_hz\":%.0f,\"bw_hz\":%.0f,\"lxmf\":\"%s\",\"uptime\":%.0f}\n",
        (long long)time(NULL), source, esc_name, esc_fw, lat, lon,
        have_loc ? "true" : "false",
        mhz_to_hz(freq_s), mhz_to_hz(bw_s), esc_lxmf, uptime);

    mkdir("/run/rnsbox", 0755);
    mkdir(STATE_DIR, 0755);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), STATE_FILE ".tmp.%ld", (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(body), off = 0;
    while (off < len) {
        ssize_t w = write(fd, body + off, len - off);
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    close(fd);
    if (rename(tmp, STATE_FILE) != 0) { unlink(tmp); return -1; }
    return have_loc ? 0 : 1;
}

/* ---------------- forwarding (one short-lived child per packet) ----------- */

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int __attribute__((unused)) read_packet(int fd, unsigned char *buf, size_t cap) {
    /* Blocking with a 5 s receive timeout (set by the caller). Returns the
     * packet length or -1. */
    size_t got = 0;
    while (got < 2) {
        ssize_t r = recv(fd, buf + got, cap - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    size_t mult = 1, rem = 0, at = 1;
    do {
        if (at >= got) {
            ssize_t r = recv(fd, buf + got, cap - got, 0);
            if (r <= 0) return -1;
            got += (size_t)r;
        }
        unsigned char b = buf[at++];
        rem += (b & 0x7f) * mult;
        mult *= 128;
        if (mult > 128 * 128 * 128) return -1;
    } while (buf[at - 1] & 0x80);
    size_t total = at + rem;
    if (total > cap) return -1;
    while (got < total) {
        ssize_t r = recv(fd, buf + got, total - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (int)total;
}

static int mqtt_connect_packet(unsigned char *out, size_t cap,
                               const char *client_id, const char *user,
                               const char *pass) {
    size_t cid = strlen(client_id), us = user ? strlen(user) : 0;
    size_t ps = pass ? strlen(pass) : 0;
    size_t rem = 10 + 2 + cid;
    unsigned char flags = 0x02;   /* clean session */
    if (us) { rem += 2 + us; flags |= 0x80; }
    if (ps) { rem += 2 + ps; flags |= 0x40; }
    if (cap < rem + 2) return -1;
    size_t i = 0;
    out[i++] = 0x10;
    if (rem < 128) out[i++] = (unsigned char)rem;
    else { out[i++] = (unsigned char)(rem % 128 + 128); out[i++] = (unsigned char)(rem / 128); }
    out[i++] = 0; out[i++] = 4;
    memcpy(out + i, "MQTT", 4); i += 4;
    out[i++] = 4;      /* protocol level 3.1.1 */
    out[i++] = flags;
    out[i++] = 0; out[i++] = 30;   /* keepalive */
    out[i++] = (unsigned char)(cid >> 8); out[i++] = (unsigned char)(cid & 0xff);
    memcpy(out + i, client_id, cid); i += cid;
    if (us) {
        out[i++] = (unsigned char)(us >> 8); out[i++] = (unsigned char)(us & 0xff);
        memcpy(out + i, user, us); i += us;
    }
    if (ps) {
        out[i++] = (unsigned char)(ps >> 8); out[i++] = (unsigned char)(ps & 0xff);
        memcpy(out + i, pass, ps); i += ps;
    }
    return (int)i;
}

static int forward_packet(const struct conf *c, const char *topic,
                          const char *payload) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c->upstream, c->upstream_port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) { fcntl(fd, F_SETFL, flags); break; }
        if (errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv = { 5, 0 };
            if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;
                }
            }
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    unsigned char pkt[512];
    char cid[MQTT_ID_LEN];
    snprintf(cid, sizeof(cid), "rnsbox-%ld", (long)getpid());
    int n = mqtt_connect_packet(pkt, sizeof(pkt), cid, c->user, c->pass);
    if (n < 0 || write_all(fd, pkt, (size_t)n) != 0) { close(fd); return -1; }
    unsigned char ack[4];
    if (recv(fd, ack, sizeof(ack), 0) < 4 || ack[0] != 0x20 || ack[3] != 0) {
        close(fd); return -1;
    }

    size_t tl = strlen(topic), pl = strlen(payload);
    size_t rem = 2 + tl + pl;
    unsigned char hdr[8];
    size_t hn = 0;
    hdr[hn++] = 0x30;   /* PUBLISH, QoS 0 */
    if (rem < 128) hdr[hn++] = (unsigned char)rem;
    else if (rem < 16384) {
        hdr[hn++] = (unsigned char)((rem & 0x7f) | 0x80);
        hdr[hn++] = (unsigned char)(rem >> 7);
    } else { close(fd); return -1; }
    unsigned char tlen[2] = { (unsigned char)(tl >> 8), (unsigned char)(tl & 0xff) };
    if (write_all(fd, hdr, hn) != 0 ||
        write_all(fd, tlen, 2) != 0 ||
        write_all(fd, topic, tl) != 0 ||
        write_all(fd, payload, pl) != 0) { close(fd); return -1; }
    unsigned char disc[2] = { 0xe0, 0x00 };
    write_all(fd, disc, 2);
    close(fd);
    return 0;
}

/* ---------------- MQTT server --------------------------------------------- */

struct client {
    int fd;
    int authed;
    unsigned char *buf;
    size_t len, cap;
};

static void client_close(struct client *cl) {
    if (cl->fd >= 0) close(cl->fd);
    free(cl->buf);
    memset(cl, 0, sizeof(*cl));
    cl->fd = -1;
}

static int send_connack(struct client *cl, unsigned char rc) {
    unsigned char ack[4] = { 0x20, 0x02, 0x00, rc };
    return (int)send(cl->fd, ack, sizeof(ack), 0);
}

static int send_puback(struct client *cl, unsigned short id) {
    unsigned char ack[4] = { 0x40, 0x02, (unsigned char)(id >> 8), (unsigned char)(id & 0xff) };
    return (int)send(cl->fd, ack, sizeof(ack), 0);
}

static const char *peer_ip(int fd, char *out, size_t n) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) { snprintf(out, n, "?"); return out; }
    void *addr = (ss.ss_family == AF_INET)
        ? (void *)&((struct sockaddr_in *)&ss)->sin_addr
        : (void *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
    if (!inet_ntop(ss.ss_family, addr, out, (socklen_t)n)) snprintf(out, n, "?");
    return out;
}

/* Parse CONNECT: return 0 when accepted, otherwise the CONNACK code. */
static int handle_connect(struct client *cl, const unsigned char *body, size_t n,
                          const struct conf *c, char *client_id, size_t cid_n) {
    (void)cl;
    if (n < 10) return 0x01;
    size_t p = 0;
    unsigned plen = (unsigned)(body[p] << 8 | body[p + 1]); p += 2;
    if (plen + p + 4 > n) return 0x01;
    p += plen;                       /* protocol name */
    p += 1;                          /* level */
    unsigned char flags = body[p++];
    p += 2;                          /* keepalive */
    if (p + 2 > n) return 0x01;
    unsigned cid_len = (unsigned)(body[p] << 8 | body[p + 1]); p += 2;
    if (p + cid_len > n) return 0x01;
    if (client_id && cid_n) {
        size_t k = cid_len < cid_n - 1 ? cid_len : cid_n - 1;
        memcpy(client_id, body + p, k);
        client_id[k] = '\0';
    }
    p += cid_len;
    if (flags & 0x04) {              /* will topic + message */
        if (p + 2 > n) return 0x01;
        unsigned wl = (unsigned)(body[p] << 8 | body[p + 1]); p += 2 + wl;
        if (p + 2 > n) return 0x01;
        unsigned ml = (unsigned)(body[p] << 8 | body[p + 1]); p += 2 + ml;
    }
    char user[64] = "", pass[64] = "";
    if (flags & 0x80) {
        if (p + 2 > n) return 0x01;
        unsigned ul = (unsigned)(body[p] << 8 | body[p + 1]); p += 2;
        if (p + ul > n) return 0x01;
        size_t k = ul < sizeof(user) - 1 ? ul : sizeof(user) - 1;
        memcpy(user, body + p, k); user[k] = '\0';
        p += ul;
    }
    if (flags & 0x40) {
        if (p + 2 > n) return 0x04;
        unsigned pl = (unsigned)(body[p] << 8 | body[p + 1]); p += 2;
        if (p + pl > n) return 0x01;
        size_t k = pl < sizeof(pass) - 1 ? pl : sizeof(pass) - 1;
        memcpy(pass, body + p, k); pass[k] = '\0';
        p += pl;
    }
    /* The portal configures the modem with these credentials; requiring them
     * keeps other LAN devices from injecting map points. */
    if (strcmp(user, c->user) != 0 || strcmp(pass, c->pass) != 0) return 0x05;
    return 0;
}

static void handle_packet(struct client *cl, const unsigned char *pkt, size_t n,
                          const struct conf *c) {
    unsigned type = pkt[0] >> 4;
    size_t at = 1;
    size_t rem = 0, mult = 1;
    unsigned char b;
    do {
        if (at >= n) return;
        b = pkt[at++];
        rem += (b & 0x7f) * mult;
        mult *= 128;
    } while (b & 0x80);
    const unsigned char *body = pkt + at;
    if (at + rem > n) return;

    switch (type) {
    case 1: {   /* CONNECT */
        char cid[MQTT_ID_LEN] = "";
        int rc = handle_connect(cl, body, rem, c, cid, sizeof(cid));
        send_connack(cl, (unsigned char)rc);
        cl->authed = (rc == 0);
        if (rc == 0) syslog(LOG_INFO, "modem connected (%s, id=%s)", cid, cid);
        break;
    }
    case 3: {   /* PUBLISH */
        if (!cl->authed) { client_close(cl); return; }
        unsigned qos = (pkt[0] >> 1) & 0x03;
        size_t p = 0;
        if (rem < 2) return;
        unsigned tl = (unsigned)(body[p] << 8 | body[p + 1]); p += 2;
        if (p + tl > rem) return;
        char topic[128];
        size_t k = tl < sizeof(topic) - 1 ? tl : sizeof(topic) - 1;
        memcpy(topic, body + p, k); topic[k] = '\0';
        p += tl;
        unsigned short id = 0;
        if (qos) {
            if (p + 2 > rem) return;
            id = (unsigned short)(body[p] << 8 | body[p + 1]); p += 2;
        }
        size_t pl = rem - p;
        char *payload = malloc(pl + 1);
        if (!payload) return;
        memcpy(payload, body + p, pl);
        payload[pl] = '\0';

        char ip[64];
        peer_ip(cl->fd, ip, sizeof(ip));
        int stored = store_state(ip, payload);
        if (stored < 0) syslog(LOG_WARNING, "could not store telemetry state");
        else if (stored == 1) syslog(LOG_INFO, "telemetry has no usable location");

        /* Forward the exact topic/payload so rnode-halow.ru sees the same
         * packet it would have received directly. */
        pid_t pid = fork();
        if (pid == 0) {
            int rc = forward_packet(c, topic, payload);
            if (rc != 0) syslog(LOG_WARNING, "upstream forward failed");
            _exit(rc == 0 ? 0 : 1);
        }
        free(payload);
        if (qos == 1) send_puback(cl, id);
        break;
    }
    case 8: {   /* SUBSCRIBE: not used by the modem, but grant QoS 0 */
        if (!cl->authed || rem < 2) return;
        unsigned short id = (unsigned short)(body[0] << 8 | body[1]);
        unsigned char ack[5] = { 0x90, 0x03, (unsigned char)(id >> 8),
                                 (unsigned char)(id & 0xff), 0x00 };
        send(cl->fd, ack, sizeof(ack), 0);
        break;
    }
    case 12: {  /* PINGREQ */
        unsigned char pong[2] = { 0xd0, 0x00 };
        send(cl->fd, pong, 2, 0);
        break;
    }
    case 14:    /* DISCONNECT */
        client_close(cl);
        break;
    default:
        break;
    }
}

static void client_data(struct client *cl, const struct conf *c) {
    if (cl->len == cl->cap) {
        size_t ncap = cl->cap ? cl->cap * 2 : 2048;
        if (ncap > MAX_PACKET) ncap = MAX_PACKET;
        if (ncap == cl->cap) { client_close(cl); return; }
        unsigned char *nb = realloc(cl->buf, ncap);
        if (!nb) { client_close(cl); return; }
        cl->buf = nb; cl->cap = ncap;
    }
    ssize_t r = recv(cl->fd, cl->buf + cl->len, cl->cap - cl->len, 0);
    if (r <= 0) { client_close(cl); return; }
    cl->len += (size_t)r;

    /* Consume every complete packet in the buffer. */
    for (;;) {
        if (cl->len < 2) return;
        size_t at = 1, rem = 0, mult = 1;
        unsigned char b;
        do {
            if (at >= cl->len) return;
            b = cl->buf[at++];
            rem += (b & 0x7f) * mult;
            mult *= 128;
            if (mult > 128 * 128 * 128) { client_close(cl); return; }
        } while (b & 0x80);
        if (cl->len < at + rem) return;
        handle_packet(cl, cl->buf, at + rem, c);
        if (cl->fd < 0) return;   /* packet closed the connection */
        memmove(cl->buf, cl->buf + at + rem, cl->len - (at + rem));
        cl->len -= at + rem;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* forwarding children */
    openlog("rnsbox-telemetry", LOG_PID, LOG_DAEMON);

    struct conf c;
    read_conf(&c);
    if (!c.enabled) {
        syslog(LOG_INFO, "halow_mqtt is off; exiting");
        return 0;
    }
    syslog(LOG_INFO, "broker on :%d, forwarding to %s:%s",
           BROKER_PORT, c.upstream, c.upstream_port);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(BROKER_PORT);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        syslog(LOG_ERR, "bind :%d: %s", BROKER_PORT, strerror(errno));
        close(lfd);
        return 1;
    }
    if (listen(lfd, 4) != 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(lfd);
        return 1;
    }

    struct client cls[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) { memset(&cls[i], 0, sizeof(cls[i])); cls[i].fd = -1; }

    for (;;) {
        struct pollfd pf[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS];
        pf[0].fd = lfd;
        pf[0].events = POLLIN;
        int nf = 1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cls[i].fd < 0) continue;
            pf[nf].fd = cls[i].fd;
            pf[nf].events = POLLIN;
            map[nf] = i;
            nf++;
        }
        int rc = poll(pf, (nfds_t)nf, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "poll: %s", strerror(errno));
            break;
        }
        if (pf[0].revents & POLLIN) {
            int fd = accept(lfd, NULL, NULL);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (cls[i].fd < 0) { slot = i; break; }
                if (slot < 0) {
                    close(fd);
                } else {
                    cls[slot].fd = fd;
                    cls[slot].authed = 0;
                    cls[slot].len = cls[slot].cap = 0;
                    cls[slot].buf = NULL;
                }
            }
        }
        for (int i = 1; i < nf; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int idx = map[i];
            if (cls[idx].fd < 0) continue;
            client_data(&cls[idx], &c);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) client_close(&cls[i]);
    close(lfd);
    return 0;
}
