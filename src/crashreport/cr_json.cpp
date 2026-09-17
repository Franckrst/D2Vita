// src/crashreport/cr_json.cpp — minimal ordered JSON writer and strict reader.
#include "crashreport/cr_json.h"

#include <cstdio>
#include <cstring>

namespace d2cr {

namespace {

// Length of the well-formed UTF-8 sequence at the start of [p, p + avail), or
// 0 if the bytes there do not form one (RFC 3629: no overlongs, no surrogates,
// no code point above U+10FFFF).
size_t utf8_seq_len_raw(const char* p, size_t avail) {
    const unsigned char c0 = (unsigned char)p[0];
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;          // allowed range of the 2nd byte
    if (c0 >= 0xC2 && c0 <= 0xDF) n = 2;
    else if (c0 == 0xE0) { n = 3; lo = 0xA0; }
    else if ((c0 >= 0xE1 && c0 <= 0xEC) || c0 == 0xEE || c0 == 0xEF) n = 3;
    else if (c0 == 0xED) { n = 3; hi = 0x9F; }
    else if (c0 == 0xF0) { n = 4; lo = 0x90; }
    else if (c0 >= 0xF1 && c0 <= 0xF3) n = 4;
    else if (c0 == 0xF4) { n = 4; hi = 0x8F; }
    else return 0;
    if (n > avail) return 0;
    const unsigned char c1 = (unsigned char)p[1];
    if (c1 < lo || c1 > hi) return 0;
    for (size_t k = 2; k < n; ++k) {
        const unsigned char ck = (unsigned char)p[k];
        if (ck < 0x80 || ck > 0xBF) return 0;
    }
    return n;
}

size_t utf8_seq_len(const std::string& s, size_t i) {
    return utf8_seq_len_raw(s.data() + i, s.size() - i);
}

// UTF-8 of one code point (already checked to be a scalar value).
void append_utf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        *out += (char)cp;
    } else if (cp < 0x800) {
        *out += (char)(0xC0 | (cp >> 6));
        *out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += (char)(0xE0 | (cp >> 12));
        *out += (char)(0x80 | ((cp >> 6) & 0x3F));
        *out += (char)(0x80 | (cp & 0x3F));
    } else {
        *out += (char)(0xF0 | (cp >> 18));
        *out += (char)(0x80 | ((cp >> 12) & 0x3F));
        *out += (char)(0x80 | ((cp >> 6) & 0x3F));
        *out += (char)(0x80 | (cp & 0x3F));
    }
}

// Four hexadecimal digits at p (4 bytes available).
bool hex4(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

}  // namespace

std::string json_quote(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 2);
    r += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\b': r += "\\b"; break;
                case '\f': r += "\\f"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c); r += b; }
                    else r += (char)c;
            }
            ++i;
            continue;
        }
        const size_t n = utf8_seq_len(s, i);
        if (n == 0) { r += "\\ufffd"; ++i; continue; }   // one replacement per bad byte
        r.append(s, i, n);
        i += n;
    }
    r += '"';
    return r;
}

void JsonWriter::before_value() {
    if (after_key_) { after_key_ = false; return; }
    if (!has_items_.empty()) {
        if (has_items_.back()) out_ += ',';
        has_items_.back() = true;
    }
}

JsonWriter& JsonWriter::begin_object() { before_value(); out_ += '{'; has_items_.push_back(false); return *this; }
JsonWriter& JsonWriter::end_object() { out_ += '}'; if (!has_items_.empty()) has_items_.pop_back(); return *this; }
JsonWriter& JsonWriter::begin_array() { before_value(); out_ += '['; has_items_.push_back(false); return *this; }
JsonWriter& JsonWriter::end_array() { out_ += ']'; if (!has_items_.empty()) has_items_.pop_back(); return *this; }

