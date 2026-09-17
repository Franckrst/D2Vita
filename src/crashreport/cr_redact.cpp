// src/crashreport/cr_redact.cpp — secret patterns, in-place redaction, stream scan.
#include "crashreport/cr_redact.h"

#include <algorithm>
#include <cstring>

namespace d2cr {

namespace {

inline uint8_t fold(uint8_t c) { return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c; }

// Zeroes the whole buffer, up to its capacity (bytes past size() may hold
// an older, longer value), without reallocating it.
void wipe_str(std::string& s) {
    if (s.capacity() > 0) {
        s.resize(s.capacity());
        std::memset(&s[0], 0, s.size());
    }
    s.clear();
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

bool is_alnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Length matched by `p` at data[i], 0 for no match, kNeedMore when the
// bytes stop before the pattern is decided.
size_t match_one(const SecretPattern& p, const uint8_t* data, size_t n, size_t i) {
    const size_t w = p.utf16 ? 2 : 1;
    size_t j = i, k = 0;
    bool prev_sep = false;
    while (k < p.units.size()) {
        if (j + w > n) return SecretPatterns::kNeedMore;
        const uint8_t c = data[j];
        if (p.utf16 && data[j + 1] != 0) return 0;
        if (fold(c) == (uint8_t)p.units[k]) { j += w; ++k; prev_sep = false; continue; }
        if (p.separators && k > 0 && !prev_sep && (c == '-' || c == ' ')) { j += w; prev_sep = true; continue; }
        return 0;
    }
    return j - i;
}

}  // namespace

SecretPatterns::SecretPatterns(const SecretPatterns& o) : pats_(o.pats_), max_len_(o.max_len_) { rebuild_index(); }

SecretPatterns& SecretPatterns::operator=(const SecretPatterns& o) {
    if (this != &o) {
        wipe();
        pats_ = o.pats_;
        max_len_ = o.max_len_;
        rebuild_index();
    }
    return *this;
}

SecretPatterns::~SecretPatterns() { wipe(); }

void SecretPatterns::wipe() {
    for (SecretPattern& p : pats_) wipe_str(p.units);
    pats_.clear();
    max_len_ = 0;
    rebuild_index();
}

void SecretPatterns::rebuild_index() {
    for (auto& b : first_) b.clear();
    for (size_t i = 0; i < pats_.size(); ++i)
        if (!pats_[i].units.empty()) first_[(uint8_t)pats_[i].units[0]].push_back((uint16_t)i);
}

void SecretPatterns::add(const std::string& units, bool separators) {
    if (units.empty()) return;
    for (int u = 0; u < 2; ++u) {
        const bool utf16 = (u == 1);
        bool dup = false;
        for (const SecretPattern& p : pats_)
            if (p.units == units && p.utf16 == utf16 && p.separators == separators) dup = true;
        if (dup || pats_.size() >= 0xFFFF) continue;
        if (pats_.size() == pats_.capacity()) {
            // Grow by hand: a vector reallocation would move short names
            // (kept inside the string object) and free the old storage
            // with those bytes still in it.
            std::vector<SecretPattern> bigger;
            bigger.reserve(pats_.size() * 2 + 4);
            for (SecretPattern& p : pats_) {
                bigger.emplace_back();
                bigger.back().units.assign(p.units);
                bigger.back().utf16 = p.utf16;
                bigger.back().separators = p.separators;
                wipe_str(p.units);
            }
            pats_.swap(bigger);
        }
        pats_.emplace_back();   // filled in place: no temporary copy left to free
        SecretPattern& sp = pats_.back();
        sp.units = units;
        sp.utf16 = utf16;
        sp.separators = separators;
        const size_t chars = separators ? 2 * units.size() - 1 : units.size();
        max_len_ = std::max(max_len_, chars * (utf16 ? 2 : 1));
    }
    rebuild_index();
}

void SecretPatterns::add_key(const std::string& key) {
    // Same normalization as the game-side reader (cdkeys_file.cpp:196-200):
    // dashes, spaces and tabs ignored, 16 or 26 alphanumerics.
    std::string clean;
    clean.reserve(key.size());   // one buffer, never reallocated with a partial key in it
    for (char c : key)
        if (c != '-' && c != ' ' && c != '\t' && c != '\r' && c != '\n') clean += (char)fold((uint8_t)c);
    bool ok = clean.size() == 16 || clean.size() == 26;
    for (char c : clean) if (!is_alnum((uint8_t)c)) ok = false;
    if (ok) add(clean, true);
    wipe_str(clean);
}

void SecretPatterns::add_name(const std::string& name) {
    std::string t = trim(name);
    if (t.size() >= kMinSecretNameLen) {
        for (char& c : t) c = (char)fold((uint8_t)c);
        add(t, false);
    }
    wipe_str(t);
}

size_t SecretPatterns::match_at(const uint8_t* data, size_t n, size_t i, bool final, bool* utf16) const {
    if (i >= n) return final ? 0 : kNeedMore;
    size_t best = 0;
    bool best_utf16 = false, need_more = false;
    for (uint16_t idx : first_[fold(data[i])]) {
        const size_t r = match_one(pats_[idx], data, n, i);
        if (r == kNeedMore) { need_more = true; continue; }
        if (r > best) { best = r; best_utf16 = pats_[idx].utf16; }
    }
    if (need_more && !final) return kNeedMore;
    if (best && utf16) *utf16 = best_utf16;
    return best;
}

SecretPatterns build_patterns(const std::string& keys_txt, const std::vector<std::string>& accounts) {
    SecretPatterns p;
    size_t pos = 0;
    while (pos < keys_txt.size()) {
        size_t nl = keys_txt.find('\n', pos);
        if (nl == std::string::npos) nl = keys_txt.size();
        std::string s = keys_txt.substr(pos, nl - pos);
        pos = nl + 1;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        const size_t a = s.find_first_not_of(" \t");
        const size_t eq = s.find('=');
        if (a != std::string::npos && s[a] != '#' && eq != std::string::npos && eq > a) {
            std::string name = s.substr(a, eq - a);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            for (char& c : name) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            std::string value = s.substr(eq + 1);
            if (name == "classic" || name == "lod") p.add_key(value);
            else if (name == "owner" || name == "classic_owner" || name == "lod_owner") p.add_name(value);
            wipe_str(value);
        }
        wipe_str(s);
    }
    for (const std::string& acc : accounts) p.add_name(acc);
    return p;
}

std::vector<std::string> accounts_from_registry(const std::string& registry_txt) {
    // Read in place: copies of a line or of its fields would carry the hex of
    // the account name into freed memory.
    static const char kKey[] = "software\\blizzard entertainment\\diablo ii";
    static const char kValue[] = "last bnet";
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::string> out;
    out.reserve(kMaxRegistryAccounts);   // never reallocated with names inside
    const size_t n = registry_txt.size();
    size_t pos = 0;
    while (pos < n) {
        size_t end = registry_txt.find('\n', pos);
        if (end == std::string::npos) end = n;
        const size_t next = end + 1;
        if (end > pos && registry_txt[end - 1] == '\r') --end;
        // key|value|Name|type|hex: the four bars of this line.
        size_t bars[4];
        size_t count = 0;
        for (size_t i = pos; i < end && count <= 4; ++i)
            if (registry_txt[i] == '|') { if (count < 4) bars[count] = i; ++count; }
        const bool shape = count == 4 && bars[0] - pos == sizeof kKey - 1 &&
                           registry_txt.compare(pos, sizeof kKey - 1, kKey) == 0 &&
                           bars[1] - bars[0] - 1 == sizeof kValue - 1 &&
                           registry_txt.compare(bars[0] + 1, sizeof kValue - 1, kValue) == 0 &&
                           bars[3] - bars[2] == 2 && registry_txt[bars[2] + 1] == '1';
        const size_t hex_lo = shape ? bars[3] + 1 : 0, hex_len = shape ? end - hex_lo : 0;
        pos = next;
        if (!shape || hex_len % 2 || out.size() >= kMaxRegistryAccounts) continue;
        std::string name;
        name.reserve(hex_len / 2);
        bool ok = true;
        for (size_t i = 0; i < hex_len; i += 2) {
            const int hi = hv(registry_txt[hex_lo + i]), lo = hv(registry_txt[hex_lo + i + 1]);
            if (hi < 0 || lo < 0) { ok = false; break; }
            const char c = (char)(hi * 16 + lo);
            if (c == '\0') break;
            name += c;
        }
        if (ok && !name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        wipe_str(name);
    }
    return out;
}

int redact_in_place(std::string* s, const SecretPatterns& p) {
    if (p.empty() || s->empty()) return 0;
    const uint8_t* data = (const uint8_t*)s->data();
    const size_t n = s->size();
    int count = 0;
    size_t i = 0;
    while (i < n) {
        bool utf16 = false;
        const size_t len = p.match_at(data, n, i, true, &utf16);
        if (len == 0) { ++i; continue; }
        if (utf16) { for (size_t k = i; k < i + len; k += 2) (*s)[k] = 'X'; }
        else std::memset(&(*s)[i], 'X', len);
        ++count;
        i += len;
    }
    return count;
}

StreamScanner::StreamScanner(const SecretPatterns& p) : p_(p) { carry_.reserve(p.max_match_len()); }

StreamScanner::~StreamScanner() {
    if (!carry_.empty()) std::memset(carry_.data(), 0, carry_.size());
}

void StreamScanner::feed(const uint8_t* data, size_t n) {
    const size_t ml = p_.max_match_len();
    if (finished_ || ml == 0 || n == 0) return;
    const uint64_t carry_end = base_ + carry_.size();
    const uint64_t total_end = carry_end + n;
    // Positions p with p + ml <= total_end are decidable now.
    const uint64_t decide_end = total_end >= ml ? total_end - ml + 1 : base_;
    uint8_t window[512];
    std::vector<uint8_t> big_window;
    uint8_t* win = window;
    if (ml > sizeof window) { big_window.resize(ml); win = big_window.data(); }

    for (uint64_t pos = std::max(base_, skip_until_); pos < decide_end && pos < carry_end; ++pos) {
        if (pos < skip_until_) continue;
        size_t w = 0;
        for (uint64_t q = pos; q < pos + ml; ++q, ++w)
            win[w] = q < carry_end ? carry_[(size_t)(q - base_)] : data[(size_t)(q - carry_end)];
        bool u16;
        const size_t len = p_.match_at(win, w, 0, false, &u16);
        if (len && len != SecretPatterns::kNeedMore) { ++hits_; skip_until_ = pos + len; }
    }
    for (uint64_t pos = std::max(carry_end, skip_until_); pos < decide_end; ++pos) {
        if (pos < skip_until_) continue;
        bool u16;
        const size_t len = p_.match_at(data, n, (size_t)(pos - carry_end), false, &u16);
        if (len && len != SecretPatterns::kNeedMore) { ++hits_; skip_until_ = pos + len; }
    }
    if (ml > sizeof window) std::memset(big_window.data(), 0, big_window.size());
    else std::memset(window, 0, ml);

    uint64_t keep_from = std::max<uint64_t>(decide_end, base_);
    if (skip_until_ > keep_from) keep_from = std::min(skip_until_, total_end);
    std::vector<uint8_t> next;
    next.reserve(ml);
    for (uint64_t q = keep_from; q < total_end; ++q)
        next.push_back(q < carry_end ? carry_[(size_t)(q - base_)] : data[(size_t)(q - carry_end)]);
    if (!carry_.empty()) std::memset(carry_.data(), 0, carry_.size());
    carry_.swap(next);
    base_ = keep_from;
}

void StreamScanner::finish() {
    if (finished_) return;
    finished_ = true;
    const uint64_t end = base_ + carry_.size();
    for (uint64_t pos = std::max(base_, skip_until_); pos < end; ++pos) {
        if (pos < skip_until_) continue;
        bool u16;
        const size_t len = p_.match_at(carry_.data(), carry_.size(), (size_t)(pos - base_), true, &u16);
        if (len) { ++hits_; skip_until_ = pos + len; }
    }
    if (!carry_.empty()) std::memset(carry_.data(), 0, carry_.size());
    carry_.clear();
}

}  // namespace d2cr
