// src/crashreport/cr_upload.cpp — sending a report (spec §4.8).
#include "crashreport/cr_upload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "crashreport/cr_claim.h"
#include "crashreport/cr_json.h"

namespace d2cr {

namespace {

constexpr size_t kFileReadChunk = 64 * 1024;

// A body already in memory (the claim, the complete request).
struct StringBody {
    const std::string* text = nullptr;
};

bool string_body(HttpBodySinkFn sink, void* sink_ud, void* ud) {
    const StringBody* b = static_cast<StringBody*>(ud);
    if (!b->text || b->text->empty()) return true;
    return sink((const uint8_t*)b->text->data(), b->text->size(), sink_ud);
}

// A file, sealed on its way to the socket: the file is read by chunks, each
// chunk is fed to the sealer, and the sealer's output goes straight out. One
// chunk of plaintext and one of ciphertext live at a time.
struct SealedFileBody {
    IoApi* io = nullptr;
    std::string path;
    const uint8_t* recipient_pk = nullptr;
    RandomBytesFn random = nullptr;
    void* random_ud = nullptr;
    HttpBodySinkFn sink = nullptr;
    void* sink_ud = nullptr;
    Sealer sealer;
};

bool sealed_to_socket(const uint8_t* data, size_t n, void* ud) {
    SealedFileBody* b = static_cast<SealedFileBody*>(ud);
    return b->sink(data, n, b->sink_ud);
}

bool file_to_sealer(const uint8_t* data, size_t n, void* ud) {
    SealedFileBody* b = static_cast<SealedFileBody*>(ud);
    return b->sealer.feed(data, n);
}

bool sealed_file_body(HttpBodySinkFn sink, void* sink_ud, void* ud) {
    SealedFileBody* b = static_cast<SealedFileBody*>(ud);
    b->sink = sink;
    b->sink_ud = sink_ud;
    if (!b->sealer.begin(b->recipient_pk, b->random, b->random_ud, sealed_to_socket, b)) return false;
    if (!b->io->read_stream(b->path, kFileReadChunk, file_to_sealer, b)) return false;
    return b->sealer.finish();
}

std::string decimal(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", (long long)v);
    return b;
}

}  // namespace

const char* report_outcome_name(ReportOutcome o) {
    switch (o) {
        case ReportOutcome::Skipped: return "skipped";
        case ReportOutcome::Deleted: return "deleted";
        case ReportOutcome::Completed: return "completed";
        case ReportOutcome::Failed: return "failed";
        case ReportOutcome::Gated: return "gated";
        case ReportOutcome::UnknownBuild: return "unknown_build";
        case ReportOutcome::Stopped: break;
    }
    return "stopped";
}

bool parse_outbox_gate(const std::string& text, OutboxGate* out) {
    OutboxGate g;
    bool have = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "not_before_unix") {
            char* end = nullptr;
            const long long v = std::strtoll(value.c_str(), &end, 10);
            if (!end || *end) return false;
            g.not_before_unix = v;
            have = true;
        } else if (key == "reason") {
            g.reason = value;
        }
    }
    if (!have) return false;
    *out = g;
    return true;
}

std::string serialize_outbox_gate(const OutboxGate& g) {
    std::string reason = g.reason;
    for (char& c : reason)
        if (c == '\n' || c == '\r') c = ' ';
    return "not_before_unix=" + decimal(g.not_before_unix) + "\nreason=" + reason + "\n";
}

// One answer of the API: verified, matching its schema, and naming this very
// request.
struct Uploader::Answer {
    bool ok = false;
    int status = 0;
    bool is_error = false;
    ErrorBody error;
    Decision decision;
    ArtifactStored stored;
    CompleteResponse complete;
    HttpError http_error = HttpError::None;
};

Uploader::Uploader(IoApi& io, NetApi& net, Outbox& outbox, const UploadConfig& cfg, const UploadKeys& keys,
                   const UploadEnv& env)
    : io_(io), net_(net), outbox_(outbox), cfg_(cfg), keys_(keys), env_(env), http_(net, cfg.http) {}