JsonWriter& JsonWriter::key(const std::string& k) {
    before_value();
    out_ += json_quote(k);
    out_ += ':';
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::str(const std::string& v) { before_value(); out_ += json_quote(v); return *this; }

JsonWriter& JsonWriter::num(int64_t v) {
    before_value();
    char b[32]; std::snprintf(b, sizeof b, "%lld", (long long)v); out_ += b;
    return *this;
}

JsonWriter& JsonWriter::unum(uint64_t v) {
    before_value();
    char b[32]; std::snprintf(b, sizeof b, "%llu", (unsigned long long)v); out_ += b;
    return *this;
}

JsonWriter& JsonWriter::boolean(bool v) { before_value(); out_ += v ? "true" : "false"; return *this; }
JsonWriter& JsonWriter::null() { before_value(); out_ += "null"; return *this; }
JsonWriter& JsonWriter::raw(const std::string& json) { before_value(); out_ += json; return *this; }

// ---------------------------------------------------------------- reader

JsonLimits JsonLimits::vectors() {
    JsonLimits l;
    l.max_nodes = 200000;
    l.max_bytes = 16 * 1024 * 1024;
    return l;
}

bool JsonDoc::fail(const char* why) {
    if (error_.empty()) error_ = why;
    return false;
}

int JsonDoc::add_node() {
    if (nodes_.size() >= limits_.max_nodes) { fail("too many values"); return -1; }
    nodes_.push_back(JsonNode());
    return (int)(nodes_.size() - 1);
}

bool JsonDoc::skip_space() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    return p_ < end_;
}

bool JsonDoc::parse_string(std::string* out, size_t* codepoints) {
    ++p_;                                   // opening quote
    out->clear();
    *codepoints = 0;
    for (;;) {
        if (p_ >= end_) return fail("unterminated string");
        const unsigned char c = (unsigned char)*p_;
        if (c == '"') { ++p_; return true; }
        if (c < 0x20) return fail("control character in a string");
        if (c == '\\') {
            ++p_;
            if (p_ >= end_) return fail("unterminated escape");
            const char e = *p_++;
            switch (e) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (end_ - p_ < 4 || !hex4(p_, &cp)) return fail("bad \\u escape");
                    p_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        uint32_t low = 0;
                        if (hex4(p_ + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            p_ += 6;
                        } else {
                            cp = 0xFFFD;    // lone surrogate: one replacement character
                        }
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
            ++*codepoints;
            continue;
        }
        if (c < 0x80) { *out += (char)c; ++p_; ++*codepoints; continue; }
        const size_t n = utf8_seq_len_raw(p_, (size_t)(end_ - p_));
        if (n == 0) return fail("invalid UTF-8 in a string");
        out->append(p_, n);
        p_ += n;
        ++*codepoints;
    }
}

bool JsonDoc::parse_number(JsonNode* node) {
    const bool negative = *p_ == '-';
    if (negative) ++p_;
    if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad number");
    std::string digits;
    if (*p_ == '0') {
        digits += '0';
        ++p_;
        if (p_ < end_ && *p_ >= '0' && *p_ <= '9') return fail("leading zero in a number");
    } else {
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') digits += *p_++;
    }
    int64_t fraction = 0;
    if (p_ < end_ && *p_ == '.') {
        ++p_;
        if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad fraction");
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') { digits += *p_++; ++fraction; }
    }
    int64_t exponent = 0;
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
        ++p_;
        bool exp_negative = false;
        if (p_ < end_ && (*p_ == '+' || *p_ == '-')) exp_negative = *p_++ == '-';
        if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad exponent");
        int64_t v = 0;
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
            if (v < 1000000) v = v * 10 + (*p_ - '0');
            ++p_;
        }
        exponent = exp_negative ? -v : v;
    }
    // Exact integer value, as JSON Schema reads it (1420, 1420.0, 1.42e3).
    size_t lead = 0;
    while (lead + 1 < digits.size() && digits[lead] == '0') ++lead;
    digits.erase(0, lead);
    bool all_zero = true;
    for (char c : digits) if (c != '0') all_zero = false;
    const int64_t scale = exponent - fraction;
    bool integer = true;
    if (all_zero) {
        digits = "0";
    } else if (scale < 0) {
        const uint64_t strip = (uint64_t)(-scale);
        if (strip >= digits.size()) {
            integer = false;
        } else {
            for (size_t i = digits.size() - (size_t)strip; i < digits.size(); ++i)
                if (digits[i] != '0') integer = false;
            if (integer) digits.resize(digits.size() - (size_t)strip);
        }
    } else if (scale > 0) {
        if ((uint64_t)scale + digits.size() > 20) integer = false;
        else digits.append((size_t)scale, '0');
    }
    uint64_t value = 0;
    if (integer) {
        for (char c : digits) {
            const uint64_t d = (uint64_t)(c - '0');
            if (value > (UINT64_MAX - d) / 10) { integer = false; break; }
            value = value * 10 + d;
        }
    }
    if (negative && value != 0) integer = false;
    node->type = JsonType::Number;
    node->integer = integer;
    node->uvalue = integer ? value : 0;
    return true;
}

