#include "vita_net.h"
#include "runtime/net_nonblock.h"
#include "runtime/net_guard.h"   // wx86_net_private_only() — leaf unit, see that header
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/socket.h>
#ifndef __vita__
#include <sys/ioctl.h>   // not in VitaSDK: the FIONBIO path is host-only
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
// VitaSDK's <netdb.h> declares getnameinfo with `restrict`, a C keyword C++
// doesn't have; without this local alias the file fails to compile for
// console. Undefined again right after so it doesn't leak into the rest.
#ifdef __vita__
#define restrict __restrict__
#endif
#include <netdb.h>
#ifdef __vita__
#undef restrict
#endif
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <chrono>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2common/net.h>
// 0x23114 bytes (~140 KiB): matches the SDK libc's own sizing exactly — the
// literal 0x00023114 that _vita_net_init (lib_a-vitanet.o) loads into
// SceNetInitParam::size. Every byte here comes out of the arena, which claims
// 297 MB of a ~330 MB user budget.
static const int D2VITA_NET_POOL = 0x23114;
static void* g_netPool = nullptr;

// The SDK headers define NO error code for sceNetCtlInit, so it's named
// here. Value taken from disassembling the SDK libc (lib_a-vitanet.o,
// _vita_net_init), which compares sceNetCtlInit's return to 0x80412102 and
// treats equality as success.
// NOT to be confused with SCE_NET_ERROR_EBUSY (0x80410110), sceNetInit's
// "already initialized" code — the same libc treats them as distinct.
static const unsigned D2VITA_NETCTL_ALREADY_INITED = 0x80412102u;

// Common fallback for any failure path AFTER a successful init.
// ORDER IS CRITICAL and matches the SDK libc (lib_a-vitanet.o: sceNetTerm
// THEN free): the Sony stack owns g_netPool until sceNetTerm has been
// called. Freeing it first lets the 297 MB arena allocation that follows
// immediately reclaim it while a live stack still writes there — silent,
// non-reproducible guest memory corruption with nothing in the boot log.
static void net_teardown(bool ctlUp, bool netUp) {
    if (ctlUp) sceNetCtlTerm();
    if (netUp) sceNetTerm();
    if (g_netPool) { std::free(g_netPool); g_netPool = nullptr; }
}
#endif

// MSG_NOSIGNAL doesn't exist in VitaSDK's newlib, and it wouldn't do anything
// there anyway: without SIGPIPE, send just returns an error.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Engine log (winx86 platform/vita_host.h): writes a durable line on
// console, a no-op off console. The host bench (tools/net_check.sh) compiles
// vita_host.cpp together with this unit, which is what makes the direct call
// valid on both targets.
#include "platform/vita_host.h"

