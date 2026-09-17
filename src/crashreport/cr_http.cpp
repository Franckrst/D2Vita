// src/crashreport/cr_http.cpp — the little HTTP/1.1 client the reporter needs.
#include "crashreport/cr_http.h"

#include <cstdio>
#include <cstring>

namespace d2cr {

namespace {

constexpr size_t kReadChunk = 2048;       // one platform read
constexpr size_t kMaxChunkLine = 256;     // chunk size line, extensions included
constexpr size_t kMaxTrailerBytes = 1024;

bool ascii_equal_ci(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return i == a.size() && !b[i];
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

bool digits_to_u64(const std::string& s, uint64_t* out) {
    if (s.empty() || s.size() > 19) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (uint64_t)(c - '0');
    }
    *out = v;
    return true;
}

bool hex_to_u64(const std::string& s, uint64_t* out) {
    if (s.empty() || s.size() > 15) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (uint64_t)d;
    }
    *out = v;
    return true;
}

}  // namespace

const char* http_error_name(HttpError e) {
    switch (e) {
        case HttpError::None: return "none";
        case HttpError::Cancelled: return "cancelled";
        case HttpError::BadUrl: return "bad_url";
        case HttpError::Resolve: return "resolve";
        case HttpError::Connect: return "connect";
        case HttpError::Send: return "send";
        case HttpError::Recv: return "recv";
        case HttpError::Timeout: return "timeout";
        case HttpError::Protocol: return "protocol";
        case HttpError::TooLarge: return "too_large";
        case HttpError::Body: break;
    }
    return "body";
}

// One exchange: the connection, the deadlines and what has been read so far.
struct HttpClient::Conn {
    NetHandle h = kNoHandle;
    uint64_t deadline = 0;        // whole exchange
    uint64_t idle_deadline = 0;   // no byte moved since
    std::string in;               // bytes read from the peer
    size_t pos = 0;               // parsing cursor in `in`
    bool peer_closed = false;
};

HttpClient::HttpClient(NetApi& net, const HttpLimits& limits) : net_(net), limits_(limits) {}

HttpError HttpClient::fail(HttpError e, const char* why) {
    detail_ = why;
    return e;
}

bool HttpClient::set_base_url(const std::string& url) {
    host_.clear();
    base_path_.clear();
    port_ = 80;
    have_address_ = false;
    const char* prefix = "http://";
    if (url.compare(0, std::strlen(prefix), prefix) != 0) return false;
    const size_t start = std::strlen(prefix);
    size_t end = url.find('/', start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::string path = end == std::string::npos ? std::string() : url.substr(end);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        uint64_t p = 0;
        if (!digits_to_u64(authority.substr(colon + 1), &p) || p == 0 || p > 65535) return false;
        port_ = (uint16_t)p;
        authority.resize(colon);
    }
    if (authority.empty()) return false;
    for (char c : authority)
        if (c <= ' ' || c == '/' || c == '?' || c == '#' || c == 0x7f) return false;
    while (!path.empty() && path.back() == '/') path.pop_back();
    for (char c : path)
        if (c <= ' ' || c == 0x7f) return false;
    host_ = authority;
    base_path_ = path;
    return true;
}

HttpError HttpClient::send_all(Conn& c, const char* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        if (stopped()) return fail(HttpError::Cancelled, "stopped while sending");
        const uint64_t now = net_.now_ms();
        if (now >= c.deadline) return fail(HttpError::Timeout, "the exchange took too long");
        if (now >= c.idle_deadline) return fail(HttpError::Timeout, "the peer stopped reading");
        size_t sent = 0;
        const NetResult r = net_.send(c.h, (const uint8_t*)data + done, n - done, limits_.slice_ms, &sent);
        if (r == NetResult::Ok) {
            done += sent;
            c.idle_deadline = net_.now_ms() + limits_.idle_timeout_ms;
            continue;
        }
        if (r == NetResult::Timeout) continue;
        return fail(HttpError::Send, "the connection broke while sending");
    }
    return HttpError::None;
}

