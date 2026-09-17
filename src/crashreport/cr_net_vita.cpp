// src/crashreport/cr_net_vita.cpp — Vita NetApi, raw sceNet* sockets and the
// reporter's OWN sceNetResolver.
//
// Deliberately NOT the game's Winsock/POSIX socket path: the reporter's
// traffic is host-side maintenance traffic, unrelated to the guest's BNCS
// networking, so it opens its own socket directly on the already-initialized
// sceNet stack (see src/platform/vita_net.h/.cpp: d2vita_net_init() brings
// that stack up before the arena claim, for the game's own use; this file
// never calls sceNetInit — see cr_boot.cpp for how the stack gets started
// when the game itself did not ask for D2NET) and resolves names with its
// own sceNetResolver handle rather than d2rt::wx86_net_resolve (that one is
// the GUEST-facing resolver of the shared winx86 engine — third_party/winx86
// — and must stay untouched by this port-specific file).
//
// Mirrors the contract cr_net_posix.cpp documents at the top of that file:
//   resolve   host name or dotted quad -> IPv4 in HOST byte order;
//   connect   non-blocking connect with its own timeout, kNoHandle on
//             refusal, timeout or any error;
//   send      Ok with *sent >= 1, Timeout when the peer window stayed full,
//             Error when the connection broke;
//   recv      Ok with *got >= 1, Closed at end of stream, Timeout when
//             nothing arrived (timeout 0 = a poll);
//   close     closes the handle, never reused afterward.
//
// Every wait is bounded by a fresh, one-shot sceNetEpoll set (created,
// armed, waited on and destroyed for that one wait): the reporter opens at
// most one connection at a time and this keeps no epoll state alive between
// calls, so a bug here cannot leak a registration into the next report's
// connection.
#include "crashreport/cr_net.h"

#include <cstring>
#include <memory>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2common/net.h>

namespace d2cr {

namespace {

// sceNet*() timeouts are microseconds (confirmed by this project's existing
// guest resolver, third_party/winx86/src/runtime/net_nonblock.cpp:111 —
// sceNetResolverStartNtoa's 5-second wait is passed as 5*1000*1000); the same
// convention is assumed here for sceNetEpollWait, which carries no doc
// comment of its own in the VitaSDK headers — this assumption is
// unconfirmed on real hardware.
inline int to_usec(uint32_t ms) {
    // Clamped: an oversized ms*1000 would overflow the signed 32-bit
    // microsecond parameter and could wrap to a SMALL or negative value —
    // silently turning a long timeout into a near-instant one (or "wait
    // forever" on some platforms' negative-timeout convention). The
    // reporter's own timeouts never approach this bound (HttpLimits caps at
    // 200 ms slices, 120 s total), so clamping only guards against a future
    // misuse, not a case exercised today.
    const uint64_t us = (uint64_t)ms * 1000ull;
    return us > 0x7fffffffull ? 0x7fffffff : (int)us;
}

// One-shot wait: true if `events` (or anything else, including an error)
// became ready before timeout_ms elapses; false on timeout or a local
// failure to even arm the wait. The caller's own next send/recv/getsockopt
// is what actually discovers success vs. failure — this only says "stop
// waiting now".
bool wait_ready(int fd, unsigned events, uint32_t timeout_ms) {
    int eid = sceNetEpollCreate("d2cr_wait", 0);
    if (eid < 0) return false;
    SceNetEpollEvent ev; std::memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.fd = fd;
    bool armed = sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, fd, &ev) >= 0;
    bool ready = false;
    if (armed) {
        SceNetEpollEvent out[1];
        int n = sceNetEpollWait(eid, out, 1, to_usec(timeout_ms));
        ready = n > 0;
    }
    sceNetEpollDestroy(eid);
    return ready;
}

class VitaNet final : public NetApi {
 public:
    uint64_t now_ms() override {
        return (uint64_t)(sceKernelGetProcessTimeWide() / 1000ull);
    }

    void sleep_ms(uint32_t ms) override {
        sceKernelDelayThread(ms * 1000u);
    }

