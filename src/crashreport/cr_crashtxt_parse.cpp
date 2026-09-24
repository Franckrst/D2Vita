// src/crashreport/cr_crashtxt_parse.cpp — facts from the game's own Crash.txt.
//
// Format sources (Game.exe 1.14d, reference binary ~/d2-vita-refs/1.14d):
//   "[%s] (%s) failed at %s(%i)"   .rdata 0x6cd25c, pushed at VA 0x4089d4 by
//                                  the common reporter VA 0x408900; Halt call
//                                  sites pass file = "" (VA 0x6cc837), hence
//                                  "[Halt] (Unrecoverable internal error %08x)
//                                  failed at (1420)"
//   "<Inspector.LineNumber>%u"     same string table
//   "Location : %s, line #%d"      same string table (no code reference found
//                                  in 1.14d; accepted when present)
//   "DBG-ADDR<%p>(\"%s\")%s"       .rdata 0x6ffc24, pushed at VA 0x402dbf
//   "    Base:%08lXh  Size:%7lXh  Name:%-15.15s  Path:%s"   .rdata 0x6cd84c,
//                                  pushed at VA 0x40bc88
// Section layout ("<Inspector.Summary:>" ... "<:Inspector.Summary>",
// "<Inspector.Assertion:>" frames "<:Inspector.Assertion>", "Registers:",
// "Threads:") as in a public 1.14d crash log.
#include "crashreport/cr_crashtxt_parse.h"

#include "crashreport/cr_addr.h"

namespace d2cr {

namespace {

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 1..8 hex digits at s[*pos]; advances.
bool hex_at(const std::string& s, size_t* pos, uint32_t* out) {
    size_t n = 0;
    uint32_t v = 0;
    while (*pos + n < s.size() && hexval(s[*pos + n]) >= 0) {
        if (n == 8) return false;
        v = (v << 4) | (uint32_t)hexval(s[*pos + n]);
        ++n;
    }
    if (n == 0) return false;
    *pos += n;
    *out = v;
    return true;
}

// Whole string is a decimal number that fits in int.
bool dec_all(const std::string& s, int* out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

std::string base_name(const std::string& path) {
    const size_t sl = path.find_last_of("\\/");
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

bool iequals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return i == a.size() && !b[i];
}

// A bare exception name as the whole summary line: what Fog's top-level
// filter writes for a hardware exception it reports itself ("ACCESS_VIOLATION"
// -- reached on console once the runtime delivers guest faults to it). No
// line number: code stays 0, the frames carry the signature.
bool parse_exception_summary(const std::string& line, HaltFacts* h) {
    if (line.size() < 4 || line.size() > 40) return false;
    for (char c : line)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    if (line.find('_') == std::string::npos) return false;   // ACCESS_VIOLATION, STACK_OVERFLOW... not a lone word
    h->error_type = line;
    h->code = 0;
    return true;
}

// "[%s] (%s) failed at %s(%i)"
bool parse_summary(const std::string& line, HaltFacts* h, std::string* file) {
    if (line.size() < 4 || line[0] != '[' || line.back() != ')') return false;
    const size_t close = line.find("] (");
    const size_t fa = line.rfind(") failed at ");
    const size_t open = line.rfind('(');
    if (close == std::string::npos || fa == std::string::npos || open == std::string::npos) return false;
    if (!(close < fa && fa + 12 <= open)) return false;
    int code;
    if (!dec_all(line.substr(open + 1, line.size() - 2 - open), &code)) return false;
    h->error_type = line.substr(1, close - 1);
    h->code = code;
    *file = line.substr(fa + 12, open - (fa + 12));
    return true;
}

}  // namespace

HaltFacts parse_crash_txt(const std::string& text, uint32_t game_base) {
    HaltFacts h;
    bool have_summary = false, have_line_number = false;
    int line_number = 0;
    std::string summary_file, location_file;
    int location_line = 0;
    uint32_t module_base = 0;
    bool in_assertion = false, in_summary = false;
    std::vector<uint32_t> raw;           // DBG-ADDR addresses of the halting thread

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "<Inspector.Assertion:>") { in_assertion = true; continue; }
        if (line == "<:Inspector.Assertion>") { in_assertion = false; continue; }
        if (line == "<Inspector.Summary:>") { in_summary = true; continue; }
        if (line == "<:Inspector.Summary>") { in_summary = false; continue; }

