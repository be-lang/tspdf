#define _GNU_SOURCE
#include "commands.h"
#include "assets.h"
#include "../include/tspdf.h"
#include "../include/tspdf_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "../src/qr/qr_encode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_REQUEST    (50 * 1024 * 1024)  /* 50 MB for multi-file uploads */
#define MAX_HEADERS    64
#define MAX_FORM_PARTS 64
#define REQUEST_READ_TIMEOUT_SEC 5
/* Bound how long a single response write may block. A slow/absent reader that
 * never drains its receive window would otherwise wedge write() forever on this
 * single-threaded server (Slowloris on the response side), so SO_SNDTIMEO makes
 * a stalled send fail with EAGAIN/EWOULDBLOCK instead of hanging every client. */
#define RESPONSE_WRITE_TIMEOUT_SEC 30

/* ── HTTP Request ──────────────────────────────────────────────────── */

typedef struct {
    char method[8];
    char path[2048];
    char headers[MAX_HEADERS][2][512];
    int  header_count;
    char *body;
    size_t body_len;
    char content_type[512];
    size_t content_length;
    int    content_length_set;
} HttpRequest;

/* Parse Content-Length: digits only, optional OWS; rejects negative, overflow, garbage. */
static int parse_content_length_value(const char *val, size_t *out)
{
    if (!val || !out) return -1;
    const char *s = val;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return -1;
    if (*s == '-' || *s == '+') return -1;
    if (*s < '0' || *s > '9') return -1;
    unsigned long long v = 0ULL;
    for (; *s >= '0' && *s <= '9'; s++) {
        int digit = *s - '0';
        if (v > (ULLONG_MAX - (unsigned long long)digit) / 10ULL)
            return -1;
        v = v * 10ULL + (unsigned long long)digit;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') return -1;
#if ULLONG_MAX > SIZE_MAX
    if (v > (unsigned long long)SIZE_MAX) return -1;
#endif
    *out = (size_t)v;
    return 0;
}

/* Count header field lines after the request line. hdr_scan_end is exclusive: it must be
 * the first byte of the blank line (memmem(..., "\r\n\r\n") + 2), so [buf, hdr_scan_end)
 * includes the CRLF that terminates the last header but not the blank line's CRLF.
 * parse_request stores at most MAX_HEADERS colon-headers; rejecting here keeps scans aligned. */
static int count_header_field_lines(const char *buf, const char *hdr_scan_end, int *out_n)
{
    const char *req_eol = NULL;
    for (const char *q = buf; q + 1 < hdr_scan_end; q++) {
        if (q[0] == '\r' && q[1] == '\n') {
            req_eol = q;
            break;
        }
    }
    if (!req_eol)
        return -1;
    const char *p = req_eol + 2;
    int n = 0;
    while (p < hdr_scan_end) {
        const char *eol = NULL;
        for (const char *q = p; q + 1 < hdr_scan_end; q++) {
            if (q[0] == '\r' && q[1] == '\n') {
                eol = q;
                break;
            }
        }
        if (!eol)
            return -1;
        if (eol > p)
            n++;
        p = eol + 2;
    }
    *out_n = n;
    return 0;
}

/* Scan raw header field bytes [buf, hdr_scan_end) for Content-Length; last header wins.
 * hdr_scan_end is exclusive, same as count_header_field_lines (memmem + 2). */
static int scan_content_length_raw(const char *buf, const char *hdr_scan_end,
                                   int *have_cl, size_t *cl_out)
{
    *have_cl = 0;
    const char *p = buf;
    while (p < hdr_scan_end) {
        const char *eol = NULL;
        for (const char *q = p; q + 1 < hdr_scan_end; q++) {
            if (q[0] == '\r' && q[1] == '\n') { eol = q; break; }
        }
        if (!eol) break;
        if (eol > p && (size_t)(eol - p) >= 15 &&
            !strncasecmp(p, "Content-Length:", 15)) {
            const char *v = p + 15;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= 512) vlen = 511;
            char tmp[512];
            memcpy(tmp, v, vlen);
            tmp[vlen] = '\0';
            size_t cl;
            if (parse_content_length_value(tmp, &cl) != 0)
                return -1;
            *have_cl = 1;
            *cl_out = cl;
        }
        p = eol + 2;
    }
    return 0;
}

/* First \r\n in [start, end); does not read past end (raw socket buffer is not NUL-terminated). */
static const char *find_crlf_bounded(const char *start, const char *end)
{
    for (const char *q = start; q + 1 < end; q++) {
        if (q[0] == '\r' && q[1] == '\n')
            return q;
    }
    return NULL;
}

static const char *find_content_type_param_value(const char *content_type, const char *name)
{
    if (!content_type || !name) return NULL;

    size_t name_len = strlen(name);
    const char *p = content_type;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';')
            p++;
        if (!strncasecmp(p, name, name_len) && p[name_len] == '=') {
            return p + name_len + 1;
        }
        const char *next = strchr(p, ';');
        if (!next)
            break;
        p = next + 1;
    }
    return NULL;
}

static int parse_request(const char *raw, size_t raw_len, HttpRequest *req)
{
    memset(req, 0, sizeof(*req));
    const char *raw_end = raw + raw_len;
    const char *line_end = find_crlf_bounded(raw, raw_end);
    if (!line_end) return -1;
    size_t line1_len = (size_t)(line_end - raw);
    char line1_buf[8192];
    if (line1_len >= sizeof(line1_buf)) return -1;
    memcpy(line1_buf, raw, line1_len);
    line1_buf[line1_len] = '\0';
    if (sscanf(line1_buf, "%7s %2047s", req->method, req->path) != 2) return -1;

    const char *p = line_end + 2;
    while (p < raw_end) {
        const char *eol = find_crlf_bounded(p, raw_end);
        if (!eol) break;
        if (eol == p) { p = eol + 2; break; }
        if (req->header_count >= MAX_HEADERS) { p = eol + 2; continue; }
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            if (nlen >= 512) nlen = 511;
            memcpy(req->headers[req->header_count][0], p, nlen);
            req->headers[req->header_count][0][nlen] = '\0';
            const char *val = colon + 1;
            while (val < eol && *val == ' ') val++;
            size_t vlen = (size_t)(eol - val);
            if (vlen >= 512) vlen = 511;
            memcpy(req->headers[req->header_count][1], val, vlen);
            req->headers[req->header_count][1][vlen] = '\0';
            req->header_count++;
        }
        p = eol + 2;
    }

    for (int i = 0; i < req->header_count; i++) {
        if (!strcasecmp(req->headers[i][0], "Content-Type"))
            strncpy(req->content_type, req->headers[i][1], sizeof(req->content_type) - 1);
        else if (!strcasecmp(req->headers[i][0], "Content-Length")) {
            if (parse_content_length_value(req->headers[i][1], &req->content_length) != 0)
                return -1;
            req->content_length_set = 1;
        }
    }

    size_t header_len = (size_t)(p - raw);
    if (header_len < raw_len) {
        req->body = (char *)raw + header_len;
        req->body_len = raw_len - header_len;
    }
    if (req->content_length_set && req->body_len != req->content_length)
        return -1;
    return 0;
}

/* ── Local-origin enforcement (DNS-rebinding / cross-site mitigation) ──
 *
 * The server binds 127.0.0.1 by default, and binding the loopback interface
 * alone does not stop a remote web page from pointing an attacker-controlled
 * hostname at 127.0.0.1 (DNS rebinding) and then POSTing to our /api
 * endpoints — the browser would happily send the request and hand the
 * generated PDF back to the attacker's page. The standard, dependency-free
 * defence for a local tool is to (1) require the Host header to name loopback
 * (so a rebound attacker hostname is rejected) and (2) when an Origin/Referer
 * is present, require it to point at our own loopback origin too. Both checks
 * use already-parsed req->headers, so this is purely additive.
 *
 * --bind relaxes this by tiers. A specific non-loopback address (e.g. a LAN
 * IP) additionally accepts that address in Host/Origin; a rebound hostname
 * still fails the Host check on that tier. Binding 0.0.0.0 (the Docker
 * case) means the server is reachable under any name we cannot enumerate,
 * so the Host check is reduced to presence and the Origin check becomes
 * same-origin: a present Origin/Referer must match the request's own Host
 * (or loopback). Be precise about what that buys: it blocks foreign-Origin
 * CSRF (a page on another site sends its own origin, which cannot match
 * ours), but it does NOT mitigate DNS rebinding on 0.0.0.0 — a rebound
 * hostname resolves to this server, so the attacker's page sends that
 * hostname in both Host and Origin and they match each other. A
 * non-loopback bind fundamentally trusts the network, per the documented
 * threat model: there is no authentication, and cmd_serve prints a warning
 * saying so. */

/* Bind configuration (set once in cmd_serve before start_server). */
static struct {
    struct in_addr addr;
    int loopback;                       /* 127.0.0.0/8 (the default) */
    int any;                            /* INADDR_ANY */
    char addr_str[INET_ADDRSTRLEN];
} g_bind = { { 0 }, 1, 0, "127.0.0.1" };

static const char *find_request_header(const HttpRequest *req, const char *name)
{
    if (!req || !name) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (!strcasecmp(req->headers[i][0], name))
            return req->headers[i][1];
    }
    return NULL;
}

/* Extract the bare host from a Host-style value (no scheme, may carry
 * ":port") into out. The port, if present, must equal expect_port — a
 * browser always sends the port it connected to, so a mismatch means the
 * value was forged or rebound. Returns 1 on success. */
