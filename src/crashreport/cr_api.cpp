// src/crashreport/cr_api.cpp — reading the answers of API v1.
#include "crashreport/cr_api.h"

#include <cstring>

#include "crashreport/cr_json.h"

namespace d2cr {

namespace {

constexpr size_t kMessageMaxCodepoints = 500;
constexpr uint64_t kMinSealedBytes = 88;     // header + one tag: the smallest object
constexpr size_t kUploadTokenMax = 1024;

struct NamedError { ApiError code; const char* name; int status; };
const NamedError kErrors[] = {
    {ApiError::InvalidPayload, "invalid_payload", 400},
    {ApiError::Unauthorized, "unauthorized", 401},
    {ApiError::UnknownBuild, "unknown_build", 403},
    {ApiError::BadToken, "bad_token", 403},
    {ApiError::Turnstile, "turnstile", 403},
    {ApiError::NotFound, "not_found", 404},
    {ApiError::MethodNotAllowed, "method_not_allowed", 405},
    {ApiError::Exists, "exists", 409},
    {ApiError::Incomplete, "incomplete", 409},
    {ApiError::PayloadTooLarge, "payload_too_large", 413},
    {ApiError::RateLimited, "rate_limited", 429},
    {ApiError::InternalError, "internal_error", 500},
    {ApiError::NotAccepting, "not_accepting", 503},
};

// One member of `object`, with the expected type; null when absent or of
// another type.
const JsonNode* member_of(const JsonDoc& doc, const JsonNode& object, const char* key, JsonType type) {
    const JsonNode* n = doc.member(object, key);
    return n && n->type == type ? n : nullptr;
}

// Every member of `object` must be one of `names`.
bool only_members(const JsonDoc& doc, const JsonNode& object, const char* const* names, size_t count) {
    for (const JsonNode* m = doc.first(object); m; m = doc.next(*m)) {
        bool known = false;
        for (size_t i = 0; i < count && !known; ++i) known = m->key == names[i];
        if (!known) return false;
    }
    return true;
}

bool is_version_one(const JsonDoc& doc, const JsonNode& object) {
    const JsonNode* v = member_of(doc, object, "v", JsonType::Number);
    return v && v->is_u64() && v->u64() == 1;
}

bool u32_member(const JsonDoc& doc, const JsonNode& object, const char* key, bool* present, uint64_t* out) {
    const JsonNode* n = doc.member(object, key);
    if (!n || n->type == JsonType::Null) {
        *present = false;
        *out = 0;
        return n != nullptr;                 // the member must exist, null is allowed
    }
    if (n->type != JsonType::Number || !n->is_u64() || n->u64() > 0xFFFFFFFFull) return false;
    *present = true;
    *out = n->u64();
    return true;
}

// SignatureId of decision.v1: ^S[A-Z2-7]{15}$
bool valid_signature_id(const std::string& s) {
    if (s.size() != 16 || s[0] != 'S') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

// UploadToken: ^[A-Za-z0-9._~+/=-]+$, 1 to 1024 characters.
bool valid_upload_token(const std::string& s) {
    if (s.empty() || s.size() > kUploadTokenMax) return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '~' || c == '+' || c == '/' || c == '=' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool sealed_size_in_caps(const std::string& name, uint64_t bytes) {
    const uint64_t cap = artifact_sealed_cap(name);
    return cap != 0 && bytes >= kMinSealedBytes && bytes <= cap;
}

}  // namespace

const char* api_error_name(ApiError e) {
    for (const NamedError& n : kErrors)
        if (n.code == e) return n.name;
    return "none";
}

int api_error_status(ApiError e) {
    for (const NamedError& n : kErrors)
        if (n.code == e) return n.status;
    return 0;
}

bool valid_report_id(const std::string& s) {
    // ReportId: ^[0-7][0-9A-HJKMNP-TV-Z]{25}$
    if (s.size() != 26 || s[0] < '0' || s[0] > '7') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O' && c != 'U';
        if (!digit && !letter) return false;
    }
    return true;
}

bool valid_artifact_name(const std::string& s) {
    return s == "dump" || s == "crash_txt" || s == "crash_log" || s == "boot_progress";
}

uint64_t artifact_sealed_cap(const std::string& name) {
    if (name == "dump") return 2 * 1024 * 1024;
    if (name == "crash_txt" || name == "crash_log") return 64 * 1024;
    if (name == "boot_progress") return 320 * 1024;
    return 0;
}

bool parse_decision(const std::string& body, Decision* out) {
    JsonDoc doc;
    if (!doc.parse(body)) return false;
    const JsonNode* root = doc.root();
    if (!root || root->type != JsonType::Object) return false;
    static const char* const kMembers[] = {"v", "report_id", "signature", "action", "upload", "retry_after_s",
                                           "disable_until_unix"};
    if (root->count != 7 || !only_members(doc, *root, kMembers, sizeof kMembers / sizeof *kMembers)) return false;
    if (!is_version_one(doc, *root)) return false;
    const JsonNode* report_id = member_of(doc, *root, "report_id", JsonType::String);
    const JsonNode* signature = member_of(doc, *root, "signature", JsonType::String);
    const JsonNode* action = member_of(doc, *root, "action", JsonType::String);
    if (!report_id || !signature || !action) return false;
    if (!valid_report_id(report_id->text) || !valid_signature_id(signature->text)) return false;
    if (action->text != "upload" && action->text != "count_only") return false;

    Decision d;
    d.report_id = report_id->text;
    d.signature = signature->text;
    d.upload = action->text == "upload";
    if (!u32_member(doc, *root, "retry_after_s", &d.has_retry_after, &d.retry_after_s)) return false;
    if (!u32_member(doc, *root, "disable_until_unix", &d.has_disable_until, &d.disable_until_unix)) return false;

    const JsonNode* upload = doc.member(*root, "upload");
    if (!upload) return false;
    if (!d.upload) {
        if (upload->type != JsonType::Null) return false;    // count_only: upload is null
        *out = d;
        return true;
    }
    if (upload->type != JsonType::Object || upload->count != 3) return false;
    static const char* const kUploadMembers[] = {"token", "expires_unix", "artifacts"};
    if (!only_members(doc, *upload, kUploadMembers, 3)) return false;
    const JsonNode* token = member_of(doc, *upload, "token", JsonType::String);
    const JsonNode* expires = member_of(doc, *upload, "expires_unix", JsonType::Number);
    const JsonNode* artifacts = member_of(doc, *upload, "artifacts", JsonType::Array);
    if (!token || !expires || !artifacts) return false;
    if (!valid_upload_token(token->text)) return false;
    if (!expires->is_u64() || expires->u64() > 0xFFFFFFFFull) return false;
    if (artifacts->count < 1 || artifacts->count > 4) return false;
    d.token = token->text;
    d.expires_unix = expires->u64();
    for (const JsonNode* a = doc.first(*artifacts); a; a = doc.next(*a)) {
        if (a->type != JsonType::Object || a->count != 2) return false;
        static const char* const kArtifactMembers[] = {"name", "max_bytes"};
        if (!only_members(doc, *a, kArtifactMembers, 2)) return false;
        const JsonNode* name = member_of(doc, *a, "name", JsonType::String);
        const JsonNode* max_bytes = member_of(doc, *a, "max_bytes", JsonType::Number);
        if (!name || !max_bytes || !max_bytes->is_u64()) return false;
        if (!valid_artifact_name(name->text) || !sealed_size_in_caps(name->text, max_bytes->u64())) return false;
        for (const ArtifactRequest& seen : d.artifacts)
            if (seen.name == name->text) return false;        // one entry per name
        d.artifacts.push_back(ArtifactRequest{name->text, max_bytes->u64()});
    }
    *out = d;
    return true;
}

bool parse_artifact_stored(const std::string& body, ArtifactStored* out) {
    JsonDoc doc;
    if (!doc.parse(body)) return false;
    const JsonNode* root = doc.root();
    if (!root || root->type != JsonType::Object || root->count != 4) return false;
    static const char* const kMembers[] = {"v", "report_id", "name", "bytes"};
    if (!only_members(doc, *root, kMembers, 4) || !is_version_one(doc, *root)) return false;
    const JsonNode* report_id = member_of(doc, *root, "report_id", JsonType::String);
    const JsonNode* name = member_of(doc, *root, "name", JsonType::String);
    const JsonNode* bytes = member_of(doc, *root, "bytes", JsonType::Number);
    if (!report_id || !name || !bytes || !bytes->is_u64()) return false;
    if (!valid_report_id(report_id->text) || !valid_artifact_name(name->text)) return false;
    if (!sealed_size_in_caps(name->text, bytes->u64())) return false;
    out->report_id = report_id->text;
    out->name = name->text;
    out->bytes = bytes->u64();
    return true;
}

bool parse_complete_response(const std::string& body, CompleteResponse* out) {
    JsonDoc doc;
    if (!doc.parse(body)) return false;
    const JsonNode* root = doc.root();
    if (!root || root->type != JsonType::Object || root->count != 3) return false;
    static const char* const kMembers[] = {"v", "report_id", "sample_stored"};
    if (!only_members(doc, *root, kMembers, 3) || !is_version_one(doc, *root)) return false;
    const JsonNode* report_id = member_of(doc, *root, "report_id", JsonType::String);
    const JsonNode* stored = member_of(doc, *root, "sample_stored", JsonType::Bool);
    if (!report_id || !stored || !valid_report_id(report_id->text)) return false;
    out->report_id = report_id->text;
    out->sample_stored = stored->bval;
    return true;
}

bool parse_error_body(const std::string& body, ErrorBody* out) {
    JsonDoc doc;
    if (!doc.parse(body)) return false;
    const JsonNode* root = doc.root();
    if (!root || root->type != JsonType::Object) return false;
    static const char* const kMembers[] = {"v", "error", "message", "retry_after_s", "disable_until_unix",
                                           "report_id", "artifact"};
    if (!only_members(doc, *root, kMembers, sizeof kMembers / sizeof *kMembers)) return false;
    if (!is_version_one(doc, *root)) return false;
    const JsonNode* code = member_of(doc, *root, "error", JsonType::String);
    const JsonNode* message = member_of(doc, *root, "message", JsonType::String);
    if (!code || !message || message->codepoints > kMessageMaxCodepoints) return false;

    ErrorBody e;
    for (const NamedError& n : kErrors)
        if (code->text == n.name) e.error = n.code;
    if (e.error == ApiError::None) return false;

    const JsonNode* retry = doc.member(*root, "retry_after_s");
    if (retry) {
        if (retry->type != JsonType::Number || !retry->is_u64() || retry->u64() > 0xFFFFFFFFull) return false;
        e.has_retry_after = true;
        e.retry_after_s = retry->u64();
    }
    const JsonNode* disable = doc.member(*root, "disable_until_unix");
    if (disable) {
        if (disable->type != JsonType::Number || !disable->is_u64() || disable->u64() > 0xFFFFFFFFull) return false;
        e.has_disable_until = true;
        e.disable_until_unix = disable->u64();
    }
    const JsonNode* report_id = doc.member(*root, "report_id");
    if (report_id) {
        if (report_id->type != JsonType::String || !valid_report_id(report_id->text)) return false;
        e.report_id = report_id->text;
    }
    const JsonNode* artifact = doc.member(*root, "artifact");
    if (artifact) {
        if (artifact->type != JsonType::String || !valid_artifact_name(artifact->text)) return false;
        e.artifact = artifact->text;
    }
    // Conditional requirements of the schema.
    if (e.error == ApiError::RateLimited && !e.has_retry_after) return false;
    if (e.error == ApiError::NotAccepting && !e.has_disable_until) return false;
    if (e.error == ApiError::Exists && (e.report_id.empty() || e.artifact.empty())) return false;
    if (e.error == ApiError::Incomplete && e.report_id.empty()) return false;
    *out = e;
    return true;
}

}  // namespace d2cr
