// src/crashreport/cr_claim.cpp — report id and claim JSON (spec §4.4).
#include "crashreport/cr_claim.h"

#include <algorithm>

#include "crashreport/cr_json.h"

namespace d2cr {

namespace {

constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool valid_fw(const std::string& fw) {
    if (fw == "unknown") return true;
    // ^[0-9]{1,2}\.[0-9]{2}$
    const size_t dot = fw.find('.');
    if (dot == std::string::npos || dot < 1 || dot > 2 || fw.size() != dot + 3) return false;
    for (size_t i = 0; i < fw.size(); ++i)
        if (i != dot && (fw[i] < '0' || fw[i] > '9')) return false;
    return true;
}

bool artifact_name(const std::string& n) {
    return n == "dump" || n == "crash_txt" || n == "crash_log" || n == "boot_progress";
}

}  // namespace

std::string new_ulid(int64_t now_ms, const uint8_t rand10[10]) {
    if (now_ms < 0 || now_ms >= (1LL << 48)) return std::string();
    char out[26];
    uint64_t t = (uint64_t)now_ms;
    for (int i = 9; i >= 0; --i) { out[i] = kCrockford[t & 31]; t >>= 5; }
    // 80 random bits, big endian, 16 characters of 5 bits.
    uint64_t hi = 0, lo = 0;   // hi: first 16 bits, lo: last 64 bits
    for (int i = 0; i < 2; ++i) hi = (hi << 8) | rand10[i];
    for (int i = 2; i < 10; ++i) lo = (lo << 8) | rand10[i];
    for (int i = 25; i >= 10; --i) {
        out[i] = kCrockford[lo & 31];
        lo = (lo >> 5) | ((hi & 31) << 59);
        hi >>= 5;
    }
    return std::string(out, sizeof out);
}

std::string build_claim_json(const ClaimInputs& in) {
    if (in.kind == Kind::None) return std::string();
    JsonWriter w;
    w.begin_object()
        .key("v").num(1)
        .key("report_id").str(in.report_id)
        .key("install_id").str(in.install_id)
        .key("build_id").str(in.build_id)
        .key("channel").str(in.channel)
        .key("platform").begin_object()
            .key("model").str(in.platform_model == "vita" || in.platform_model == "pstv" ? in.platform_model : "unknown")
            .key("fw").str(valid_fw(in.platform_fw) ? in.platform_fw : "unknown")
        .end_object()
        .key("session").begin_object()
            .key("started_unix").num(in.started_unix)
            .key("uptime_s");
    if (in.uptime_s < 0 || in.uptime_s > 0xFFFFFFFFLL) w.null(); else w.num(in.uptime_s);
    w.key("online").boolean(in.online).end_object()
        .key("kind").str(kind_name(in.kind))
        .key("features").raw(in.features_json.empty() ? "{}" : in.features_json)
        .key("hints").begin_array();
    // Only kinds strictly less severe than kind, once each, most severe first.
    std::vector<Kind> hints;
    for (Kind h : in.hints)
        if (h != Kind::None && h < in.kind && std::find(hints.begin(), hints.end(), h) == hints.end()) hints.push_back(h);
    std::sort(hints.begin(), hints.end(), [](Kind a, Kind b) { return a > b; });
    for (Kind h : hints) w.str(kind_name(h));
    w.end_array().key("artifacts").begin_array();
    std::vector<std::string> seen;
    for (const ClaimArtifact& a : in.artifacts) {
        if (!artifact_name(a.name) || std::find(seen.begin(), seen.end(), a.name) != seen.end()) continue;
        seen.push_back(a.name);
        w.begin_object().key("name").str(a.name).key("bytes").unum(a.bytes).end_object();
    }
    w.end_array();
    if (in.redactions >= 0) w.key("redactions").num(in.redactions);
    w.end_object();
    return w.out().size() > kClaimMaxBytes ? std::string() : w.out();
}

ClaimInputs claim_inputs_from_record(const ReportRecord& rec, const std::string& install_id,
                                     const std::string& channel, const std::string& platform_model) {
    ClaimInputs in;
    in.report_id = rec.report_id;
    in.install_id = install_id;
    in.build_id = rec.build_id;
    in.channel = channel;
    in.platform_model = platform_model;
    in.platform_fw = rec.fw.empty() ? "unknown" : rec.fw;
    in.started_unix = rec.started_unix;
    in.uptime_s = rec.uptime_s;
    in.online = rec.online;
    in.kind = rec.kind;
    in.hints = rec.hints;
    in.features_json = rec.features_json;
    for (const ArtifactFile& a : rec.artifacts) in.artifacts.push_back(ClaimArtifact{a.name, sealed_size(a.bytes)});
    in.redactions = rec.redactions;
    return in;
}

}  // namespace d2cr