static int host_authority(const char *host, int expect_port, char *out, size_t out_sz)
{
    if (!host) return 0;
    while (*host == ' ' || *host == '\t') host++;

    /* Split off an optional port. IPv6 literals are bracketed ("[::1]:8080"),
     * so only treat the final ':' as a port separator when it is unambiguous. */
    char hostbuf[256];
    size_t hlen = strlen(host);
    if (hlen >= sizeof(hostbuf)) return 0;
    memcpy(hostbuf, host, hlen);
    hostbuf[hlen] = '\0';
    /* Trim trailing OWS that a header value might carry. */
    while (hlen > 0 && (hostbuf[hlen - 1] == ' ' || hostbuf[hlen - 1] == '\t'))
        hostbuf[--hlen] = '\0';

    char *port_sep = NULL;
    if (hostbuf[0] == '[') {
        char *rb = strchr(hostbuf, ']');
        if (!rb) return 0;
        if (rb[1] == ':') port_sep = rb + 1;
        else if (rb[1] != '\0') return 0;
    } else {
        /* IPv4/hostname: a lone ':' separates the port. A bare IPv6 address
         * (multiple colons, no brackets) is not valid in a Host header, so any
         * ambiguous multi-colon value is rejected below by the literal match. */
        char *first = strchr(hostbuf, ':');
        char *last  = strrchr(hostbuf, ':');
        if (first && first == last) port_sep = first;
    }

    if (port_sep) {
        char *endp = NULL;
        long p = strtol(port_sep + 1, &endp, 10);
        if (endp == port_sep + 1 || (endp && *endp != '\0')) return 0;
        if (p != (long)expect_port) return 0;
        *port_sep = '\0';
    }

    const char *h = hostbuf;
    if (h[0] == '[') {
        /* Strip the brackets around an IPv6 literal for the compare. */
        char *rb = strchr(hostbuf, ']');
        if (rb) *rb = '\0';
        h = hostbuf + 1;
    }

    size_t out_len = strlen(h);
    if (out_len >= out_sz) return 0;
    memcpy(out, h, out_len + 1);
    return 1;
}

/* True if host (no scheme, may carry ":port") names a loopback address.
 * Accepts "localhost", "127.0.0.1", "127.x.y.z", "[::1]" and "::1". */
static int host_is_loopback(const char *host, int expect_port)
{
    char h[256];
    if (!host_authority(host, expect_port, h, sizeof(h))) return 0;

    if (!strcasecmp(h, "localhost")) return 1;
    if (!strcmp(h, "::1")) return 1;
    /* Any 127.0.0.0/8 address is loopback. Match the leading octet strictly. */
    if (!strncmp(h, "127.", 4)) return 1;
    return 0;
}

/* Host-header gate, per the bind tiers described above. */
static int host_is_allowed(const char *host, int expect_port)
{
    if (host_is_loopback(host, expect_port)) return 1;
    if (g_bind.loopback) return 0;
    if (g_bind.any) {
        /* Reachable under any name; require presence only. The Origin
         * same-origin check below blocks foreign-Origin CSRF, but not DNS
         * rebinding (see the threat-model note above). */
        return host != NULL && *host != '\0';
    }
    /* Specific non-loopback bind: also accept the bound address. */
    char h[256];
    return host_authority(host, expect_port, h, sizeof(h)) &&
           strcmp(h, g_bind.addr_str) == 0;
}

/* Validate an Origin/Referer value (e.g. "http://127.0.0.1:8080" or
 * "http://192.168.1.5:8080/tool") by extracting the authority and applying
 * the bind tiers; for a 0.0.0.0 bind the authority must equal the request's
 * own Host value (same-origin). */
static int origin_is_allowed(const char *origin, const char *request_host, int expect_port)
{
    if (!origin) return 0;
    while (*origin == ' ' || *origin == '\t') origin++;
    /* The literal "null" origin (e.g. sandboxed iframe) is not us. */
    if (!strcasecmp(origin, "null")) return 0;

    const char *scheme = strstr(origin, "://");
    if (!scheme) return 0;
    const char *authority = scheme + 3;

    /* Authority ends at the first '/', '?', '#', or end-of-string. */
    char authbuf[256];
    size_t i = 0;
    for (const char *p = authority;
         *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (i >= sizeof(authbuf) - 1) return 0;
        authbuf[i++] = *p;
    }
    authbuf[i] = '\0';

    if (host_is_loopback(authbuf, expect_port)) return 1;
    if (g_bind.loopback) return 0;
    if (!g_bind.any) {
        char h[256];
        return host_authority(authbuf, expect_port, h, sizeof(h)) &&
               strcmp(h, g_bind.addr_str) == 0;
    }
    /* 0.0.0.0: same-origin — the Origin authority must equal the Host the
     * request was addressed to (both carry the same host:port text). This
     * stops a foreign origin only; a DNS-rebound page presents its own
     * hostname in both headers, which match. See the threat-model note. */
    if (!request_host) return 0;
    while (*request_host == ' ' || *request_host == '\t') request_host++;
    size_t hl = strlen(request_host);
    while (hl > 0 && (request_host[hl - 1] == ' ' || request_host[hl - 1] == '\t')) hl--;
    return strlen(authbuf) == hl && strncasecmp(authbuf, request_host, hl) == 0;
}

/* Gate for /api requests: the Host must pass the bind tier, and if the
 * browser supplied an Origin (always present on cross-origin/CORS-relevant
 * fetches) or a Referer, that too must be an allowed origin. Returns 1 to
 * allow. */
static int request_is_local(const HttpRequest *req, int port)
{
    const char *host = find_request_header(req, "Host");
    if (!host_is_allowed(host, port))
        return 0;

    const char *origin = find_request_header(req, "Origin");
    if (origin && !origin_is_allowed(origin, host, port))
        return 0;

    /* Fall back to Referer only when Origin is absent (older browsers / some
     * fetch modes omit Origin). A present Referer must still match. */
    if (!origin) {
        const char *referer = find_request_header(req, "Referer");
        if (referer && !origin_is_allowed(referer, host, port))
            return 0;
    }
    return 1;
}

/* ── Response helpers ──────────────────────────────────────────────── */

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 408: return "Request Timeout";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

// Write all bytes, retrying on short writes. Returns false if the peer closed
// or errored. Best-effort: callers are writing a response to a socket that may
// have gone away, so a failure just ends the response early.
static bool write_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return false;
        p += n;
        remaining -= (size_t)n;
    }
    return true;
}

static void send_response(int fd, int status, const char *ctype,
                           const void *body, size_t body_len)
{
    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text(status), ctype, body_len);
    if (hlen <= 0 || !write_all(fd, hdr, (size_t)hlen)) return;
    if (body && body_len > 0) {
        write_all(fd, body, body_len);
    }
}

static void send_error(int fd, int status, const char *msg)
{
    send_response(fd, status, "text/plain", msg, strlen(msg));
}

static void send_pdf_response(int fd, const char *filename,
                              const void *body, size_t body_len)
{
    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/pdf\r\n"
        "Content-Length: %zu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len, filename);
    if (hlen <= 0 || !write_all(fd, hdr, (size_t)hlen)) return;
    if (body && body_len > 0) {
        write_all(fd, body, body_len);
    }
}

/* ── Template rendering ────────────────────────────────────────────── */

typedef struct {
    char name[64];
    const char *start;
    size_t len;
} Block;

static int extract_blocks(const char *text, Block *blocks, int max_blocks)
{
    int count = 0;
    const char *p = text;
    while (count < max_blocks) {
        const char *open = strstr(p, "{% block ");
        if (!open) break;
        const char *name_start = open + 9;
        const char *name_end = strstr(name_start, " %}");
        if (!name_end) break;
        size_t nlen = (size_t)(name_end - name_start);
        while (nlen > 0 && name_start[nlen - 1] == ' ') nlen--;
        if (nlen >= sizeof(blocks[0].name)) nlen = sizeof(blocks[0].name) - 1;
        memcpy(blocks[count].name, name_start, nlen);
        blocks[count].name[nlen] = '\0';

        const char *content_start = name_end + 3;
        const char *close = strstr(content_start, "{% endblock %}");
        if (!close) break;

        blocks[count].start = content_start;
        blocks[count].len = (size_t)(close - content_start);
        count++;
        p = close + 14;
    }
    return count;
}

/* Render a child template (embedded asset) into base.html.
 * Both are embedded byte arrays from assets.h. Returns malloc'd string. */