// Measurement clock, host and console. Times name resolution: duration alone
// determines the units of the Sony resolver's (undocumented) timeout.
static uint64_t now_ms(void) {
#ifdef __vita__
    return (uint64_t)(sceKernelGetProcessTimeWide() / 1000ull);
#else
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static char g_status[160] = "reseau: non initialise";
static char g_ip[32]      = "0.0.0.0";

const char* d2vita_net_status(void)   { return g_status; }
const char* d2vita_net_local_ip(void) { return g_ip; }

int d2vita_net_init(int wait_ms) {
#ifndef __vita__
    (void)wait_ms;
    std::snprintf(g_status, sizeof g_status, "reseau: hote POSIX, rien a initialiser");
    return 0;
#else
    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (rc < 0) {
        std::snprintf(g_status, sizeof g_status, "reseau: module NET non charge 0x%08X", (unsigned)rc);
        return -1;
    }
    g_netPool = std::malloc(D2VITA_NET_POOL);
    if (!g_netPool) {
        std::snprintf(g_status, sizeof g_status, "reseau: pool de %d o refuse", D2VITA_NET_POOL);
        return -2;
    }
    SceNetInitParam p;
    p.memory = g_netPool; p.size = D2VITA_NET_POOL; p.flags = 0;
    rc = sceNetInit(&p);
    // netUp says who OWNS g_netPool: while true, the block belongs to the
    // Sony stack, and freeing it without sceNetTerm corrupts memory (see
    // net_teardown). This flag, not a return code re-read later, drives
    // every error exit below.
    bool netUp = false, ctlUp = false;
    if (rc >= 0) {
        netUp = true;
    } else if ((unsigned)rc == (unsigned)SCE_NET_ERROR_EBUSY) {
        // EBUSY = the stack was already running (the system brought it up),
        // so it never took our block; nobody owns it, so we free it
        // ourselves. The SDK libc does the same on this code.
        std::free(g_netPool); g_netPool = nullptr;
    } else {
        std::snprintf(g_status, sizeof g_status, "reseau: sceNetInit 0x%08X", (unsigned)rc);
        std::free(g_netPool); g_netPool = nullptr;   // init failed: block was never taken
        return -2;
    }
    rc = sceNetCtlInit();
    if (rc >= 0) {
        ctlUp = true;
    } else if ((unsigned)rc != D2VITA_NETCTL_ALREADY_INITED) {
        std::snprintf(g_status, sizeof g_status, "reseau: sceNetCtlInit 0x%08X", (unsigned)rc);
        net_teardown(false, netUp);
        return -3;
    }
    // Logged BEFORE the wait: it can run the full wait_ms, the screen isn't
    // initialized yet at this point, and a silent multi-second wait is
    // indistinguishable from a hang.
    {
        char m[96];
        std::snprintf(m, sizeof m, "reseau: attente de la connexion (jusqu'a %d ms)", wait_ms);
        wx86_vita_progress_c(m);
    }
    // Starts DISCONNECTED, never CONNECTED: an out-parameter must not start
    // out already equal to the success value, or a call that "succeeds"
    // without writing anything would look like a connection that isn't
    // there.
    int state = SCE_NETCTL_STATE_DISCONNECTED, waited = 0;
    bool sawProgress = false;
    for (;;) {
        int rs = sceNetCtlInetGetState(&state);
        // A failed call is NOT a silent way out of the loop: letting it
        // through would return 0 without ever having seen a connection.
        if (rs < 0) {
            std::snprintf(g_status, sizeof g_status,
                          "reseau: sceNetCtlInetGetState 0x%08X", (unsigned)rs);
            net_teardown(ctlUp, netUp);
            return -4;
        }
        if (state == SCE_NETCTL_STATE_CONNECTED) break;
        if (state != SCE_NETCTL_STATE_DISCONNECTED) sawProgress = true;
        // Early exit: staying DISCONNECTED without ever passing through
        // CONNECTING means Wi-Fi is off. Waiting out the full timeout
        // wouldn't change that, and would just burn it before anything is
        // even shown on screen.
        if (!sawProgress && waited >= 1000) {
            std::snprintf(g_status, sizeof g_status,
                          "reseau: aucune tentative de connexion (wifi coupe ?)");
            net_teardown(ctlUp, netUp);
            return -4;
        }
        if (waited >= wait_ms) {
            std::snprintf(g_status, sizeof g_status, "reseau: pas de connexion (etat %d)", state);
            // MOST COMMON failure path (Wi-Fi off, no association). Boot
            // continues solo: leaking the pool and two now-unusable modules
            // would eat into what little is left after the arena.
            net_teardown(ctlUp, netUp);
            return -4;
        }
        sceKernelDelayThread(200 * 1000); waited += 200;
    }
    // Buffer cleared BEFORE the call: sceNetCtlInetGetInfo can return >= 0
    // without writing anything, and a string left to stack garbage would
    // pass for an address. The IP is informational only, so its absence
    // doesn't block networking — but it shows up as unknown in the status
    // instead of being fabricated.
    SceNetCtlInfo info; std::memset(&info, 0, sizeof info);
    int ri = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    if (ri < 0 || info.ip_address[0] == 0) {
        std::snprintf(g_status, sizeof g_status,
                      "reseau: pret, ip INCONNUE (sceNetCtlInetGetInfo 0x%08X)", (unsigned)ri);
        return 0;
    }
    std::snprintf(g_ip, sizeof g_ip, "%s", info.ip_address);
    // DNS servers, logged right after init. Without them, a resolution
    // failure leaves too many possible causes: no DNS configured, DNS
    // unreachable, or the resolver misused. A fast ECONNREFUSED (rather than
    // a timeout) means something actively refused the request, and knowing
    // these two addresses narrows down what.
    {   SceNetCtlInfo d1; std::memset(&d1, 0, sizeof d1);
        SceNetCtlInfo d2; std::memset(&d2, 0, sizeof d2);
        int r1 = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_PRIMARY_DNS,   &d1);
        int r2 = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SECONDARY_DNS, &d2);
        char m[160];
        std::snprintf(m, sizeof m, "reseau: dns1=%s (rc 0x%08X) dns2=%s (rc 0x%08X)",
                      r1 >= 0 ? d1.primary_dns : "?", (unsigned)r1,
                      r2 >= 0 ? d2.secondary_dns : "?", (unsigned)r2);
        wx86_vita_progress_c(m);
        std::printf("%s\n", m);
    }
    std::snprintf(g_status, sizeof g_status, "reseau: pret, ip %s", g_ip);
    return 0;