// Reads at least one byte, or says why not.
HttpError HttpClient::fill(Conn& c) {
    for (;;) {
        if (stopped()) return fail(HttpError::Cancelled, "stopped while reading");
        const uint64_t now = net_.now_ms();
        if (now >= c.deadline) return fail(HttpError::Timeout, "the exchange took too long");
        if (now >= c.idle_deadline) return fail(HttpError::Timeout, "the peer stopped answering");
        uint8_t buf[kReadChunk];
        size_t got = 0;
        const NetResult r = net_.recv(c.h, buf, sizeof buf, limits_.slice_ms, &got);
        if (r == NetResult::Ok) {
            c.in.append((const char*)buf, got);
            c.idle_deadline = net_.now_ms() + limits_.idle_timeout_ms;
            return HttpError::None;
        }
        if (r == NetResult::Timeout) continue;
        if (r == NetResult::Closed) {
            c.peer_closed = true;
            return fail(HttpError::Recv, "the peer closed the connection");
        }
        return fail(HttpError::Recv, "the connection broke while reading");
    }
}

HttpError HttpClient::read_head(Conn& c, HttpResponse* out, bool* chunked, uint64_t* content_length,
                                bool* has_length) {
    for (;;) {                                  // 1xx answers are skipped
        size_t end;
        for (;;) {
            end = c.in.find("\r\n\r\n", c.pos);
            if (end != std::string::npos) break;
            if (c.in.size() - c.pos > limits_.max_head_bytes) return fail(HttpError::TooLarge, "head too large");
            const HttpError e = fill(c);
            if (e == HttpError::None) continue;
            if (e == HttpError::Recv && c.peer_closed)
                return fail(HttpError::Protocol, c.in.size() == c.pos ? "no answer at all" : "answer cut short");
            return e;
        }
        if (end - c.pos > limits_.max_head_bytes) return fail(HttpError::TooLarge, "head too large");
        const std::string head = c.in.substr(c.pos, end - c.pos);
        c.pos = end + 4;

        size_t line_end = head.find("\r\n");
        const std::string status_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);
        if (status_line.size() < 12 || status_line.compare(0, 7, "HTTP/1.") != 0 ||
            (status_line[7] != '0' && status_line[7] != '1') || status_line[8] != ' ' ||
            (status_line.size() > 12 && status_line[12] != ' '))
            return fail(HttpError::Protocol, "not an HTTP/1.x status line");
        int status = 0;
        for (int i = 9; i < 12; ++i) {
            if (status_line[(size_t)i] < '0' || status_line[(size_t)i] > '9')
                return fail(HttpError::Protocol, "bad status code");
            status = status * 10 + (status_line[(size_t)i] - '0');
        }

        out->status = status;
        out->signature.clear();
        *chunked = false;
        *has_length = false;
        *content_length = 0;
        size_t p = line_end == std::string::npos ? head.size() : line_end + 2;
        while (p < head.size()) {
            size_t e = head.find("\r\n", p);
            if (e == std::string::npos) e = head.size();
            const std::string line = head.substr(p, e - p);
            p = e + 2;
            if (line.empty()) continue;
            if (line[0] == ' ' || line[0] == '\t') return fail(HttpError::Protocol, "folded header line");
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) return fail(HttpError::Protocol, "header without a name");
            const std::string name = line.substr(0, colon);
            const std::string value = trim(line.substr(colon + 1));
            if (ascii_equal_ci(name, "content-length")) {
                uint64_t v = 0;
                if (!digits_to_u64(value, &v)) return fail(HttpError::Protocol, "bad Content-Length");
                if (*has_length && v != *content_length) return fail(HttpError::Protocol, "two Content-Length values");
                *content_length = v;
                *has_length = true;
            } else if (ascii_equal_ci(name, "transfer-encoding")) {
                if (!ascii_equal_ci(value, "chunked")) return fail(HttpError::Protocol, "unknown Transfer-Encoding");
                *chunked = true;
            } else if (ascii_equal_ci(name, "x-d2v-signature")) {
                out->signature = value;
            }
        }
        // Both framings at once is the request-smuggling shape; the Worker
        // never sends it, so it is refused rather than resolved.
        if (*chunked && *has_length) return fail(HttpError::Protocol, "Content-Length with chunked");
        if (status < 100 || status > 599) return fail(HttpError::Protocol, "status out of range");
        if (status >= 200) return HttpError::None;
        // 1xx: no body, read the real answer.
    }
}