static char *render_template(const unsigned char *child_data, size_t child_size)
{
    /* Make nul-terminated copies */
    char *base = malloc(asset_base_html_len + 1);
    char *child = malloc(child_size + 1);
    if (!base || !child) { free(base); free(child); return NULL; }
    memcpy(base, asset_base_html, asset_base_html_len);
    base[asset_base_html_len] = '\0';
    memcpy(child, child_data, child_size);
    child[child_size] = '\0';

    Block child_blocks[16];
    int nblocks = extract_blocks(child, child_blocks, 16);

    size_t base_len = asset_base_html_len;
    size_t out_cap = base_len + child_size + 4096;
    char *out = malloc(out_cap);
    if (!out) { free(base); free(child); return NULL; }
    size_t out_len = 0;

    const char *p = base;
    while (*p) {
        const char *open = strstr(p, "{% block ");
        if (!open) {
            size_t rest = base_len - (size_t)(p - base);
            if (out_len + rest >= out_cap) {
                out_cap = (out_len + rest + 1) * 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, p, rest);
            out_len += rest;
            break;
        }
        size_t before = (size_t)(open - p);
        if (out_len + before >= out_cap) {
            out_cap = (out_len + before + 1) * 2;
            out = realloc(out, out_cap);
        }
        memcpy(out + out_len, p, before);
        out_len += before;

        const char *bname_start = open + 9;
        const char *bname_end = strstr(bname_start, " %}");
        if (!bname_end) {
            size_t rest = base_len - (size_t)(p - base);
            if (out_len + rest >= out_cap) {
                out_cap = (out_len + rest + 1) * 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, p, rest);
            out_len += rest;
            break;
        }
        char bname[64] = {0};
        size_t bnlen = (size_t)(bname_end - bname_start);
        if (bnlen >= sizeof(bname)) bnlen = sizeof(bname) - 1;
        memcpy(bname, bname_start, bnlen);

        const char *base_content_start = bname_end + 3;
        const char *base_close = strstr(base_content_start, "{% endblock %}");
        if (!base_close) break;

        int found = -1;
        for (int i = 0; i < nblocks; i++) {
            if (!strcmp(bname, child_blocks[i].name)) { found = i; break; }
        }

        if (found >= 0) {
            size_t need = out_len + child_blocks[found].len + base_len + 4096;
            if (need > out_cap) {
                out_cap = need * 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, child_blocks[found].start, child_blocks[found].len);
            out_len += child_blocks[found].len;
        } else {
            size_t def_len = (size_t)(base_close - base_content_start);
            if (out_len + def_len >= out_cap) {
                out_cap = (out_len + def_len + 1) * 2;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, base_content_start, def_len);
            out_len += def_len;
        }

        p = base_close + 14;
    }

    out[out_len] = '\0';
    free(base);
    free(child);
    return out;
}

/* ── Multipart form parsing ────────────────────────────────────────── */

typedef struct {
    char name[256];
    char filename[256];
    char *data;
    size_t data_len;
    int is_file;
} FormPart;

typedef struct {
    FormPart parts[MAX_FORM_PARTS];
    int part_count;
} MultipartForm;