std::vector<std::string> Uploader::headers_for(const std::string& build_id) const {
    std::vector<std::string> h;
    h.push_back("X-D2V-Client: d2vita/" + build_id);
    h.push_back("X-D2V-Install: " + cfg_.install_id);
    return h;
}

void Uploader::load_gate() {
    std::string text;
    OutboxGate g;
    if (io_.read_file(gate_path(), &text, 4096) && parse_outbox_gate(text, &g)) gate_ = g;
}

void Uploader::save_gate(int64_t not_before, const char* reason) {
    if (not_before <= gate_.not_before_unix) return;      // a gate only moves forward
    gate_.not_before_unix = not_before;
    gate_.reason = reason ? reason : "";
    const std::string path = gate_path(), tmp = path + ".new";
    DirEntry de;
    if (io_.stat(tmp, &de)) io_.remove(tmp);
    if (!io_.write_file(tmp, serialize_outbox_gate(gate_))) return;
    if (io_.stat(path, &de) && !io_.remove(path)) return;
    io_.rename(tmp, path);
}

bool Uploader::call(Shape shape, const HttpRequest& req, const std::string& report_id, const std::string& artifact,
                    uint64_t sent_bytes, Answer* out) {
    *out = Answer();
    HttpResponse resp;
    ++stats_.requests;
    const HttpError e = http_.request(req, &resp);
    out->http_error = e;
    if (e != HttpError::None) {
        detail_ = std::string("http ") + http_error_name(e) + ": " + http_.error_detail();
        network_down_ = e == HttpError::Resolve || e == HttpError::Connect;
        return false;
    }
    out->status = resp.status;
    // Verified before it is read (contract README.md, response signatures).
    if (!verify_response(keys_.response_pk, resp.signature, resp.body)) {
        detail_ = resp.signature.empty() ? "answer without a signature" : "answer with an invalid signature";
        return false;
    }
    if (resp.status >= 200 && resp.status < 300) {
        switch (shape) {
            case Shape::Decision:
                if (resp.status != 200 || !parse_decision(resp.body, &out->decision)) {
                    detail_ = "the body is not a decision.v1";
                    return false;
                }
                if (out->decision.report_id != report_id) {
                    detail_ = "the decision names another report";
                    return false;
                }
                break;
            case Shape::Stored:
                if (resp.status != 201 || !parse_artifact_stored(resp.body, &out->stored)) {
                    detail_ = "the body is not an ArtifactStored";
                    return false;
                }
                if (out->stored.report_id != report_id || out->stored.name != artifact ||
                    out->stored.bytes != sent_bytes) {
                    detail_ = "the answer names another piece";
                    return false;
                }
                break;
            case Shape::Complete:
                if (resp.status != 200 || !parse_complete_response(resp.body, &out->complete)) {
                    detail_ = "the body is not a CompleteResponse";
                    return false;
                }
                if (out->complete.report_id != report_id) {
                    detail_ = "the answer names another report";
                    return false;
                }
                break;
        }
        out->ok = true;
        return true;
    }
    if (!parse_error_body(resp.body, &out->error)) {
        detail_ = "the error body is not an admin.v1#ErrorBody";
        return false;
    }
    if (api_error_status(out->error.error) != resp.status) {
        detail_ = std::string("error ") + api_error_name(out->error.error) + " with status " + decimal(resp.status);
        return false;
    }
    if (!out->error.report_id.empty() && out->error.report_id != report_id) {
        detail_ = "the error names another report";
        return false;
    }
    if (!out->error.artifact.empty() && out->error.artifact != artifact) {
        detail_ = "the error names another piece";
        return false;
    }
    out->is_error = true;
    out->ok = true;
    return true;
}