HttpError HttpClient::read_body(Conn& c, HttpResponse* out, bool chunked, uint64_t content_length, bool has_length) {
    out->body.clear();
    if (chunked) {
        size_t trailer_bytes = 0;
        for (;;) {
            size_t e;
            for (;;) {
                e = c.in.find("\r\n", c.pos);
                if (e != std::string::npos) break;
                if (c.in.size() - c.pos > kMaxChunkLine) return fail(HttpError::Protocol, "chunk line too long");
                const HttpError err = fill(c);
                if (err != HttpError::None) return err;
            }
            const std::string line = c.in.substr(c.pos, e - c.pos);
            c.pos = e + 2;
            if (line.size() > kMaxChunkLine) return fail(HttpError::Protocol, "chunk line too long");
            // Extensions after ';' are allowed and ignored (RFC 7230 §4.1.1).
            const size_t semi = line.find(';');
            uint64_t size = 0;
            if (!hex_to_u64(trim(semi == std::string::npos ? line : line.substr(0, semi)), &size))
                return fail(HttpError::Protocol, "bad chunk size");
            if (size == 0) break;
            if (out->body.size() + size > limits_.max_body_bytes) return fail(HttpError::TooLarge, "body too large");
            while (c.in.size() - c.pos < size + 2) {
                const HttpError err = fill(c);
                if (err != HttpError::None) return err;
            }
            out->body.append(c.in, c.pos, (size_t)size);
            c.pos += (size_t)size;
            if (c.in.compare(c.pos, 2, "\r\n") != 0) return fail(HttpError::Protocol, "chunk not closed by CRLF");
            c.pos += 2;
        }
        for (;;) {                                  // trailers, then the final empty line
            size_t e;
            for (;;) {
                e = c.in.find("\r\n", c.pos);
                if (e != std::string::npos) break;
                if (c.in.size() - c.pos > kMaxTrailerBytes) return fail(HttpError::TooLarge, "trailer too large");
                const HttpError err = fill(c);
                if (err != HttpError::None) return err;
            }
            const size_t len = e - c.pos;
            c.pos = e + 2;
            if (len == 0) break;
            trailer_bytes += len;
            if (trailer_bytes > kMaxTrailerBytes) return fail(HttpError::TooLarge, "trailer too large");
        }
        return HttpError::None;
    }
    if (has_length) {
        if (content_length > limits_.max_body_bytes) return fail(HttpError::TooLarge, "body too large");
        while (c.in.size() - c.pos < content_length) {
            const HttpError err = fill(c);
            if (err != HttpError::None)
                return err == HttpError::Recv && c.peer_closed ? fail(HttpError::Protocol, "body cut short") : err;
        }
        out->body.assign(c.in, c.pos, (size_t)content_length);
        c.pos += (size_t)content_length;
        return HttpError::None;
    }
    // No framing: the body ends with the connection (HTTP/1.0 style).
    for (;;) {
        if (c.in.size() - c.pos > limits_.max_body_bytes) return fail(HttpError::TooLarge, "body too large");
        const HttpError err = fill(c);
        if (err == HttpError::Recv && c.peer_closed) break;
        if (err != HttpError::None) return err;
    }
    if (c.in.size() - c.pos > limits_.max_body_bytes) return fail(HttpError::TooLarge, "body too large");
    out->body.assign(c.in, c.pos, std::string::npos);
    c.pos = c.in.size();
    return HttpError::None;
}

// What the body callback writes goes straight out, counted so that exactly
// content_length bytes leave, and stopped as soon as the peer answers.
struct HttpClient::BodyPump {
    HttpClient* client = nullptr;
    Conn* conn = nullptr;
    uint64_t remaining = 0;
    uint64_t written = 0;
    HttpError error = HttpError::None;
    bool early = false;
};

// An origin-form request target (RFC 9112 section 3.2.1): "/" then visible
// ASCII only. A space would cut the request line in two and a CR or LF would
// append a header of the caller's choosing, so the client checks it itself
// instead of trusting every caller that pastes a name into a target.
static bool valid_request_target(const std::string& p) {
    if (p.empty() || p[0] != '/') return false;
    for (size_t i = 0; i < p.size(); ++i)
        if ((unsigned char)p[i] <= 0x20 || (unsigned char)p[i] >= 0x7F) return false;
    return true;
}