static int extract_quoted(const char *haystack, const char *key, char *out, size_t out_sz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = strstr(haystack, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q) return -1;
    size_t len = (size_t)(q - p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int parse_multipart(const char *body, size_t body_len,
                            const char *boundary, MultipartForm *form)
{
    memset(form, 0, sizeof(*form));
    size_t blen = strlen(boundary);
    const char *p = body;
    const char *end = body + body_len;

    while (p < end && form->part_count < MAX_FORM_PARTS) {
        const char *bstart = NULL;
        for (const char *s = p; s + blen + 2 <= end; s++) {
            if (s[0] == '-' && s[1] == '-' && !memcmp(s + 2, boundary, blen)) {
                bstart = s;
                break;
            }
        }
        if (!bstart) break;

        const char *after_b = bstart + 2 + blen;
        if (after_b + 2 <= end && after_b[0] == '-' && after_b[1] == '-')
            break;

        const char *part_start = after_b;
        if (part_start < end && *part_start == '\r') part_start++;
        if (part_start < end && *part_start == '\n') part_start++;

        const char *hdr_end = NULL;
        for (const char *s = part_start; s + 4 <= end; s++) {
            if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
                hdr_end = s;
                break;
            }
        }
        if (!hdr_end) { p = after_b; continue; }

        size_t hdr_len = (size_t)(hdr_end - part_start);
        char hdr_buf[2048];
        if (hdr_len >= sizeof(hdr_buf)) hdr_len = sizeof(hdr_buf) - 1;
        memcpy(hdr_buf, part_start, hdr_len);
        hdr_buf[hdr_len] = '\0';

        FormPart *fp = &form->parts[form->part_count];
        memset(fp, 0, sizeof(*fp));
        extract_quoted(hdr_buf, "name", fp->name, sizeof(fp->name));
        if (extract_quoted(hdr_buf, "filename", fp->filename, sizeof(fp->filename)) == 0)
            fp->is_file = 1;

        const char *data_start = hdr_end + 4;

        const char *next_b = NULL;
        for (const char *s = data_start; s + blen + 2 <= end; s++) {
            if (s[0] == '-' && s[1] == '-' && !memcmp(s + 2, boundary, blen)) {
                next_b = s;
                break;
            }
        }
        if (!next_b) break;

        const char *data_end = next_b;
        if (data_end >= data_start + 2 &&
            data_end[-2] == '\r' && data_end[-1] == '\n')
            data_end -= 2;

        fp->data = (char *)data_start;
        fp->data_len = (size_t)(data_end - data_start);
        form->part_count++;

        p = next_b;
    }
    return 0;
}

static FormPart *form_find(MultipartForm *form, const char *name)
{
    for (int i = 0; i < form->part_count; i++)
        if (!strcmp(form->parts[i].name, name)) return &form->parts[i];
    return NULL;
}

/* ── Minimal JSON value extractor ──────────────────────────────────── */

/* Read 4 hex digits at p into *out (0..0xFFFF). Returns 0 on success. */
static int json_read_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Encode a Unicode code point as UTF-8 into buf (must have room for up to 4
 * bytes). Returns number of bytes written, or 0 if it would not fit within the
 * remaining capacity (avail = bytes still free including the space we need). */
static size_t json_utf8_encode(unsigned cp, char *dst, size_t avail)
{
    if (cp <= 0x7F) {
        if (avail < 1) return 0;
        dst[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        if (avail < 2) return 0;
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        if (avail < 3) return 0;
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        if (avail < 4) return 0;
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* Extract a string value for "key" from a flat JSON object.
 * Handles: {"key":"value", ...}  Correctly scans past escaped quotes and
 * decodes JSON string escapes (\" \\ \/ \n \r \t \b \f and \uXXXX, including
 * surrogate pairs). Output is UTF-8, clamped to buf_sz-1 bytes (never
 * overflows). Returns 0 on success. */
static int json_get_string(const char *json, const char *key, char *buf, size_t buf_sz)
{
    if (!json || !key || buf_sz == 0) return -1;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    /* skip whitespace and colon */
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;

    size_t out = 0;
    const size_t max = buf_sz - 1;  /* leave room for NUL */
    while (*p && *p != '"') {
        if (*p == '\\') {
            char e = p[1];
            if (e == '\0') break;  /* trailing backslash: stop */
            switch (e) {
                case '"':  if (out < max) buf[out++] = '"';  p += 2; break;
                case '\\': if (out < max) buf[out++] = '\\'; p += 2; break;
                case '/':  if (out < max) buf[out++] = '/';  p += 2; break;
                case 'n':  if (out < max) buf[out++] = '\n'; p += 2; break;
                case 'r':  if (out < max) buf[out++] = '\r'; p += 2; break;
                case 't':  if (out < max) buf[out++] = '\t'; p += 2; break;
                case 'b':  if (out < max) buf[out++] = '\b'; p += 2; break;
                case 'f':  if (out < max) buf[out++] = '\f'; p += 2; break;
                case 'u': {
                    unsigned cp;
                    if (json_read_hex4(p + 2, &cp) != 0) {
                        /* Malformed \u: emit literal 'u' and continue past it. */
                        if (out < max) buf[out++] = 'u';
                        p += 2;
                        break;
                    }
                    p += 6;  /* consumed \uXXXX */
                    /* High surrogate: try to combine with a following
                     * low surrogate \uXXXX into an astral code point. */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p[0] == '\\' && p[1] == 'u') {
                        unsigned lo;
                        if (json_read_hex4(p + 2, &lo) == 0 &&
                            lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u +
                                 (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                            p += 6;
                        }
                    }
                    /* Reject values that would corrupt the UTF-8 C string the
                     * callers consume: an unpaired surrogate (invalid UTF-8) or
                     * a NUL (truncates every downstream strlen). Substitute the
                     * Unicode replacement character U+FFFD instead. */
                    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                        cp = 0xFFFD;
                    }
                    out += json_utf8_encode(cp, buf + out, max - out);
                    break;
                }
                default:
                    /* Unknown escape: keep the character literally. */
                    if (out < max) buf[out++] = e;
                    p += 2;
                    break;
            }
        } else {
            if (out < max) buf[out++] = *p;
            p++;
        }
    }
    if (*p != '"') return -1;  /* unterminated string */
    buf[out] = '\0';
    return 0;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonBuffer;

static void json_buffer_destroy(JsonBuffer *buf)
{
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static bool json_buffer_reserve(JsonBuffer *buf, size_t extra)
{
    if (!buf) return false;
    if (extra > MAX_REQUEST || buf->len > MAX_REQUEST - extra)
        return false;
    size_t needed = buf->len + extra;
    if (needed <= buf->cap)
        return true;

    size_t new_cap = buf->cap ? buf->cap : 256u;
    while (new_cap < needed) {
        if (new_cap >= MAX_REQUEST) {
            break;
        }
        if (new_cap <= MAX_REQUEST / 2u) {
            new_cap *= 2u;
        } else {
            new_cap = MAX_REQUEST;
        }
    }
    if (new_cap < needed || new_cap > MAX_REQUEST)
        return false;

    char *new_data = (char *)realloc(buf->data, new_cap);
    if (!new_data)
        return false;
    buf->data = new_data;
    buf->cap = new_cap;
    return true;
}

static bool json_buffer_append_bytes(JsonBuffer *buf, const void *data, size_t len)
{
    if (!json_buffer_reserve(buf, len))
        return false;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return true;
}

static bool json_buffer_append_cstr(JsonBuffer *buf, const char *lit)
{
    return json_buffer_append_bytes(buf, lit, strlen(lit));
}

static bool json_buffer_append_byte(JsonBuffer *buf, char ch)
{
    return json_buffer_append_bytes(buf, &ch, 1u);
}

/* Length of the valid UTF-8 sequence starting at u (1..4), or 0 if u does not
 * begin a well-formed sequence — including overlong forms, out-of-range code
 * points, and truncated sequences (a continuation byte missing or out of the
 * 0x80..0xBF range). Metadata getters can return RAW bytes (BOM-less UTF-16BE
 * or PDFDocEncoded /Title//Author), so the input is not guaranteed UTF-8. */
static int json_utf8_seq_len(const unsigned char *u)
{
    unsigned char c = u[0];
    if (c < 0x80u) return 1;
    if (c < 0xC2u) return 0;                 /* stray cont / overlong 0xC0-0xC1 */
    if (c < 0xE0u) {                         /* 2-byte */
        if ((u[1] & 0xC0u) != 0x80u) return 0;
        return 2;
    }
    if (c < 0xF0u) {                         /* 3-byte */
        if ((u[1] & 0xC0u) != 0x80u || (u[2] & 0xC0u) != 0x80u) return 0;
        if (c == 0xE0u && u[1] < 0xA0u) return 0;                 /* overlong */
        if (c == 0xEDu && u[1] >= 0xA0u) return 0;                /* surrogate */
        return 3;
    }
    if (c < 0xF5u) {                         /* 4-byte */
        if ((u[1] & 0xC0u) != 0x80u || (u[2] & 0xC0u) != 0x80u ||
            (u[3] & 0xC0u) != 0x80u) return 0;
        if (c == 0xF0u && u[1] < 0x90u) return 0;                 /* overlong */
        if (c == 0xF4u && u[1] >= 0x90u) return 0;                /* > U+10FFFF */
        return 4;
    }
    return 0;                                /* 0xF5..0xFF */
}

/* JSON string emission (same escaping rules as the CLI's writer in
 * cmd_info.c — keep them in sync): control bytes as \u00XX, quote and
 * backslash escaped, valid UTF-8 passed through verbatim, and any byte that
 * does not form well-formed UTF-8 replaced with U+FFFD so the output is always
 * a decodable UTF-8 JSON string. */
static bool json_buffer_append_str_json(JsonBuffer *buf, const char *s)
{
    if (!s) s = "";
    if (!json_buffer_append_byte(buf, '"'))
        return false;
    for (const unsigned char *u = (const unsigned char *)s; *u; ) {
        if (*u < 0x20u) {
            char esc[7];
            int nw = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*u);
            if (nw != 6 || !json_buffer_append_bytes(buf, esc, (size_t)nw))
                return false;
            u++;
        } else if (*u == '"' || *u == '\\') {
            char esc[2] = {'\\', (char)*u};
            if (!json_buffer_append_bytes(buf, esc, sizeof(esc)))
                return false;
            u++;
        } else {
            int n = json_utf8_seq_len(u);
            if (n == 0) {
                if (!json_buffer_append_bytes(buf, "\xEF\xBF\xBD", 3u))
                    return false;            /* U+FFFD REPLACEMENT CHARACTER */
                u++;
            } else {
                if (!json_buffer_append_bytes(buf, u, (size_t)n))
                    return false;
                u += n;
            }
        }
    }
    return json_buffer_append_byte(buf, '"');
}

/* ── GET route handling ────────────────────────────────────────────── */

typedef struct {
    const char *route;              /* e.g. "/tool/merge" */
    const unsigned char *asset;
    size_t asset_len;
} ToolRoute;

static const ToolRoute tool_routes[] = {
    { "/tool/merge",              asset_tool_merge,              0 },
    { "/tool/split",              asset_tool_split,              0 },
    { "/tool/delete-pages",       asset_tool_delete_pages,       0 },
    { "/tool/rotate",             asset_tool_rotate,             0 },
    { "/tool/reorder",            asset_tool_reorder,            0 },
    { "/tool/compress",           asset_tool_compress,           0 },
    { "/tool/unlock",             asset_tool_unlock,             0 },
    { "/tool/password-protect",   asset_tool_password_protect,   0 },
    { "/tool/metadata",           asset_tool_metadata,           0 },
    { "/tool/watermark-existing", asset_tool_watermark_existing, 0 },
    { "/tool/img2pdf",            asset_tool_img2pdf,            0 },
    { "/tool/md2pdf",             asset_tool_md2pdf,             0 },
    { "/tool/qrcode",             asset_tool_qrcode,             0 },
};

/* We need the _len values; since they are 'static const size_t' in assets.h we
 * cannot use them in the aggregate initialiser above.  Build a parallel array
 * at startup instead. */
static size_t tool_route_lens[sizeof(tool_routes)/sizeof(tool_routes[0])];
#define N_TOOL_ROUTES ((int)(sizeof(tool_routes)/sizeof(tool_routes[0])))

static void init_tool_route_lens(void)
{
    tool_route_lens[0]  = asset_tool_merge_len;
    tool_route_lens[1]  = asset_tool_split_len;
    tool_route_lens[2]  = asset_tool_delete_pages_len;
    tool_route_lens[3]  = asset_tool_rotate_len;
    tool_route_lens[4]  = asset_tool_reorder_len;
    tool_route_lens[5]  = asset_tool_compress_len;
    tool_route_lens[6]  = asset_tool_unlock_len;
    tool_route_lens[7]  = asset_tool_password_protect_len;
    tool_route_lens[8]  = asset_tool_metadata_len;
    tool_route_lens[9]  = asset_tool_watermark_existing_len;
    tool_route_lens[10] = asset_tool_img2pdf_len;
    tool_route_lens[11] = asset_tool_md2pdf_len;
    tool_route_lens[12] = asset_tool_qrcode_len;
}

static void handle_get(int fd, const char *path)
{
    /* Static assets */
    if (strcmp(path, "/static/style.css") == 0) {
        send_response(fd, 200, "text/css",
                      asset_style_css, asset_style_css_len);
        return;
    }
    if (strcmp(path, "/static/app.js") == 0) {
        send_response(fd, 200, "application/javascript",
                      asset_app_js, asset_app_js_len);
        return;
    }

    /* Index */
    if (strcmp(path, "/") == 0) {
        char *html = render_template(asset_index_html, asset_index_html_len);
        if (html) {
            send_response(fd, 200, "text/html; charset=utf-8", html, strlen(html));
            free(html);
        } else {
            send_error(fd, 500, "Template render failed");
        }
        return;
    }

    /* Tool pages */
    for (int i = 0; i < N_TOOL_ROUTES; i++) {
        if (strcmp(path, tool_routes[i].route) == 0) {
            char *html = render_template(tool_routes[i].asset, tool_route_lens[i]);
            if (html) {
                send_response(fd, 200, "text/html; charset=utf-8", html, strlen(html));
                free(html);
            } else {
                send_error(fd, 500, "Template render failed");
            }
            return;
        }
    }

    send_error(fd, 404, "Not found");
}

/* ── POST API handlers ─────────────────────────────────────────────── */

static void api_merge(int fd, MultipartForm *form)
{
    /* Collect all pdf_file parts (pdf_file, pdf_file_1, pdf_file_2, ...) */
    TspdfReader *docs[MAX_FORM_PARTS];
    int ndocs = 0;

    for (int i = 0; i < form->part_count; i++) {
        if (!form->parts[i].is_file) continue;
        if (strncmp(form->parts[i].name, "pdf_file", 8) != 0) continue;

        TspdfError err = TSPDF_OK;
        TspdfReader *doc = tspdf_reader_open(
            (const uint8_t *)form->parts[i].data, form->parts[i].data_len, &err);
        if (!doc) {
            for (int j = 0; j < ndocs; j++) tspdf_reader_destroy(docs[j]);
            send_error(fd, 400, "Failed to open uploaded PDF");
            return;
        }
        docs[ndocs++] = doc;
    }

    if (ndocs < 2) {
        for (int j = 0; j < ndocs; j++) tspdf_reader_destroy(docs[j]);
        send_error(fd, 400, "Need at least 2 PDF files to merge");
        return;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *merged = tspdf_reader_merge(docs, (size_t)ndocs, &err);
    for (int j = 0; j < ndocs; j++) tspdf_reader_destroy(docs[j]);

    if (!merged) {
        send_error(fd, 500, "Merge failed");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    tspdf_reader_destroy(merged);

    if (err != TSPDF_OK) {
        free(out);
        send_error(fd, 500, "Failed to save merged PDF");
        return;
    }

    send_pdf_response(fd, "merged.pdf", out, out_len);
    free(out);
}

static void api_split(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char pages_str[256] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "pages", pages_str, sizeof(pages_str));
    }

    if (!pages_str[0]) { send_error(fd, 400, "Missing pages in config"); return; }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    size_t count = 0;
    size_t *indices = parse_page_range(pages_str, &count);
    if (!indices || count == 0) {
        tspdf_reader_destroy(doc);
        send_error(fd, 400, "Invalid page range");
        return;
    }

    TspdfReader *result = tspdf_reader_extract(doc, indices, count, &err);
    free(indices);
    if (!result) {
        tspdf_reader_destroy(doc);
        send_error(fd, 500, "Extract failed");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "split.pdf", out, out_len);
    free(out);
}

static void api_delete_pages(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char pages_str[256] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "pages", pages_str, sizeof(pages_str));
    }
    if (!pages_str[0]) { send_error(fd, 400, "Missing pages in config"); return; }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    size_t count = 0;
    size_t *indices = parse_page_range(pages_str, &count);
    if (!indices || count == 0) {
        tspdf_reader_destroy(doc);
        send_error(fd, 400, "Invalid page range");
        return;
    }

    TspdfReader *result = tspdf_reader_delete(doc, indices, count, &err);
    free(indices);
    if (!result) {
        tspdf_reader_destroy(doc);
        send_error(fd, 500, "Delete pages failed");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "deleted.pdf", out, out_len);
    free(out);
}

static void api_rotate(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char pages_str[256] = {0};
    char angle_str[32] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "pages", pages_str, sizeof(pages_str));
        json_get_string(config_buf, "angle", angle_str, sizeof(angle_str));
    }

    int angle = angle_str[0] ? atoi(angle_str) : 90;

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    size_t count = 0;
    size_t *indices = NULL;

    int all_pages = !pages_str[0] || strcasecmp(pages_str, "all") == 0;
    if (!all_pages) {
        indices = parse_page_range(pages_str, &count);
        if (!indices) {
            tspdf_reader_destroy(doc);
            send_error(fd, 400, "Invalid page range");
            return;
        }
    } else {
        count = tspdf_reader_page_count(doc);
        indices = malloc(count * sizeof(size_t));
        if (!indices) {
            tspdf_reader_destroy(doc);
            send_error(fd, 500, "Out of memory");
            return;
        }
        for (size_t i = 0; i < count; i++) indices[i] = i;
    }

    TspdfReader *result = tspdf_reader_rotate(doc, indices, count, angle, &err);
    free(indices);
    if (!result) {
        tspdf_reader_destroy(doc);
        send_error(fd, 500, "Rotate failed");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "rotated.pdf", out, out_len);
    free(out);
}

