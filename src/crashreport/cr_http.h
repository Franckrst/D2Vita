// src/crashreport/cr_http.h — the little HTTP/1.1 client the reporter needs.
//
// Plain HTTP, no TLS (spec §4.8: the pieces are sealed, the claim carries
// nothing nominative, and the answers are signed). One request per connection,
// always with `Connection: close` and always with a `Content-Length` — the API
// refuses a body without one, and a sealed object's length is known before it
// is produced (cr_seal.h).
//
// What it does, and nothing else:
//   * request bodies are streamed from a callback, so a 2 MiB dump never sits
//     in memory;
//   * responses come with `Content-Length`, `Transfer-Encoding: chunked`, or
//     end with the connection; the body is kept whole because the Ed25519
//     signature covers its exact bytes, so it is capped (16 KiB by default,
//     the largest console answer is a few hundred bytes);
//   * the head is capped too, every wait has a deadline, and between two
//     platform calls the client checks its deadlines and the stop flag, so the
//     upload thread stops quickly at teardown (spec §4.9);
//   * a server that answers before the body is finished (409, 413, 403 on a
//     PUT) is noticed and the body is dropped instead of being pushed into a
//     closed connection.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "crashreport/cr_net.h"

namespace d2cr {

struct HttpLimits {
  uint32_t resolve_timeout_ms = 10000;
  uint32_t connect_timeout_ms = 10000;
  uint32_t idle_timeout_ms = 15000;     // nothing moved for this long: give up
  uint32_t total_timeout_ms = 120000;   // whole exchange
  uint32_t slice_ms = 200;              // longest single platform call
  size_t max_head_bytes = 8 * 1024;     // status line and headers
  size_t max_body_bytes = 16 * 1024;    // response body
};

enum class HttpError {
  None = 0,
  Cancelled,   // the stop flag was raised
  BadUrl,      // not http://host[:port][/prefix]
  Resolve,
  Connect,
  Send,        // the connection broke while the request was going out
  Recv,        // it broke while the answer was coming in
  Timeout,
  Protocol,    // the answer is not HTTP/1.x the way this client reads it
  TooLarge,    // head or body over the limits
  Body,        // the body callback failed, or wrote the wrong number of bytes
};
const char* http_error_name(HttpError e);

// Receives request body bytes; false stops the request.
using HttpBodySinkFn = bool (*)(const uint8_t* data, size_t n, void* ud);

// Produces exactly content_length bytes through sink(data, n, sink_ud).
// False means a local failure (the file vanished, sealing failed): the request
// is aborted and the connection closed.
using HttpBodyFn = bool (*)(HttpBodySinkFn sink, void* sink_ud, void* ud);

struct HttpRequest {
  const char* method = "GET";
  std::string path;                     // "/v1/claims": "/" then visible ASCII, already encoded
  std::vector<std::string> headers;     // "Name: value", no CR, no LF
  uint64_t content_length = 0;
  HttpBodyFn body = nullptr;            // required when content_length > 0
  void* body_ud = nullptr;
};

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string signature;                // X-D2V-Signature, empty when absent
  bool early = false;                   // answered before the request body was finished
  uint64_t body_bytes_sent = 0;
};

class HttpClient {
 public:
  explicit HttpClient(NetApi& net, const HttpLimits& limits = HttpLimits());

  // http://host[:port][/prefix] — no TLS, no query, no user info.
  bool set_base_url(const std::string& url);
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }
  const std::string& base_path() const { return base_path_; }

  // Checked between platform calls; true aborts with HttpError::Cancelled.
  void set_stop(bool (*stop)(void*), void* ud) { stop_ = stop; stop_ud_ = ud; }

  // One request over a fresh connection. HttpError::None fills *out.
  HttpError request(const HttpRequest& req, HttpResponse* out);

  // Why the last request failed, for the logs. Never holds body bytes.
  const std::string& error_detail() const { return detail_; }

 private:
  struct Conn;
  struct BodyPump;
  // Receives what the body callback produces (HttpBodySinkFn).
  static bool body_sink(const uint8_t* data, size_t n, void* ud);
  HttpError send_all(Conn& c, const char* data, size_t n);
  HttpError read_head(Conn& c, HttpResponse* out, bool* chunked, uint64_t* content_length, bool* has_length);
  HttpError read_body(Conn& c, HttpResponse* out, bool chunked, uint64_t content_length, bool has_length);
  HttpError fill(Conn& c);
  HttpError fail(HttpError e, const char* why);
  bool stopped() const { return stop_ && stop_(stop_ud_); }

  NetApi& net_;
  HttpLimits limits_;
  std::string host_, base_path_, detail_;
  uint16_t port_ = 80;
  bool have_address_ = false;
  uint32_t address_ = 0;
  bool (*stop_)(void*) = nullptr;
  void* stop_ud_ = nullptr;
};

}  // namespace d2cr