HttpError HttpClient::request(const HttpRequest& req, HttpResponse* out) {
    detail_.clear();
    if (!out) return fail(HttpError::BadUrl, "no response object");
    *out = HttpResponse();
    if (host_.empty()) return fail(HttpError::BadUrl, "no base URL");
    if (!valid_request_target(req.path)) return fail(HttpError::BadUrl, "the request target is not /visible-ascii");
    if (req.content_length > 0 && !req.body) return fail(HttpError::Body, "a body was announced but not given");
    if (stopped()) return fail(HttpError::Cancelled, "stopped before the request");

    if (!have_address_) {
        if (stopped()) return fail(HttpError::Cancelled, "stopped before resolving");
        if (!net_.resolve(host_, &address_, limits_.resolve_timeout_ms))
            return fail(HttpError::Resolve, "cannot resolve the host");
        have_address_ = true;
    }

    Conn c;
    const uint64_t start = net_.now_ms();
    c.deadline = start + limits_.total_timeout_ms;
    c.idle_deadline = start + limits_.idle_timeout_ms;
    c.h = net_.connect(address_, port_, limits_.connect_timeout_ms);
    if (c.h == kNoHandle) {
        have_address_ = false;              // a moved address is worth another lookup
        return fail(HttpError::Connect, "cannot connect");
    }

    std::string head;
    head.reserve(256);
    head += req.method;
    head += ' ';
    head += base_path_;
    head += req.path;
    head += " HTTP/1.1\r\nHost: ";
    head += host_;
    if (port_ != 80) {
        char p[8];
        std::snprintf(p, sizeof p, ":%u", (unsigned)port_);
        head += p;
    }
    head += "\r\nConnection: close\r\n";
    char len[48];
    std::snprintf(len, sizeof len, "Content-Length: %llu\r\n", (unsigned long long)req.content_length);
    head += len;
    for (const std::string& h : req.headers) {
        if (h.find('\r') != std::string::npos || h.find('\n') != std::string::npos) {
            net_.close(c.h);
            return fail(HttpError::Protocol, "a header holds a line break");
        }
        head += h;
        head += "\r\n";
    }
    head += "\r\n";

    HttpError e = send_all(c, head.data(), head.size());
    if (e == HttpError::None && req.content_length > 0) {
        BodyPump pump;
        pump.client = this;
        pump.conn = &c;
        pump.remaining = req.content_length;
        const bool ok = req.body(body_sink, &pump, req.body_ud);
        out->body_bytes_sent = pump.written;
        out->early = pump.early;
        if (pump.error != HttpError::None) {
            e = pump.error;
        } else if (!ok) {
            e = fail(HttpError::Body, "the body source failed");
        } else if (!pump.early && pump.remaining != 0) {
            e = fail(HttpError::Body, "the body source wrote fewer bytes than announced");
        }
    }
    if (e == HttpError::None || (out->early && (e == HttpError::Send || e == HttpError::Body))) {
        // An answer that arrived while the body was going out is worth reading
        // even if the rest of the body could not leave.
        bool chunked = false, has_length = false;
        uint64_t content_length = 0;
        e = read_head(c, out, &chunked, &content_length, &has_length);
        if (e == HttpError::None) e = read_body(c, out, chunked, content_length, has_length);
    }
    net_.close(c.h);
    if (e != HttpError::None) out->status = 0;
    return e;
}

bool HttpClient::body_sink(const uint8_t* data, size_t n, void* ud) {
    BodyPump* p = static_cast<BodyPump*>(ud);
    HttpClient* self = p->client;
    if (n == 0) return true;
    if (n > p->remaining) {
        p->error = self->fail(HttpError::Body, "the body source wrote more bytes than announced");
        return false;
    }
    // An answer already waiting means the server decided without the body
    // (409 exists, 403 bad_token, 413): stop pushing a dump into it.
    size_t got = 0;
    uint8_t peek[kReadChunk];
    const NetResult r = self->net_.recv(p->conn->h, peek, sizeof peek, 0, &got);
    if (r == NetResult::Ok && got > 0) {
        p->conn->in.append((const char*)peek, got);
        p->early = true;
        return false;
    }
    if (r == NetResult::Closed) {
        p->conn->peer_closed = true;
        p->early = !p->conn->in.empty();
        if (!p->early) p->error = self->fail(HttpError::Send, "the peer closed while the body was going out");
        return false;
    }
    const HttpError e = self->send_all(*p->conn, (const char*)data, n);
    if (e != HttpError::None) {
        p->error = e;
        return false;
    }
    p->remaining -= n;
    p->written += n;
    return true;
}

}  // namespace d2cr
