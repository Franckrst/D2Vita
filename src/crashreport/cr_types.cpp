// src/crashreport/cr_types.cpp — kind names, address formatting, session.txt.
#include "crashreport/cr_types.h"

#include <cstdio>

namespace d2cr {

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Hang:         return "hang";
        case Kind::GuestFault:   return "guest_fault";
        case Kind::AbnormalExit: return "abnormal_exit";
        case Kind::Halt:         return "halt";
        case Kind::HostFault:    return "host_fault";
        case Kind::None:         break;
    }
    return "none";
}

std::string format_addr(const Addr& a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "+0x%x", (unsigned)a.offset);
    return a.module + buf;
}

namespace {

// Strict unsigned parse: decimal, or hexadecimal with a 0x prefix. The whole
// value must be consumed and fit in `max`.
bool parse_u64(const std::string& v, uint64_t max, uint64_t* out) {
    if (v.empty()) return false;
    uint64_t r = 0;
    size_t i = 0;
    unsigned base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { base = 16; i = 2; }
    for (; i < v.size(); ++i) {
        const char c = v[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return false;
        if (r > (max - d) / base) return false;
        r = r * base + d;
    }
    *out = r;
    return true;
}

bool parse_u32(const std::string& v, uint32_t* out) {
    uint64_t r;
    if (!parse_u64(v, 0xFFFFFFFFull, &r)) return false;
    *out = (uint32_t)r;
    return true;
}

// Values are single-line by construction: a line break inside a value would
// let it forge another key.
std::string one_line(const std::string& v) {
    std::string r = v;
    for (char& c : r) if (c == '\n' || c == '\r') c = ' ';
    return r;
}

}  // namespace

bool parse_session(const std::string& text, SessionRecord* out) {
    SessionRecord s;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        bool ok = true;
        if (k == "session_id") s.session_id = v;
        else if (k == "build_id") s.build_id = v;
        else if (k == "write_root") s.write_root = v;
        else if (k == "progress_path") s.progress_path = v;
        else if (k == "state") s.state = v;
        else if (k == "stop_reason") s.stop_reason = v;
        else if (k == "started_unix") {
            uint64_t t;
            ok = parse_u64(v, 0x7FFFFFFFFFFFFFFFull, &t);
            if (ok) s.started_unix = (int64_t)t;
        }
        else if (k == "game_base") ok = parse_u32(v, &s.game_base);
        else if (k == "arena_host_base") ok = parse_u32(v, &s.arena_host_base);
        else if (k == "eboot_base") ok = parse_u32(v, &s.eboot_base);
        else if (k == "eboot_size") ok = parse_u32(v, &s.eboot_size);
        else if (k == "jit_lo") ok = parse_u32(v, &s.jit_lo);
        else if (k == "jit_hi") ok = parse_u32(v, &s.jit_hi);
        else if (k == "main_exit") { ok = parse_u32(v, &s.main_exit); s.has_main_exit = ok; }
        // Unknown keys are ignored: a newer build may add fields.
        if (!ok) return false;
    }
    if (s.session_id.empty()) return false;
    *out = s;
    return true;
}

std::string serialize_session(const SessionRecord& s) {
    std::string r;
    auto str = [&r](const char* k, const std::string& v) { r += k; r += '='; r += one_line(v); r += '\n'; };
    auto hex = [&r](const char* k, uint32_t v) {
        char b[32]; std::snprintf(b, sizeof b, "%s=0x%08x\n", k, (unsigned)v); r += b; };
    str("session_id", s.session_id);
    str("build_id", s.build_id);
    { char b[48]; std::snprintf(b, sizeof b, "started_unix=%lld\n", (long long)s.started_unix); r += b; }
    str("write_root", s.write_root);
    str("progress_path", s.progress_path);
    str("state", s.state);
    hex("game_base", s.game_base);
    hex("arena_host_base", s.arena_host_base);
    hex("eboot_base", s.eboot_base);
    hex("eboot_size", s.eboot_size);
    hex("jit_lo", s.jit_lo);
    hex("jit_hi", s.jit_hi);
    str("stop_reason", s.stop_reason);
    if (s.has_main_exit) hex("main_exit", s.main_exit);
    return r;
}

}  // namespace d2cr