#endif
}

// d2vita_net_set_nonblock / d2vita_net_resolve are generic (no D2-specific
// data) and live in winx86 (d2rt::wx86_net_set_nonblock /
// d2rt::wx86_net_resolve, runtime/net_nonblock.h).

int d2vita_net_selftest(const char* host, int port) {
    int fails = 0;
    auto report = [&](const char* name, bool cond, const char* detail) {
        std::printf("[nettest] %-14s %s %s\n", name, cond ? "PASS" : "FAIL", detail ? detail : "");
        std::fflush(stdout);
        // stdout doesn't exist on console: without this redirect to the boot
        // log, a bench line is INVISIBLE on device. A measurement that
        // exists on the host and silently vanishes on target measures
        // nothing.
        {
            char m[128];
            std::snprintf(m, sizeof m, "[nettest] %s %s %s", name, cond ? "PASS" : "FAIL", detail ? detail : "");
            wx86_vita_progress_c(m);
        }
    };
    // A REQUIRED primitive counts toward the verdict.
    auto check = [&](const char* name, bool cond, const char* detail) {
        report(name, cond, detail);
        if (!cond) ++fails;
    };
    // An INFORMATIONAL probe never counts. select is one: rt_boot's select
    // shim is built on ::poll, so a non-working select on Vita breaks
    // nothing — counting it would skew the verdict where it matters most.
    auto probe = [&](const char* name, bool cond, const char* detail) {
        report(name, cond, detail);
    };

    uint32_t ip = d2rt::wx86_net_resolve(host);
    { char d[80]; std::snprintf(d, sizeof d, "%s -> %u.%u.%u.%u", host,
          ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, (ip >> 24) & 255);
      check("resolve", ip != 0, d); }
    if (!ip) { std::printf("[nettest] %d echec(s)\n", fails); return fails; }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    check("socket", fd >= 0, nullptr);
    if (fd < 0) { std::printf("[nettest] %d echec(s)\n", fails); return fails; }

    bool nb = d2rt::wx86_net_set_nonblock(fd, true);
    { char d[48]; std::snprintf(d, sizeof d, "methode %d", d2rt::wx86_net_nonblock_method());
      check("nonblock", nb, d); }
    if (!nb) {
        // Without non-blocking mode, the recv below would hang indefinitely
        // on a socket that never receives anything — no diagnostic instead
        // of a clear failure. Stop the bench rather than risk a silent
        // freeze.
        ::close(fd);
        std::printf("[nettest] %d echec(s)\n", fails);
        return fails;
    }

    sockaddr_in sa; std::memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = ip;
    errno = 0;
    int rc = ::connect(fd, (sockaddr*)&sa, sizeof sa);
    // errno is only reliable after a failed call: reading it after success
    // would return whatever a previous call (fcntl/ioctl) left behind.
    check("connect", rc == 0 || errno == EINPROGRESS || errno == EWOULDBLOCK,
          rc < 0 ? std::strerror(errno) : nullptr);

    // poll is THE primitive rt_boot's select shim depends on (W(18,...)).
    pollfd pw { fd, POLLOUT, 0 };
    int pr = ::poll(&pw, 1, 5000);
    check("poll", pr > 0 && (pw.revents & POLLOUT), nullptr);

    // select isn't used by rt_boot; measured for information only.
    fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
    timeval tv { 5, 0 };
    int sr = ::select(fd + 1, nullptr, &wf, nullptr, &tv);
    probe("select_info", sr > 0 && FD_ISSET(fd, &wf), "informatif");

    // Non-zero sentinel: if getsockopt fails, or "succeeds" without writing
    // the buffer, soerr must stay visibly a failure, not default to 0.
    // SO_ERROR is the only check in this bench that detects a refused
    // connect — it's how the game itself learns whether its non-blocking
    // connect succeeded.
    int soerr = 0x7fffffff; socklen_t sl = sizeof soerr;
    errno = 0;
    int gs = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    check("so_error", gs == 0 && soerr == 0,
          gs != 0 ? std::strerror(errno) : (soerr != 0 ? std::strerror(soerr) : nullptr));

    // Proof that non-blocking mode actually works: the server hasn't
    // received anything yet, so recv must return immediately instead of
    // blocking.
    char b[16];
    errno = 0;
    ssize_t n = ::recv(fd, b, sizeof b, 0);
    check("nonblock_recv", n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
          n < 0 ? std::strerror(errno) : "recv a rendu sans octet, errno non positionne");

    // bncs_local.py replies with SID_PING (ff 25 ..) upon receiving selector 0x01.
    const unsigned char sel = 0x01;
    errno = 0;
    ssize_t sret = ::send(fd, &sel, 1, MSG_NOSIGNAL);
    check("send", sret == 1, sret < 0 ? std::strerror(errno) : nullptr);

    // Time-bounded read, NOT a single recv: a short read is legal in TCP and
    // could cut the message short of the 4 expected bytes, causing an
    // intermittent false failure.
    int got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (got < 4) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) break;
        pollfd pi { fd, POLLIN, 0 };
        int pr2 = ::poll(&pi, 1, (int)remain);
        if (pr2 <= 0) break;
        ssize_t r = ::recv(fd, b + got, sizeof(b) - got, 0);
        if (r <= 0) break;
        got += (int)r;
    }
    { char d[64];
      if (got > 0) std::snprintf(d, sizeof d, "%d octets, %02x %02x", got,
                     (unsigned char)b[0], got > 1 ? (unsigned char)b[1] : 0);
      else std::snprintf(d, sizeof d, "aucune reponse");
      check("bncs_ping", got >= 4 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0x25, d); }

    ::close(fd);

    // DECISIVE probe: a literal IP resolves via inet_aton without touching
    // the resolver, so this is the only check that exercises a real NAME
    // resolution.
    //
    // Requires an explicit name: without D2NETTEST_NAME, this probe resolves
    // nothing and says so — a diagnostic bench has no business picking its
    // own public host to name. The network exit lock (wx86_net_private_only)
    // also covers name resolution itself, not just connect/sendto/recvfrom,
    // so this probe goes silent while the lock is armed — intentional, since
    // the resolution request itself would leak the requested name.
    //
    // Purpose: time a NAME resolution (vs. a literal, which never touches
    // the resolver) to determine the unit of the SDK's undocumented timeout
    // parameter. A near-instant failure means the value is interpreted in
    // microseconds (far too short); a failure only after the full requested
    // delay means seconds — and that the real problem is elsewhere (e.g. DNS
    // not configured).
    {   const char* rn = getenv("D2NETTEST_NAME");
        if (!rn || !*rn) {
            probe("resolve_name", true,
                  "non effectuee : poser D2NETTEST_NAME=<hote> pour la demander");
        } else if (::wx86_net_private_only()) {
            probe("resolve_name", true,
                  "non effectuee : le verrou de sortie refuse les NOMS (le lever pour mesurer)");
        } else {
            uint64_t t0 = now_ms();
            uint32_t rip = d2rt::wx86_net_resolve(rn);
            uint64_t dt = now_ms() - t0;
            char d[112];
            std::snprintf(d, sizeof d, "%s -> %u.%u.%u.%u en %llu ms (rc 0x%08X)", rn,
                          rip & 255, (rip >> 8) & 255, (rip >> 16) & 255, (rip >> 24) & 255,
                          (unsigned long long)dt, (unsigned)d2rt::wx86_net_last_resolve_rc());
            check("resolve_name", rip != 0, d);
        } }

    std::printf("[nettest] %d echec(s)\n", fails);
    return fails;
}

#ifdef D2VITA_NET_MAIN
int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? std::atoi(argv[2]) : 6112;
    if (d2vita_net_init(5000) != 0) { std::printf("%s\n", d2vita_net_status()); return 2; }
    std::printf("%s\n", d2vita_net_status());
    return d2vita_net_selftest(host, port) == 0 ? 0 : 1;
}
#endif