static void api_reorder(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char order_str[1024] = {0};
    if (cfg) {
        char config_buf[2048];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "order", order_str, sizeof(order_str));
    }
    if (!order_str[0]) { send_error(fd, 400, "Missing order in config"); return; }

    /* Parse comma-separated 1-based page numbers */
    size_t order[512];
    size_t count = 0;
    char *tok = strtok(order_str, ",");
    while (tok && count < 512) {
        int pg = atoi(tok);
        if (pg < 1) { send_error(fd, 400, "Invalid page number in order"); return; }
        order[count++] = (size_t)(pg - 1);
        tok = strtok(NULL, ",");
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    TspdfReader *result = tspdf_reader_reorder(doc, order, count, &err);
    if (!result) {
        tspdf_reader_destroy(doc);
        send_error(fd, 500, "Reorder failed");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "reordered.pdf", out, out_len);
    free(out);
}

static void api_compress(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Compress failed"); return; }
    send_pdf_response(fd, "compressed.pdf", out, out_len);
    free(out);
}

static void api_unlock(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char password[256] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "password", password, sizeof(password));
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_with_password(
        (const uint8_t *)file->data, file->data_len, password, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF (wrong password?)"); return; }

    // Saves preserve encryption by default; unlocking is the explicit opt-out.
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "unlocked.pdf", out, out_len);
    free(out);
}

static void api_password_protect(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char password[256] = {0};
    char owner_password[256] = {0};
    char bits_str[32] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        json_get_string(config_buf, "password", password, sizeof(password));
        json_get_string(config_buf, "owner_password", owner_password, sizeof(owner_password));
        json_get_string(config_buf, "bits", bits_str, sizeof(bits_str));
    }

    if (!password[0]) { send_error(fd, 400, "Missing password in config"); return; }
    int bits = bits_str[0] ? atoi(bits_str) : 128;
    if (bits != 128 && bits != 256) bits = 128;

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                 password,
                                                 owner_password[0] ? owner_password : password,
                                                 0xFFFFFFFF, bits);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Encryption failed"); return; }
    send_pdf_response(fd, "protected.pdf", out, out_len);
    free(out);
}

static void api_metadata_view(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    const char *title = tspdf_reader_get_title(doc);
    const char *author = tspdf_reader_get_author(doc);
    const char *subject = tspdf_reader_get_subject(doc);
    const char *keywords = tspdf_reader_get_keywords(doc);
    const char *creator = tspdf_reader_get_creator(doc);
    const char *producer = tspdf_reader_get_producer(doc);
    const char *created = tspdf_reader_get_creation_date(doc);
    const char *modified = tspdf_reader_get_mod_date(doc);
    size_t pages = tspdf_reader_page_count(doc);

    if (!title) title = "";
    if (!author) author = "";
    if (!subject) subject = "";
    if (!keywords) keywords = "";
    if (!creator) creator = "";
    if (!producer) producer = "";
    if (!created) created = "";
    if (!modified) modified = "";

    JsonBuffer json = {0};
    char pbuf[32];
    int plen;

    if (!json_buffer_append_cstr(&json, "{\"title\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, title)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"author\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, author)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"subject\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, subject)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"keywords\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, keywords)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"creator\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, creator)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"producer\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, producer)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"created\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, created)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"modified\":")) goto meta_json_err;
    if (!json_buffer_append_str_json(&json, modified)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, ",\"pages\":")) goto meta_json_err;
    plen = snprintf(pbuf, sizeof(pbuf), "%zu", pages);
    if (plen <= 0 || (size_t)plen >= sizeof(pbuf)) goto meta_json_err;
    if (!json_buffer_append_bytes(&json, pbuf, (size_t)plen)) goto meta_json_err;
    if (!json_buffer_append_cstr(&json, "}")) goto meta_json_err;

    tspdf_reader_destroy(doc);
    send_response(fd, 200, "application/json", json.data, json.len);
    json_buffer_destroy(&json);
    return;

meta_json_err:
    tspdf_reader_destroy(doc);
    json_buffer_destroy(&json);
    send_error(fd, 500, "Metadata JSON overflow");
}

static void api_metadata(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    if (cfg) {
        char config_buf[4096];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';

        char val[512];
        if (json_get_string(config_buf, "title", val, sizeof(val)) == 0)
            tspdf_reader_set_title(doc, val);
        if (json_get_string(config_buf, "author", val, sizeof(val)) == 0)
            tspdf_reader_set_author(doc, val);
        if (json_get_string(config_buf, "subject", val, sizeof(val)) == 0)
            tspdf_reader_set_subject(doc, val);
        if (json_get_string(config_buf, "keywords", val, sizeof(val)) == 0)
            tspdf_reader_set_keywords(doc, val);
        if (json_get_string(config_buf, "creator", val, sizeof(val)) == 0)
            tspdf_reader_set_creator(doc, val);
        if (json_get_string(config_buf, "producer", val, sizeof(val)) == 0)
            tspdf_reader_set_producer(doc, val);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "metadata.pdf", out, out_len);
    free(out);
}

static void api_watermark(int fd, MultipartForm *form)
{
    FormPart *file = form_find(form, "pdf_file");
    FormPart *cfg = form_find(form, "config");
    if (!file || !file->is_file) { send_error(fd, 400, "Missing pdf_file"); return; }

    char text[256] = "DRAFT";
    char opacity_str[32] = {0};
    char font_size_str[32] = {0};
    if (cfg) {
        char config_buf[1024];
        size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
        memcpy(config_buf, cfg->data, clen);
        config_buf[clen] = '\0';
        /* The watermark tool page sends "watermark_text"; keep "text" as a
         * fallback for direct API callers. */
        json_get_string(config_buf, "watermark_text", text, sizeof(text));
        if (!text[0] || strcmp(text, "DRAFT") == 0)
            json_get_string(config_buf, "text", text, sizeof(text));
        if (!text[0]) snprintf(text, sizeof(text), "DRAFT");
        json_get_string(config_buf, "opacity", opacity_str, sizeof(opacity_str));
        json_get_string(config_buf, "font_size", font_size_str, sizeof(font_size_str));
    }

    double opacity = opacity_str[0] ? atof(opacity_str) : 0.3;
    if (opacity <= 0.0 || opacity > 1.0) opacity = 0.3;
    double cfg_font_size = font_size_str[0] ? atof(font_size_str) : 48.0;
    if (cfg_font_size < 6.0 || cfg_font_size > 144.0) cfg_font_size = 48.0;

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)file->data, file->data_len, &err);
    if (!doc) { send_error(fd, 400, "Failed to open PDF"); return; }

    size_t page_count = tspdf_reader_page_count(doc);
    double font_size = cfg_font_size;

    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        /* Center of the VISIBLE page: the MediaBox origin is not always
         * (0,0), so the center is ((x0+x1)/2, (y0+y1)/2), not (w/2, h/2).
         * A page-level /Rotate is compensated by pre-rotating the stamp the
         * other way (45 + rotate CCW) so the diagonal reads upright as
         * viewed. Same math as cmd_watermark.c. */
        double cx = (page->media_box[0] + page->media_box[2]) / 2.0;
        double cy = (page->media_box[1] + page->media_box[3]) / 2.0;
        int rot = ((page->rotate % 360) + 360) % 360;
        double angle_rad = (45.0 + (double)rot) * M_PI / 180.0;
        double cos_a = cos(angle_rad);
        double sin_a = sin(angle_rad);

        TspdfWriter *writer = tspdf_writer_create();
        if (!writer) continue;

        const char *font_name = tspdf_writer_add_builtin_font(writer, "Helvetica");
        if (!font_name) { tspdf_writer_destroy(writer); continue; }

        const char *gs_name = tspdf_writer_add_opacity(writer, opacity, opacity);

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) { tspdf_writer_destroy(writer); continue; }

        tspdf_stream_save(stream);
        if (gs_name) tspdf_stream_set_opacity(stream, gs_name);

        TspdfColor gray = tspdf_color_rgb(0.7, 0.7, 0.7);
        tspdf_stream_set_fill_color(stream, gray);
        tspdf_stream_concat_matrix(stream, cos_a, sin_a, -sin_a, cos_a, cx, cy);

        double text_width = (double)strlen(text) * font_size * 0.5;
        tspdf_stream_begin_text(stream);
        tspdf_stream_set_font(stream, font_name, font_size);
        tspdf_stream_text_position(stream, -text_width / 2.0, -font_size / 3.0);
        tspdf_stream_show_text(stream, text);
        tspdf_stream_end_text(stream);
        tspdf_stream_restore(stream);

        err = tspdf_page_end_content(doc, i, stream, writer);
        tspdf_writer_destroy(writer);
        if (err != TSPDF_OK) {
            tspdf_reader_destroy(doc);
            send_error(fd, 500, "Watermark overlay failed");
            return;
        }
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    tspdf_reader_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "watermarked.pdf", out, out_len);
    free(out);
}