bool Uploader::call_retrying(Shape shape, const HttpRequest& req, const std::string& report_id,
                             const std::string& artifact, uint64_t sent_bytes, int64_t token_expires, Answer* out) {
    for (int attempt = 0;; ++attempt) {
        if (stopped()) {
            detail_ = "stopped";
            *out = Answer();
            return false;
        }
        const bool ok = call(shape, req, report_id, artifact, sent_bytes, out);
        // "The console repeats a request that got no verified answer", plus
        // internal_error, which says the Worker could not serve it this time.
        // The other 5xx of v1 is the kill switch (503 not_accepting), which
        // asks for the opposite: stop until disable_until_unix.
        const bool again = !ok || (out->is_error && out->error.error == ApiError::InternalError);
        if (!again || network_down_ || attempt >= cfg_.max_request_retries) return ok;
        const uint32_t delay = cfg_.retry_delay_ms << attempt;
        // Inside the token's lifetime, else the retry would answer bad_token.
        if (token_expires > 0 && now() + (int64_t)(delay / 1000) + (int64_t)cfg_.token_margin_s > token_expires)
            return ok;
        net_.sleep_ms(delay);
    }
}

ReportOutcome Uploader::fail(const std::string& id, ReportState* st, const char* why) {
    if (why && *why) detail_ = detail_.empty() ? std::string(why) : std::string(why) + " (" + detail_ + ")";
    st->attempts += 1;
    int64_t delay = cfg_.backoff_base_s;
    for (int i = 1; i < st->attempts && delay < (1 << 20); ++i) delay *= 2;
    st->next_attempt_unix = now() + delay;
    outbox_.save_state(id, *st);
    ++stats_.failed;
    return ReportOutcome::Failed;
}

ReportOutcome Uploader::gated(const ErrorBody& e, const char* why) {
    int64_t until = now();
    if (e.has_retry_after) until += (int64_t)e.retry_after_s;
    if (e.has_disable_until && (int64_t)e.disable_until_unix > until) until = (int64_t)e.disable_until_unix;
    save_gate(until, api_error_name(e.error));
    detail_ = why;
    stats_.gate_unix = gate_.not_before_unix;
    return ReportOutcome::Gated;
}

bool Uploader::stop_thunk(void* ud) { return static_cast<Uploader*>(ud)->stopped(); }

// One pass over the requested pieces. False stops the report with *outcome.
bool Uploader::upload_pieces(const std::string& id, const ReportRecord& rec, ReportState* st, const Decision& d,
                             std::vector<std::string>* stored, ReportOutcome* outcome) {
    stored->clear();
    for (const ArtifactRequest& want : d.artifacts) {
        const ArtifactFile* file = nullptr;
        for (const ArtifactFile& f : rec.artifacts)
            if (f.name == want.name) file = &f;
        if (!file) {
            *outcome = fail(id, st, "the API asked for a piece this report does not hold");
            return false;
        }
        const std::string path = outbox_.root() + "/" + id + "/" + file->file;
        DirEntry de;
        if (!io_.stat(path, &de)) {
            *outcome = fail(id, st, "a piece of this report is gone");
            return false;
        }
        const uint64_t sealed = sealed_size_for(de.size);
        if (sealed > want.max_bytes) {
            *outcome = fail(id, st, "a piece is larger than max_bytes");
            return false;
        }
        SealedFileBody body;
        body.io = &io_;
        body.path = path;
        body.recipient_pk = keys_.recipient_pk;
        body.random = env_.random_bytes;
        body.random_ud = env_.ud;

        HttpRequest put;
        put.method = "PUT";
        put.path = "/v1/reports/" + id + "/artifacts/" + want.name;
        put.headers = headers_for(rec.build_id);
        put.headers.push_back("Authorization: D2V-Upload " + d.token);
        put.headers.push_back("Content-Type: application/octet-stream");
        put.content_length = sealed;
        put.body = sealed_file_body;
        put.body_ud = &body;

        Answer a;
        call_retrying(Shape::Stored, put, id, want.name, sealed, (int64_t)d.expires_unix, &a);
        if (!a.ok) {
            *outcome = stopped() ? ReportOutcome::Stopped : fail(id, st, "piece not stored");
            return false;
        }
        if (a.is_error) {
            switch (a.error.error) {
                case ApiError::Exists:
                    // The answer to an earlier PUT was lost: the piece is
                    // there (contract README.md, Retries).
                    stored->push_back(want.name);
                    continue;
                case ApiError::RateLimited:
                    *outcome = gated(a.error, "rate limited while uploading a piece");
                    return false;
                case ApiError::NotAccepting:
                    *outcome = gated(a.error, "reports are paused");
                    return false;
                default:
                    *outcome = fail(id, st, api_error_name(a.error.error));
                    return false;
            }
        }
        ++stats_.artifacts_stored;
        stored->push_back(want.name);
    }
    return true;
}