int JsonDoc::parse_value(size_t depth) {
    if (depth > limits_.max_depth) { fail("too deeply nested"); return -1; }
    if (!skip_space()) { fail("truncated document"); return -1; }
    const char c = *p_;
    const int index = add_node();
    if (index < 0) return -1;
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        ++p_;
        nodes_[(size_t)index].type = object ? JsonType::Object : JsonType::Array;
        int last = -1;
        for (;;) {
            if (!skip_space()) { fail("truncated document"); return -1; }
            if (*p_ == (object ? '}' : ']')) { ++p_; break; }
            if (nodes_[(size_t)index].count > 0) {
                if (*p_ != ',') { fail("expected , or the end of the value"); return -1; }
                ++p_;
                if (!skip_space()) { fail("truncated document"); return -1; }
            }
            std::string key;
            if (object) {
                size_t key_codepoints = 0;
                if (*p_ != '"') { fail("expected a member name"); return -1; }
                if (!parse_string(&key, &key_codepoints)) return -1;
                if (!skip_space() || *p_ != ':') { fail("expected :"); return -1; }
                ++p_;
                for (int m = nodes_[(size_t)index].first; m >= 0; m = nodes_[(size_t)m].next)
                    if (nodes_[(size_t)m].key == key) { fail("duplicate member"); return -1; }
            }
            const int child = parse_value(depth + 1);
            if (child < 0) return -1;
            nodes_[(size_t)child].key.swap(key);
            if (last < 0) nodes_[(size_t)index].first = child;
            else nodes_[(size_t)last].next = child;
            last = child;
            ++nodes_[(size_t)index].count;
        }
        return index;
    }
    if (c == '"') {
        nodes_[(size_t)index].type = JsonType::String;
        std::string text;
        size_t codepoints = 0;
        if (!parse_string(&text, &codepoints)) return -1;
        nodes_[(size_t)index].text.swap(text);
        nodes_[(size_t)index].codepoints = codepoints;
        return index;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (!parse_number(&nodes_[(size_t)index])) return -1;
        return index;
    }
    struct { const char* word; size_t len; JsonType type; bool value; } literals[] = {
        {"true", 4, JsonType::Bool, true}, {"false", 5, JsonType::Bool, false}, {"null", 4, JsonType::Null, false},
    };
    for (const auto& l : literals) {
        if ((size_t)(end_ - p_) >= l.len && std::memcmp(p_, l.word, l.len) == 0) {
            p_ += l.len;
            nodes_[(size_t)index].type = l.type;
            nodes_[(size_t)index].bval = l.value;
            return index;
        }
    }
    fail("not a JSON value");
    return -1;
}

bool JsonDoc::parse(const std::string& text, const JsonLimits& limits) {
    nodes_.clear();
    error_.clear();
    limits_ = limits;
    if (text.size() > limits_.max_bytes) { error_ = "document too large"; return false; }
    p_ = text.data();
    end_ = text.data() + text.size();
    const bool ok = parse_value(1) >= 0 && (!skip_space() || fail("trailing content"));
    p_ = end_ = nullptr;
    if (!ok) {
        nodes_.clear();
        if (error_.empty()) error_ = "invalid JSON";
    }
    return ok;
}

const JsonNode* JsonDoc::member(const JsonNode& object, const char* key) const {
    if (object.type != JsonType::Object) return nullptr;
    for (int m = object.first; m >= 0; m = nodes_[(size_t)m].next)
        if (nodes_[(size_t)m].key == key) return &nodes_[(size_t)m];
    return nullptr;
}

}  // namespace d2cr