/* ── img2pdf API handler ───────────────────────────────────────────── */

static void api_img2pdf(int fd, MultipartForm *form)
{
    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { send_error(fd, 500, "Out of memory"); return; }

    tspdf_writer_set_title(doc, "Image Collection");
    int pages_added = 0;

    /* Stage uploaded image bytes in a per-request private temp directory rather
     * than a predictable path. mkdtemp() creates the dir 0700 and guarantees a
     * fresh name, so an attacker cannot pre-plant a symlink to redirect our
     * writes; O_CREAT|O_EXCL|O_NOFOLLOW inside it defends in depth. */
    const char *tmpbase = getenv("TMPDIR");
    if (!tmpbase || !tmpbase[0]) tmpbase = "/tmp";
    char tmpdir[PATH_MAX];
    if (snprintf(tmpdir, sizeof(tmpdir), "%s/tspdf_img_XXXXXX", tmpbase)
            >= (int)sizeof(tmpdir) ||
        !mkdtemp(tmpdir)) {
        tspdf_writer_destroy(doc);
        send_error(fd, 500, "Failed to create temp directory");
        return;
    }

    for (int i = 0; i < form->part_count; i++) {
        FormPart *p = &form->parts[i];
        if (!p->is_file || strcmp(p->name, "images") != 0) continue;

        const char *img_name = NULL;
        size_t nlen = strlen(p->filename);
        int png = (nlen > 4 &&
            (p->filename[nlen-4] == '.') &&
            (p->filename[nlen-3] == 'p' || p->filename[nlen-3] == 'P') &&
            (p->filename[nlen-2] == 'n' || p->filename[nlen-2] == 'N') &&
            (p->filename[nlen-1] == 'g' || p->filename[nlen-1] == 'G'));

        char tmppath[PATH_MAX];
        if (snprintf(tmppath, sizeof(tmppath), "%s/img_%d%s",
                     tmpdir, i, png ? ".png" : ".jpg") >= (int)sizeof(tmppath))
            continue;

        int tfd = open(tmppath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (tfd < 0) continue;
        FILE *tf = fdopen(tfd, "wb");
        if (!tf) { close(tfd); unlink(tmppath); continue; }
        fwrite(p->data, 1, p->data_len, tf);
        fclose(tf);

        if (png) {
            img_name = tspdf_writer_add_png_image(doc, tmppath);
        } else {
            img_name = tspdf_writer_add_jpeg_image(doc, tmppath);
        }
        unlink(tmppath);

        if (!img_name) continue;

        TspdfStream *page = tspdf_writer_add_page(doc);
        double margin = 36;
        double aw = TSPDF_PAGE_A4_WIDTH - 2 * margin;
        double ah = TSPDF_PAGE_A4_HEIGHT - 2 * margin;
        tspdf_stream_draw_image(page, img_name, margin, margin, aw, ah);
        pages_added++;
    }

    rmdir(tmpdir);

    if (pages_added == 0) {
        tspdf_writer_destroy(doc);
        send_error(fd, 400, "No valid images uploaded");
        return;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(doc, &out, &out_len);
    tspdf_writer_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "images.pdf", out, out_len);
    free(out);
}

/* ── qrcode API handler ───────────────────────────────────────────── */

static void api_qrcode(int fd, MultipartForm *form)
{
    FormPart *cfg = form_find(form, "config");
    if (!cfg) { send_error(fd, 400, "Missing config"); return; }

    char config_buf[2048];
    size_t clen = cfg->data_len < sizeof(config_buf)-1 ? cfg->data_len : sizeof(config_buf)-1;
    memcpy(config_buf, cfg->data, clen);
    config_buf[clen] = '\0';

    char url[512] = "https://example.com";
    char title[256] = "";
    char subtitle[256] = "";
    char show_link_str[16] = "true";
    json_get_string(config_buf, "url", url, sizeof(url));
    json_get_string(config_buf, "title", title, sizeof(title));
    json_get_string(config_buf, "subtitle", subtitle, sizeof(subtitle));
    json_get_string(config_buf, "show_link", show_link_str, sizeof(show_link_str));
    int show_link = strcmp(show_link_str, "false") != 0;

    QrCode *qr = qr_encode(url);
    if (!qr) { send_error(fd, 500, "QR encode failed"); return; }

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { qr_free(qr); send_error(fd, 500, "Out of memory"); return; }

    if (title[0]) tspdf_writer_set_title(doc, title);
    tspdf_writer_set_creator(doc, "tspdf");

    const char *sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");

    double W = TSPDF_PAGE_A4_WIDTH, H = TSPDF_PAGE_A4_HEIGHT;
    TspdfStream *page = tspdf_writer_add_page(doc);

    tspdf_stream_set_fill_color(page, tspdf_color_rgb(1, 1, 1));
    tspdf_stream_rect(page, 0, 0, W, H);
    tspdf_stream_fill(page);

    double qr_pt = 200.0;
    double cell_size = qr_pt / (double)qr->size;
    double qr_x = (W - qr_pt) / 2.0;
    double qr_y = (H - qr_pt) / 2.0 - 20.0;

    tspdf_stream_set_fill_color(page, tspdf_color_rgb(0, 0, 0));
    for (int row = 0; row < qr->size; row++) {
        for (int col = 0; col < qr->size; col++) {
            if (qr->modules[row * qr->size + col]) {
                double px = qr_x + col * cell_size;
                double py = qr_y + (qr->size - 1 - row) * cell_size;
                tspdf_stream_rect(page, px, py, cell_size, cell_size);
                tspdf_stream_fill(page);
            }
        }
    }

    tspdf_stream_set_stroke_color(page, tspdf_color_from_u8(200, 205, 220));
    tspdf_stream_set_line_width(page, 1.0);
    tspdf_stream_rect(page, qr_x - 8, qr_y - 8, qr_pt + 16, qr_pt + 16);
    tspdf_stream_stroke(page);

    if (title[0]) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, bold, 22.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(20, 25, 60));
        double title_w = tspdf_writer_measure_text(doc, bold, 22.0, title);
        tspdf_stream_text_position(page, (W - title_w) / 2.0, qr_y + qr_pt + 32.0);
        tspdf_stream_show_text(page, title);
        tspdf_stream_end_text(page);
    }

    if (show_link) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, sans, 11.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(79, 110, 247));
        double url_w = tspdf_writer_measure_text(doc, sans, 11.0, url);
        tspdf_stream_text_position(page, (W - url_w) / 2.0, qr_y - 20.0);
        tspdf_stream_show_text(page, url);
        tspdf_stream_end_text(page);
    }

    if (subtitle[0]) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, sans, 10.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(130, 140, 170));
        double sub_w = tspdf_writer_measure_text(doc, sans, 10.0, subtitle);
        tspdf_stream_text_position(page, (W - sub_w) / 2.0, qr_y - 36.0);
        tspdf_stream_show_text(page, subtitle);
        tspdf_stream_end_text(page);
    }

    qr_free(qr);

    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(doc, &out, &out_len);
    tspdf_writer_destroy(doc);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "qrcode.pdf", out, out_len);
    free(out);
}

/* ── md2pdf API handler ───────────────────────────────────────────── *
 *
 * NOTE: this is a separate, deliberately smaller Markdown dialect than the
 * CLI converter in cli/cmd_md2pdf.c. The web path renders headings, bullet
 * lists, blockquotes, fenced code blocks, rules, and paragraphs — no pipe
 * tables, no inline bold/italic/code styling (markers render literally),
 * and no images: embedding ![alt](path) would mean reading server-side
 * files named by the request, which this endpoint must never do. When the
 * CLI dialect grows, decide explicitly whether the web path follows, and
 * keep docs/known-limitations.md and web/templates/tools/md2pdf.html in
 * sync. */

static double api_md2pdf_measure(const char *f, double s, const char *t, void *u) {
    TspdfWriter *doc = (TspdfWriter *)u;
    double w = tspdf_writer_measure_text(doc, f, s, t);
    return w > 0 ? w : (double)strlen(t) * s * 0.5;
}