ReportOutcome Uploader::send_report(const std::string& id) {
    detail_.clear();
    if (!client_ready_) {
        if (!http_.set_base_url(cfg_.base_url)) {
            detail_ = "the API address is not http://host[:port][/path]";
            return ReportOutcome::Skipped;
        }
        http_.set_stop(stop_thunk, this);
        client_ready_ = true;
    }
    ReportRecord rec;
    ReportState st;
    if (!outbox_.load(id, &rec, &st)) {
        detail_ = "the report cannot be read";
        return ReportOutcome::Skipped;
    }
    const OutboxLimits& lim = outbox_.limits();
    const int64_t t = now();
    if (st.consent != "granted") {
        detail_ = "consent is " + st.consent;
        return ReportOutcome::Skipped;
    }
    if (st.attempts >= lim.max_attempts) {
        detail_ = "already failed " + decimal(st.attempts) + " times";
        return ReportOutcome::Skipped;
    }
    if (t - st.created_unix > lim.max_age_s) {
        detail_ = "older than the outbox bound";
        return ReportOutcome::Skipped;
    }
    if (t < st.next_attempt_unix) {
        detail_ = "waiting until " + decimal(st.next_attempt_unix);
        return ReportOutcome::Skipped;
    }
    if (!blocked_build_.empty() && rec.build_id == blocked_build_) {
        detail_ = "this build is unknown to the API";
        return ReportOutcome::Skipped;
    }
    if (gate_closed()) {
        detail_ = "the outbox waits until " + decimal(gate_.not_before_unix);
        return ReportOutcome::Gated;
    }
    if (stopped()) return ReportOutcome::Stopped;

    // The id names the report everywhere: in the claim, where claim.v1 fixes
    // its shape, and in the request target of every later call
    // ("/v1/reports/<id>/complete"). A directory name the collector did not
    // write is refused here, before it can reach either.
    const std::string claim =
        rec.report_id == id && valid_report_id(id)
            ? build_claim_json(claim_inputs_from_record(rec, cfg_.install_id, cfg_.channel, cfg_.platform_model))
            : std::string();
    if (claim.empty()) {
        // Nothing will make this report acceptable: stop spending sends on it
        // (the outbox drops it at the next enforce).
        st.attempts = lim.max_attempts;
        outbox_.save_state(id, st);
        ++stats_.failed;
        detail_ = "no claim can be built for this report";
        return ReportOutcome::Failed;
    }

    ++stats_.attempted;
    StringBody claim_body;
    claim_body.text = &claim;
    HttpRequest post;
    post.method = "POST";
    post.path = "/v1/claims";
    post.headers = headers_for(rec.build_id);
    post.headers.push_back("Content-Type: application/json");
    post.content_length = claim.size();
    post.body = string_body;
    post.body_ud = &claim_body;

    Answer a;
    call_retrying(Shape::Decision, post, id, "", 0, 0, &a);
    if (!a.ok) return stopped() ? ReportOutcome::Stopped : fail(id, &st, "no decision");
    if (a.is_error) {
        switch (a.error.error) {
            case ApiError::RateLimited:
                return gated(a.error, "rate limited");
            case ApiError::NotAccepting:
                return gated(a.error, "reports are paused");
            case ApiError::UnknownBuild:
                blocked_build_ = rec.build_id;
                fail(id, &st, "the API does not know this build");
                return ReportOutcome::UnknownBuild;
            default:
                return fail(id, &st, api_error_name(a.error.error));
        }
    }
    const Decision d = a.decision;
    // A decision can also ask for a wait (decision.v1, retry_after_s and
    // disable_until_unix): the pieces of this report still go out.
    if (d.has_retry_after || d.has_disable_until) {
        int64_t until = now();
        if (d.has_retry_after) until += (int64_t)d.retry_after_s;
        if (d.has_disable_until && (int64_t)d.disable_until_unix > until) until = (int64_t)d.disable_until_unix;
        save_gate(until, d.has_disable_until ? "not_accepting" : "rate_limited");
    }
    if (!d.upload) {
        // A verified decision that names this report: it can go (contract
        // README.md, "The console deletes a pending report only after...").
        outbox_.remove(id);
        ++stats_.deleted;
        detail_ = "counted, nothing to upload";
        return ReportOutcome::Deleted;
    }

    std::vector<std::string> stored;
    ReportOutcome outcome = ReportOutcome::Failed;
    if (!upload_pieces(id, rec, &st, d, &stored, &outcome)) return outcome;

    for (int round = 0;; ++round) {
        JsonWriter w;
        w.begin_object().key("v").num(1).key("artifacts").begin_array();
        for (const std::string& n : stored) w.str(n);
        w.end_array().end_object();
        const std::string body = w.out();
        StringBody complete_body;
        complete_body.text = &body;
        HttpRequest done;
        done.method = "POST";
        done.path = "/v1/reports/" + id + "/complete";
        done.headers = headers_for(rec.build_id);
        done.headers.push_back("Authorization: D2V-Upload " + d.token);
        done.headers.push_back("Content-Type: application/json");
        done.content_length = body.size();
        done.body = string_body;
        done.body_ud = &complete_body;

        Answer c;
        call_retrying(Shape::Complete, done, id, "", 0, (int64_t)d.expires_unix, &c);
        if (!c.ok) return stopped() ? ReportOutcome::Stopped : fail(id, &st, "complete got no answer");
        if (c.is_error) {
            switch (c.error.error) {
                case ApiError::Incomplete:
                    // A requested piece is missing: upload again, then
                    // complete again. Once.
                    if (round > 0) return fail(id, &st, "complete still incomplete");
                    if (!upload_pieces(id, rec, &st, d, &stored, &outcome)) return outcome;
                    continue;
                case ApiError::RateLimited:
                    return gated(c.error, "rate limited on complete");
                case ApiError::NotAccepting:
                    return gated(c.error, "reports are paused");
                default:
                    return fail(id, &st, api_error_name(c.error.error));
            }
        }
        outbox_.remove(id);
        ++stats_.completed;
        detail_ = c.complete.sample_stored ? "stored as the sample" : "uploaded, another sample was kept";
        return ReportOutcome::Completed;
    }
}

UploadStats Uploader::run() {
    stats_ = UploadStats();
    detail_.clear();
    network_down_ = false;
    blocked_build_.clear();
    load_gate();
    stats_.gate_unix = gate_.not_before_unix;
    if (gate_closed()) {
        detail_ = "the outbox waits until " + decimal(gate_.not_before_unix) + " (" + gate_.reason + ")";
        stats_.last = ReportOutcome::Gated;
        return stats_;
    }
    const std::vector<std::string> ids = outbox_.list();
    for (const std::string& id : ids) {
        if (stats_.attempted >= cfg_.max_reports_per_run) break;
        if (stopped()) {
            stats_.last = ReportOutcome::Stopped;
            break;
        }
        ++stats_.considered;
        const ReportOutcome o = send_report(id);
        stats_.last = o;
        if (o == ReportOutcome::Skipped) ++stats_.skipped;
        if (o == ReportOutcome::Gated || o == ReportOutcome::Stopped) break;
        // A name that does not resolve or a connection that cannot be opened:
        // the network is down, the other reports would only wait for nothing.
        if (network_down_) break;
    }
    stats_.gate_unix = gate_.not_before_unix;
    return stats_;
}

}  // namespace d2cr