    bool resolve(const std::string& host, uint32_t* ipv4, uint32_t timeout_ms) override {
        if (host.empty() || !ipv4) return false;
        SceNetInAddr direct; std::memset(&direct, 0, sizeof direct);
        if (sceNetInetPton(SCE_NET_AF_INET, host.c_str(), &direct) == 1) {
            *ipv4 = sceNetNtohl(direct.s_addr);
            return true;
        }
        int rid = sceNetResolverCreate("d2cr_resolver", nullptr, 0);
        if (rid < 0) return false;
        SceNetInAddr addr; std::memset(&addr, 0, sizeof addr);
        // retry=1: one attempt, our own caller (cr_http.cpp) already retries
        // whole requests; a resolver-internal retry would just spend the same
        // timeout budget twice without the caller's knowledge.
        int rc = sceNetResolverStartNtoa(rid, host.c_str(), &addr, to_usec(timeout_ms), 1, 0);
        sceNetResolverDestroy(rid);
        if (rc < 0) return false;
        *ipv4 = sceNetNtohl(addr.s_addr);
        return true;
    }

    NetHandle connect(uint32_t ipv4, uint16_t port, uint32_t timeout_ms) override {
        int fd = sceNetSocket("d2cr_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
        if (fd < 0) return kNoHandle;
        int one = 1;
        sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
        sceNetSetsockopt(fd, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof one);
        SceNetSockaddrIn sa; std::memset(&sa, 0, sizeof sa);
        sa.sin_len = sizeof sa;
        sa.sin_family = SCE_NET_AF_INET;
        sa.sin_port = sceNetHtons(port);
        sa.sin_addr.s_addr = sceNetHtonl(ipv4);
        int rc = sceNetConnect(fd, (SceNetSockaddr*)&sa, sizeof sa);
        if (rc < 0 && (unsigned)rc != (unsigned)SCE_NET_ERROR_EINPROGRESS) {
            sceNetSocketClose(fd);
            return kNoHandle;
        }
        if (rc < 0) {
            if (!wait_ready(fd, SCE_NET_EPOLLOUT, timeout_ms)) { sceNetSocketClose(fd); return kNoHandle; }
            int err = 0; unsigned int elen = sizeof err;
            if (sceNetGetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &err, &elen) < 0 || err != 0) {
                sceNetSocketClose(fd);
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
            int w = sceNetSend(h, data, (unsigned int)n, 0);
            if (w > 0) { if (sent) *sent = (size_t)w; return NetResult::Ok; }
            if (w == 0) return NetResult::Error;
            if ((unsigned)w != (unsigned)SCE_NET_ERROR_EAGAIN) return NetResult::Error;
            const uint64_t now = now_ms();
            if (now >= deadline) return NetResult::Timeout;
            if (!wait_ready(h, SCE_NET_EPOLLOUT, (uint32_t)(deadline - now))) return NetResult::Timeout;
        }
    }

    NetResult recv(NetHandle h, uint8_t* out, size_t n, uint32_t timeout_ms, size_t* got) override {
        if (h < 0 || !out || n == 0) return NetResult::Error;
        if (got) *got = 0;
        const uint64_t deadline = now_ms() + timeout_ms;
        for (;;) {
            int r = sceNetRecv(h, out, (unsigned int)n, 0);
            if (r > 0) { if (got) *got = (size_t)r; return NetResult::Ok; }
            if (r == 0) return NetResult::Closed;
            if ((unsigned)r != (unsigned)SCE_NET_ERROR_EAGAIN) return NetResult::Error;
            const uint64_t now = now_ms();
            if (now >= deadline) return NetResult::Timeout;
            if (!wait_ready(h, SCE_NET_EPOLLIN, (uint32_t)(deadline - now))) return NetResult::Timeout;
        }
    }

    void close(NetHandle h) override {
        if (h >= 0) sceNetSocketClose(h);
    }
};

}  // namespace

std::unique_ptr<NetApi> make_vita_net() { return std::unique_ptr<NetApi>(new VitaNet()); }

}  // namespace d2cr