static double api_md2pdf_lh(const char *f, double s, void *u) {
    TspdfWriter *doc = (TspdfWriter *)u;
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, f);
    if (ttf) return ttf_get_line_height(ttf, s);
    const TspdfBase14Metrics *b14 = tspdf_writer_get_base14(doc, f);
    if (b14) return tspdf_base14_line_height(b14, s);
    return s * 1.2;
}

static void api_md2pdf(int fd, MultipartForm *form)
{
    FormPart *cfg = form_find(form, "config");
    if (!cfg) { send_error(fd, 400, "Missing config"); return; }

    char *config_buf = malloc(cfg->data_len + 1);
    if (!config_buf) { send_error(fd, 500, "Out of memory"); return; }
    memcpy(config_buf, cfg->data, cfg->data_len);
    config_buf[cfg->data_len] = '\0';

    char *md = malloc(cfg->data_len + 1);
    if (!md) { free(config_buf); send_error(fd, 500, "Out of memory"); return; }
    json_get_string(config_buf, "text", md, cfg->data_len + 1);
    free(config_buf);

    if (!md[0]) { free(md); send_error(fd, 400, "Missing text in config"); return; }

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { free(md); send_error(fd, 500, "Out of memory"); return; }

    tspdf_writer_set_title(doc, "Document");
    tspdf_writer_set_creator(doc, "tspdf");

    const char *sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");
    const char *mono = tspdf_writer_add_builtin_font(doc, "Courier");

    TspdfArena arena = tspdf_arena_create(8 * 1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = api_md2pdf_measure;
    ctx.measure_userdata = doc;
    ctx.font_line_height = api_md2pdf_lh;
    ctx.line_height_userdata = doc;
    ctx.doc = doc;

    double W = TSPDF_PAGE_A4_WIDTH, H = TSPDF_PAGE_A4_HEIGHT;

    TspdfNode *root = tspdf_layout_box(&ctx);
    root->width = tspdf_size_fixed(W);
    root->height = tspdf_size_fit();
    root->direction = TSPDF_DIR_COLUMN;
    root->padding = tspdf_padding_all(52);
    root->gap = 8;
    TspdfBoxStyle *root_style = tspdf_layout_node_style(&ctx, root);
    root_style->has_background = true;
    root_style->background = tspdf_color_rgb(1, 1, 1);

    /* Parse markdown line by line */
    int in_code_block = 0;
    char code_buf[8192];
    int code_len = 0;

    char *line = md;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
        char saved = line[line_len];
        line[line_len] = '\0';
        if (line_len > 0 && line[line_len - 1] == '\r')
            line[line_len - 1] = '\0';

        if (!in_code_block && strncmp(line, "```", 3) == 0) {
            in_code_block = 1;
            code_len = 0;
            code_buf[0] = '\0';
        } else if (in_code_block && strncmp(line, "```", 3) == 0) {
            in_code_block = 0;
            TspdfNode *code_box = tspdf_layout_box(&ctx);
            code_box->width = tspdf_size_grow();
            code_box->direction = TSPDF_DIR_COLUMN;
            code_box->padding = tspdf_padding_all(8);
            TspdfBoxStyle *cs = tspdf_layout_node_style(&ctx, code_box);
            cs->has_background = true;
            cs->background = tspdf_color_from_u8(240, 242, 250);
            TspdfNode *code_txt = tspdf_layout_text(&ctx, code_buf, mono, 10);
            code_txt->text.color = tspdf_color_from_u8(60, 40, 120);
            code_txt->width = tspdf_size_grow();
            tspdf_layout_add_child(code_box, code_txt);
            tspdf_layout_add_child(root, code_box);
        } else if (in_code_block) {
            if (code_len > 0 && code_len < (int)sizeof(code_buf) - 2)
                code_buf[code_len++] = '\n';
            int remaining = (int)sizeof(code_buf) - code_len - 1;
            if (remaining > 0) {
                int copy = (int)line_len < remaining ? (int)line_len : remaining;
                memcpy(code_buf + code_len, line, copy);
                code_len += copy;
                code_buf[code_len] = '\0';
            }
        } else if (line[0] == '#') {
            int level = 0;
            while (line[level] == '#') level++;
            const char *text = line + level;
            while (*text == ' ') text++;
            double sizes[] = {24, 18, 14};
            double sz = level <= 3 ? sizes[level - 1] : 12;
            TspdfNode *node = tspdf_layout_text(&ctx, text, bold, sz);
            node->text.color = tspdf_color_from_u8(10, 15, 50);
            node->width = tspdf_size_grow();
            tspdf_layout_add_child(root, node);
        } else if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            TspdfNode *row = tspdf_layout_box(&ctx);
            row->direction = TSPDF_DIR_ROW;
            row->width = tspdf_size_grow();
            row->gap = 8;
            TspdfNode *bullet = tspdf_layout_text(&ctx, "\xe2\x80\xa2", sans, 11);
            bullet->text.color = tspdf_color_from_u8(79, 110, 247);
            tspdf_layout_add_child(row, bullet);
            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = tspdf_color_from_u8(40, 50, 80);
            tspdf_layout_add_child(row, txt);
            tspdf_layout_add_child(root, row);
        } else if (line[0] == '>' && line[1] == ' ') {
            TspdfNode *bq = tspdf_layout_box(&ctx);
            bq->width = tspdf_size_grow();
            bq->direction = TSPDF_DIR_COLUMN;
            bq->padding = (TspdfPadding){12, 8, 8, 8};
            TspdfBoxStyle *bs = tspdf_layout_node_style(&ctx, bq);
            bs->has_background = true;
            bs->background = tspdf_color_from_u8(245, 245, 250);
            bs->has_border_left = true;
            bs->border_left = 3;
            bs->border_color_left = tspdf_color_from_u8(79, 110, 247);
            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = tspdf_color_from_u8(80, 90, 110);
            tspdf_layout_add_child(bq, txt);
            tspdf_layout_add_child(root, bq);
        } else if ((strncmp(line, "---", 3) == 0 || strncmp(line, "***", 3) == 0) && line_len <= 5) {
            TspdfNode *hr = tspdf_layout_box(&ctx);
            hr->width = tspdf_size_grow();
            hr->height = tspdf_size_fixed(1);
            TspdfBoxStyle *hs = tspdf_layout_node_style(&ctx, hr);
            hs->has_background = true;
            hs->background = tspdf_color_from_u8(200, 205, 220);
            tspdf_layout_add_child(root, hr);
        } else if (line_len > 0) {
            TspdfNode *node = tspdf_layout_text(&ctx, line, sans, 11);
            node->text.wrap = TSPDF_WRAP_WORD;
            node->text.color = tspdf_color_from_u8(40, 50, 80);
            node->width = tspdf_size_grow();
            tspdf_layout_add_child(root, node);
        }

        line[line_len] = saved;
        line = eol ? eol + 1 : NULL;
    }

    free(md);

    TspdfPaginationResult pagination;
    int num_pages = tspdf_layout_compute_paginated(&ctx, root, W, H, &pagination);
    for (int pg = 0; pg < num_pages; pg++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        tspdf_layout_render_page_recompute(&ctx, root, &pagination, pg, page);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(doc, &out, &out_len);
    tspdf_writer_destroy(doc);
    tspdf_arena_destroy(&arena);

    if (err != TSPDF_OK) { free(out); send_error(fd, 500, "Save failed"); return; }
    send_pdf_response(fd, "document.pdf", out, out_len);
    free(out);
}

/* ── POST route dispatch ───────────────────────────────────────────── */

static void handle_post(int fd, HttpRequest *req, int port)
{
    /* Reject disallowed callers before touching the body so a remote page
     * cannot drive the local tool or read back generated PDFs. What this
     * covers depends on the bind tier (see the threat-model note above):
     * loopback binds reject rebound hostnames outright; 0.0.0.0 only blocks
     * foreign-Origin CSRF. */
    if (!request_is_local(req, port)) {
        send_error(fd, 403, "Forbidden: requests must originate from the local server");
        return;
    }

    /* Extract boundary from Content-Type */
    const char *bnd = find_content_type_param_value(req->content_type, "boundary");
    if (!bnd) { send_error(fd, 400, "Missing boundary in Content-Type"); return; }
    /* Remove optional quotes */
    char boundary[256];
    if (*bnd == '"') {
        bnd++;
        const char *q = strchr(bnd, '"');
        size_t len = q ? (size_t)(q - bnd) : strlen(bnd);
        if (len >= sizeof(boundary)) len = sizeof(boundary) - 1;
        memcpy(boundary, bnd, len);
        boundary[len] = '\0';
    } else {
        /* boundary ends at semicolon/space/end */
        size_t len = 0;
        while (bnd[len] && bnd[len] != ';' && bnd[len] != ' ' && bnd[len] != '\r')
            len++;
        if (len >= sizeof(boundary)) len = sizeof(boundary) - 1;
        memcpy(boundary, bnd, len);
        boundary[len] = '\0';
    }

    MultipartForm form;
    if (parse_multipart(req->body, req->body_len, boundary, &form) != 0) {
        send_error(fd, 400, "Failed to parse multipart form");
        return;
    }

    const char *path = req->path;

    if (strcmp(path, "/api/merge") == 0)              api_merge(fd, &form);
    else if (strcmp(path, "/api/split") == 0)          api_split(fd, &form);
    else if (strcmp(path, "/api/delete-pages") == 0)   api_delete_pages(fd, &form);
    else if (strcmp(path, "/api/rotate") == 0)         api_rotate(fd, &form);
    else if (strcmp(path, "/api/reorder") == 0)        api_reorder(fd, &form);
    else if (strcmp(path, "/api/compress") == 0)       api_compress(fd, &form);
    else if (strcmp(path, "/api/unlock") == 0)         api_unlock(fd, &form);
    else if (strcmp(path, "/api/password-protect") == 0) api_password_protect(fd, &form);
    else if (strcmp(path, "/api/metadata") == 0)       api_metadata(fd, &form);
    else if (strcmp(path, "/api/metadata-view") == 0)  api_metadata_view(fd, &form);
    else if (strcmp(path, "/api/watermark-existing") == 0) api_watermark(fd, &form);
    else if (strcmp(path, "/api/img2pdf") == 0)         api_img2pdf(fd, &form);
    else if (strcmp(path, "/api/qrcode") == 0)           api_qrcode(fd, &form);
    else if (strcmp(path, "/api/md2pdf") == 0)            api_md2pdf(fd, &form);
    else send_error(fd, 404, "Unknown API endpoint");
}

