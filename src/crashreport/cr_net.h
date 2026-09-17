// src/crashreport/cr_net.h — the sockets the reporter needs, behind an
// interface.
//
// src/crashreport/ never includes a VitaSDK header: the console implementation
// (sceNet) and the POSIX one used by the tests (cr_net_posix.cpp) sit
// behind NetApi. The reporter only ever opens one outgoing TCP connection at a
// time and closes it after each request (Connection: close, spec §4.8).
//
// Every call is bounded in time. The upload thread must also stop quickly at
// teardown (spec §4.9, bounded stop), so the HTTP client asks for short slices
// and checks its own deadlines and stop flag between them rather than handing
// a long timeout to the platform.
//
// Name resolution is a separate call so that the address can be resolved once
// per report and reused by the several requests it takes (claim, pieces,
// complete). IPv4 only: the Vita network stack has no AF_INET6.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace d2cr {

// Ok carries bytes, Timeout means "nothing happened yet, ask again", Closed
// means the peer closed the connection, Error means it is broken.
enum class NetResult { Ok = 0, Timeout, Closed, Error };

inline const char* net_result_name(NetResult r) {
  switch (r) {
    case NetResult::Ok: return "ok";
    case NetResult::Timeout: return "timeout";
    case NetResult::Closed: return "closed";
    case NetResult::Error: break;
  }
  return "error";
}

using NetHandle = int;
constexpr NetHandle kNoHandle = -1;

class NetApi {
 public:
  virtual ~NetApi() = default;

  // Milliseconds from an arbitrary origin that never goes backwards.
  virtual uint64_t now_ms() = 0;
  virtual void sleep_ms(uint32_t ms) = 0;

  // Host name or dotted-quad address to an IPv4 address in host byte order.
  virtual bool resolve(const std::string& host, uint32_t* ipv4, uint32_t timeout_ms) = 0;

  // Opens a TCP connection; kNoHandle when it fails or times out.
  virtual NetHandle connect(uint32_t ipv4, uint16_t port, uint32_t timeout_ms) = 0;

  // Sends at least one byte on Ok and sets *sent; Timeout when the peer took
  // nothing within timeout_ms.
  virtual NetResult send(NetHandle h, const uint8_t* data, size_t n, uint32_t timeout_ms, size_t* sent) = 0;

  // Reads at least one byte on Ok and sets *got; Closed at the end of the
  // stream; Timeout when nothing arrived within timeout_ms (0 = a poll).
  virtual NetResult recv(NetHandle h, uint8_t* out, size_t n, uint32_t timeout_ms, size_t* got) = 0;

  virtual void close(NetHandle h) = 0;
};

// POSIX sockets, for the tests on a dev machine. Its resolver ignores the
// timeout (getaddrinfo has none); the console resolver takes one.
std::unique_ptr<NetApi> make_posix_net();

// Raw sceNet sockets and the reporter's own sceNetResolver handle
// (cr_net_vita.cpp). Assumes the sceNet stack is already initialized
// (src/platform/vita_net.h) — never calls sceNetInit itself.
std::unique_ptr<NetApi> make_vita_net();

}  // namespace d2cr
