// src/crashreport/cr_net_posix.cpp — POSIX NetApi, used by the PC tests.
//
// This file is also the reference for the contract the Vita implementation
// (sceNet) must mirror:
//   resolve   host name or dotted quad -> IPv4 in host byte order;
//   connect   non-blocking connect with its own timeout, kNoHandle on refusal,
//             on timeout and on any error;
//   send      Ok with *sent >= 1, Timeout when the peer window stayed full,
//             Error when the connection broke (never a signal: MSG_NOSIGNAL);
//   recv      Ok with *got >= 1, Closed at the end of the stream, Timeout when
//             nothing arrived (timeout 0 polls without waiting);
//   close     closes the handle, which is never reused afterwards.
#include "crashreport/cr_net.h"

#include <cerrno>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace d2cr {

namespace {

class PosixNet final : public NetApi {
 public:
    uint64_t now_ms() override {
        struct timespec ts;
        if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    }

    void sleep_ms(uint32_t ms) override {
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }

    bool resolve(const std::string& host, uint32_t* ipv4, uint32_t) override {
        if (host.empty() || !ipv4) return false;
        struct in_addr direct;
        if (::inet_pton(AF_INET, host.c_str(), &direct) == 1) {
            *ipv4 = ntohl(direct.s_addr);
            return true;
        }
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;                 // the console stack is IPv4 only
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        bool ok = false;
        for (struct addrinfo* a = res; a && !ok; a = a->ai_next) {
            if (a->ai_family != AF_INET || !a->ai_addr) continue;
            struct sockaddr_in sa;
            std::memcpy(&sa, a->ai_addr, sizeof sa);
            *ipv4 = ntohl(sa.sin_addr.s_addr);
            ok = true;
        }
        ::freeaddrinfo(res);
        return ok;
    }

    NetHandle connect(uint32_t ipv4, uint16_t port, uint32_t timeout_ms) override {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return kNoHandle;
        set_nonblocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(ipv4);
        int rc = ::connect(fd, (struct sockaddr*)&sa, sizeof sa);
        if (rc != 0 && errno != EINPROGRESS && errno != EINTR) { ::close(fd); return kNoHandle; }
        if (rc != 0) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            const int ready = poll_once(&p, timeout_ms);
            int err = 0;
            socklen_t len = sizeof err;
            if (ready <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                ::close(fd);
                return kNoHandle;
            }
        }
        return fd;
    }

    NetResult send(NetHandle h, const uint8_t* data, size_t n, uint32_t timeout_ms, size_t* sent) override {
        if (h < 0 || (!data && n)) return NetResult::Error;
        if (sent) *sent = 0;
        if (n == 0) return NetResult::Ok;
        const uint64_t deadline = now_ms() + timeout_ms;
        for (;;) {
            const ssize_t w = ::send(h, data, n, MSG_NOSIGNAL);
            if (w > 0) {
                if (sent) *sent = (size_t)w;
                return NetResult::Ok;
            }
            if (w == 0) return NetResult::Error;
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return NetResult::Error;
            const uint64_t now = now_ms();
            if (now >= deadline) return NetResult::Timeout;
            struct pollfd p;
            p.fd = h;
            p.events = POLLOUT;
            p.revents = 0;
            const int ready = poll_once(&p, (uint32_t)(deadline - now));
            if (ready < 0) return NetResult::Error;
            if (ready == 0) return NetResult::Timeout;
            if (p.revents & (POLLERR | POLLNVAL)) return NetResult::Error;
        }
    }

    NetResult recv(NetHandle h, uint8_t* out, size_t n, uint32_t timeout_ms, size_t* got) override {
        if (h < 0 || !out || n == 0) return NetResult::Error;
        if (got) *got = 0;
        const uint64_t deadline = now_ms() + timeout_ms;
        for (;;) {
            const ssize_t r = ::recv(h, out, n, 0);
            if (r > 0) {
                if (got) *got = (size_t)r;
                return NetResult::Ok;
            }
            if (r == 0) return NetResult::Closed;
            if (errno == EINTR) continue;
            if (errno == ECONNRESET) return NetResult::Error;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return NetResult::Error;
            const uint64_t now = now_ms();
            if (now >= deadline) return NetResult::Timeout;
            struct pollfd p;
            p.fd = h;
            p.events = POLLIN;
            p.revents = 0;
            const int ready = poll_once(&p, (uint32_t)(deadline - now));
            if (ready < 0) return NetResult::Error;
            if (ready == 0) return NetResult::Timeout;
            if (p.revents & (POLLERR | POLLNVAL)) return NetResult::Error;
        }
    }

    void close(NetHandle h) override {
        if (h >= 0) ::close(h);
    }

 private:
    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static int poll_once(struct pollfd* p, uint32_t timeout_ms) {
        for (;;) {
            const int rc = ::poll(p, 1, (int)(timeout_ms > 0x7fffffffu ? 0x7fffffff : timeout_ms));
            if (rc < 0 && errno == EINTR) continue;
            return rc;
        }
    }
};

}  // namespace

std::unique_ptr<NetApi> make_posix_net() { return std::unique_ptr<NetApi>(new PosixNet()); }

}  // namespace d2cr