/* ── Main server loop ──────────────────────────────────────────────── */

static volatile int server_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    server_running = 0;
}

static int start_server(int port)
{
    struct sigaction sa = {0};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* No SA_RESTART — let accept() return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    init_tool_route_lens();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr = g_bind.addr;
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, 8) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    if (!g_bind.loopback) {
        fprintf(stderr, "WARNING: --bind %s exposes the web UI to the network; "
                        "it has no authentication and trusts every client that "
                        "can reach it.\n", g_bind.addr_str);
    }
    printf("tspdf server running at http://%s:%d\n",
           g_bind.loopback ? "localhost" : g_bind.addr_str, port);
    printf("Press Ctrl+C to stop.\n");

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* Bound blocking reads so one stalled client cannot monopolize the single-threaded server. */
        struct timeval request_read_timeout = {
            .tv_sec = REQUEST_READ_TIMEOUT_SEC,
            .tv_usec = 0
        };
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       &request_read_timeout, sizeof(request_read_timeout)) < 0) {
            send_error(client, 500, "Failed to configure request timeout");
            close(client);
            continue;
        }

        /* Bound blocking writes too: a client that stops reading the response
         * (e.g. a hanging fetch to localhost) must not pin write() forever. The
         * send loops in send_response/send_pdf_response already abort on a
         * non-positive return, so a fired SO_SNDTIMEO cleanly closes the socket. */
        struct timeval response_write_timeout = {
            .tv_sec = RESPONSE_WRITE_TIMEOUT_SEC,
            .tv_usec = 0
        };
        if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       &response_write_timeout, sizeof(response_write_timeout)) < 0) {
            send_error(client, 500, "Failed to configure response timeout");
            close(client);
            continue;
        }

        /* Read the full request. We allocate buf_cap+1 bytes and reserve the
         * final byte for a NUL we write before any C-string scan. All request
         * parsing is length-bounded (parse_request / scan_*), so the data never
         * depends on the terminator; the NUL is defense-in-depth so a stray
         * strstr/sscanf can never read past buf_len into adjacent heap. The +1
         * invariant must be preserved across every realloc below. */
        size_t buf_cap = 8192;
        char *buf = malloc(buf_cap + 1);
        if (!buf) {
            send_error(client, 500, "Out of memory");
            close(client);
            continue;
        }
        size_t buf_len = 0;
        int request_aborted = 0;

        /* Read until end of headers or I/O error */
        for (;;) {
            if (memmem(buf, buf_len, "\r\n\r\n", 4))
                break;

            if (buf_len >= buf_cap) {
                if (buf_cap >= MAX_REQUEST) {
                    send_error(client, 400, "Request too large");
                    request_aborted = 1;
                    break;
                }
                size_t new_cap = buf_cap <= MAX_REQUEST / 2 ? buf_cap * 2 : MAX_REQUEST;
                if (new_cap <= buf_cap)
                    new_cap = MAX_REQUEST;
                /* +1 keeps the reserved NUL slot; new_cap <= MAX_REQUEST so this
                 * cannot overflow. Capture into a temp so a failed realloc does
                 * not leak the original buffer. */
                void *nb = realloc(buf, new_cap + 1);
                if (!nb) {
                    send_error(client, 500, "Out of memory");
                    request_aborted = 1;
                    break;
                }
                buf = (char *)nb;
                buf_cap = new_cap;
            }

            ssize_t n = read(client, buf + buf_len, buf_cap - buf_len);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    send_error(client, 408, "Request Timeout");
                    request_aborted = 1;
                }
                break;
            }
            if (n == 0)
                break;
            buf_len += (size_t)n;
        }

        if (request_aborted) {
            close(client);
            free(buf);
            continue;
        }

        /* NUL-terminate in the reserved slot so any C-string scan over the
         * accumulated header bytes stops at buf_len rather than running into
         * adjacent heap. buf_len <= buf_cap and we allocated buf_cap+1. */
        buf[buf_len] = '\0';

        const char *hdr_end = memmem(buf, buf_len, "\r\n\r\n", 4);
        if (hdr_end) {
            /* hdr_end points at first \r of "\r\n\r\n"; include last header's CRLF in scans. */
            const char *hdr_scan_end = hdr_end + 2;

            int header_lines = 0;
            if (count_header_field_lines(buf, hdr_scan_end, &header_lines) != 0) {
                send_error(client, 400, "Bad request");
                close(client);
                free(buf);
                continue;
            }
            if (header_lines > MAX_HEADERS) {
                send_error(client, 400, "Bad request");
                close(client);
                free(buf);
                continue;
            }

            int have_cl = 0;
            size_t content_length = 0;
            if (scan_content_length_raw(buf, hdr_scan_end, &have_cl, &content_length) != 0) {
                send_error(client, 400, "Bad request");
                close(client);
                free(buf);
                continue;
            }

            if (have_cl) {
                size_t header_size = (size_t)(hdr_end - buf) + 4;
                if (content_length > MAX_REQUEST || header_size > MAX_REQUEST ||
                    content_length > MAX_REQUEST - header_size) {
                    send_error(client, 400, "Request too large");
                    close(client);
                    free(buf);
                    continue;
                }
                size_t total_needed = header_size + content_length;

                if (total_needed > buf_cap) {
                    /* +1 keeps the reserved NUL slot; total_needed <= MAX_REQUEST
                     * (checked above) so this cannot overflow. */
                    void *nb = realloc(buf, total_needed + 1);
                    if (!nb) {
                        send_error(client, 500, "Out of memory");
                        close(client);
                        free(buf);
                        continue;
                    }
                    buf = (char *)nb;
                    buf_cap = total_needed;
                }

                int incomplete_body = 0;
                while (buf_len < total_needed) {
                    ssize_t n = read(client, buf + buf_len, total_needed - buf_len);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            send_error(client, 408, "Request Timeout");
                        else
                            send_error(client, 400, "Bad request");
                        incomplete_body = 1;
                        break;
                    }
                    if (n == 0) {
                        send_error(client, 400, "Bad request");
                        incomplete_body = 1;
                        break;
                    }
                    buf_len += (size_t)n;
                }
                if (incomplete_body) {
                    close(client);
                    free(buf);
                    continue;
                }
            }
        }

        /* Re-terminate: buf_len may have grown while reading the body above. */
        buf[buf_len] = '\0';

        HttpRequest req;
        if (parse_request(buf, buf_len, &req) == 0) {
            if (strcmp(req.method, "GET") == 0) {
                handle_get(client, req.path);
            } else if (strcmp(req.method, "POST") == 0) {
                handle_post(client, &req, port);
            } else {
                send_error(client, 400, "Unsupported method");
            }
        } else {
            send_error(client, 400, "Bad request");
        }

        close(client);
        free(buf);
    }

    close(srv);
    printf("\nServer stopped.\n");
    return 0;
}

int cmd_serve(int argc, char **argv)
{
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf serve [--port <port>] [--bind <addr>]\n");
        printf("\nStart a local web server for PDF tools.\n");
        printf("Default port: 8080, default bind address: 127.0.0.1\n");
        printf("Binding a non-loopback address (e.g. 0.0.0.0) exposes the\n");
        printf("unauthenticated UI to the network; a warning is printed.\n");
        return 0;
    }

    int port = 8080;
    const char *port_str = find_flag(argc, argv, "--port");
    if (port_str) {
        errno = 0;
        char *end = NULL;
        long val = strtol(port_str, &end, 10);
        if (end == port_str || *end != '\0' || errno != 0 ||
            val < 1 || val > 65535) {
            fprintf(stderr,
                "error: invalid --port '%s' (must be an integer 1-65535)\n",
                port_str);
            return 1;
        }
        port = (int)val;
    }

    const char *bind_str = find_flag(argc, argv, "--bind");
    if (bind_str) {
        struct in_addr a;
        if (inet_pton(AF_INET, bind_str, &a) != 1) {
            fprintf(stderr,
                "error: invalid --bind '%s' (must be an IPv4 address, e.g. 127.0.0.1 or 0.0.0.0)\n",
                bind_str);
            return 1;
        }
        g_bind.addr = a;
        g_bind.loopback = (ntohl(a.s_addr) >> 24) == 127;
        g_bind.any = a.s_addr == htonl(INADDR_ANY);
        if (!inet_ntop(AF_INET, &a, g_bind.addr_str, sizeof(g_bind.addr_str))) {
            fprintf(stderr, "error: cannot format --bind address\n");
            return 1;
        }
    } else {
        g_bind.addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    return start_server(port);
}