        if (in_assertion && line.compare(0, 9, "DBG-ADDR<") == 0) {
            size_t p = 9;
            uint32_t addr;
            if (hex_at(line, &p, &addr) && line.compare(p, 3, ">(\"") == 0) {
                const size_t q = line.find("\")", p + 3);
                // The module name is not trusted: frames are classified by
                // address against the Game.exe base below.
                if (q != std::string::npos) raw.push_back(addr);
            }
            continue;
        }
        if (!have_summary && !line.empty() && line[0] == '[') {
            std::string file;
            if (parse_summary(line, &h, &file)) { have_summary = true; summary_file = file; }
            continue;
        }
        if (!have_summary && in_summary && parse_exception_summary(line, &h)) { have_summary = true; continue; }
        static const char kLineNumber[] = "<Inspector.LineNumber>";
        if (line.compare(0, sizeof kLineNumber - 1, kLineNumber) == 0) {
            int n;
            if (dec_all(line.substr(sizeof kLineNumber - 1), &n)) { have_line_number = true; line_number = n; }
            continue;
        }
        static const char kLocation[] = "Location : ";
        if (line.compare(0, sizeof kLocation - 1, kLocation) == 0) {
            const size_t comma = line.rfind(", line #");
            int n;
            if (comma != std::string::npos && comma > sizeof kLocation - 1 && dec_all(line.substr(comma + 8), &n)) {
                location_file = line.substr(sizeof kLocation - 1, comma - (sizeof kLocation - 1));
                location_line = n;
            }
            continue;
        }
        static const char kBase[] = "    Base:";
        if (module_base == 0 && line.compare(0, sizeof kBase - 1, kBase) == 0) {
            size_t p = sizeof kBase - 1;
            uint32_t base, size;
            if (hex_at(line, &p, &base) && line.compare(p, 8, "h  Size:") == 0) {
                p += 8;
                while (p < line.size() && line[p] == ' ') ++p;
                const size_t name_at = line.find("h  Name:", p);
                const size_t path_at = line.find("  Path:", p);
                if (hex_at(line, &p, &size) && name_at == p && path_at != std::string::npos && path_at > p + 8) {
                    std::string name = line.substr(p + 8, path_at - (p + 8));
                    while (!name.empty() && name.back() == ' ') name.pop_back();
                    if (iequals(name, "Game.exe")) module_base = base;
                }
            }
            continue;
        }
    }

    if (!have_summary && have_line_number) {
        h.code = line_number;
        h.error_type.clear();
    }
    h.is_halt = have_summary || have_line_number;
    if (!h.is_halt) return HaltFacts();

    if (have_summary && !summary_file.empty())
        h.location = base_name(summary_file) + ":" + std::to_string(h.code);
    else if (!location_file.empty())
        h.location = base_name(location_file) + ":" + std::to_string(location_line);

    h.game_base = game_base ? game_base : module_base;
    if (h.game_base == 0) return h;       // no base: frames would be guesses
    bool leading = true;
    for (size_t i = 0; i < raw.size() && h.frames.size() < 16; ++i) {
        const uint32_t rva = raw[i] - h.game_base;
        if (raw[i] < h.game_base || rva >= kGame114dImageSize) continue;   // other modules
        if (leading && rva >= kFogReporterRvaLo && rva < kFogReporterRvaHi) { ++h.reporter_frames_removed; continue; }
        leading = false;
        Addr a;
        a.module = "Game";
        a.offset = rva;
        h.frames.push_back(a);
    }
    return h;
}

}  // namespace d2cr
