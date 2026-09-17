// kernel32_time.cpp -- see kernel32_time.h.
#include "kernel32_time.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/frame_profile.h"        // lw_count/g_lwTickN
#include "runtime/win32_shims_kernel32.h" // wx86_set_perf_frequency/wx86_perf_frequency
#include "platform/vita_present.h"        // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

// UTC civil conversion without libc: newlib's localtime_r/gmtime_r misbehave on
// scheduler fibers (per-thread reentrancy structs), returning the epoch instead
// of the real date. Hinnant civil_from_days, UTC only (no TZ/DST), matching the
// zeroed TIME_ZONE_INFORMATION exposed to the guest.
static void civil_from_unix(long long t, struct tm* o){
    long long days=t/86400; long long rem=t%86400; if(rem<0){rem+=86400;days--;}
    long long z=days+719468;
    long long era=(z>=0?z:z-146096)/146097;
    unsigned doe=(unsigned)(z-era*146097);
    unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    long long y=(long long)yoe+era*400;
    unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    unsigned mp=(5*doy+2)/153;
    unsigned d=doy-(153*mp+2)/5+1;
    unsigned m=mp+(mp<10?3:-9);
    y += (m<=2);
    std::memset(o,0,sizeof *o);
    o->tm_year=(int)(y-1900); o->tm_mon=(int)m-1; o->tm_mday=(int)d;
    o->tm_hour=(int)(rem/3600); o->tm_min=(int)((rem%3600)/60); o->tm_sec=(int)(rem%60);
    o->tm_wday=(int)(((days+4)%7+7)%7);
}
// Timezone bias in minutes (Windows sense: UTC = localtime + Bias). D2 derives
// SID_AUTH_INFO's timezone field from GetSystemTime−GetLocalTime, so both those
// and GetTimeZoneInformation must agree. From the real system (see detect_tzbias);
// D2_TZBIAS overrides. UTC=0 was a fingerprint divergence.
static int32_t g_tzbias = [](){ const char* e=getenv("D2_TZBIAS");
    return e?(int32_t)std::strtol(e,nullptr,10):detect_tzbias(); }();

void kernel32_time_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // Wall clock source: newlib time() is 0 (epoch) under Vita3K — use the
    // SceRtc-backed helper there; host time() elsewhere.
    // Real wall-clock time family, kept mutually CONSISTENT. The old fixed
    // values (and GetSystemTimeAsFileTime=0 = year 1601) violated the Windows
    // contract: 1.14d's LogManager timestamps via the STATIC CRT's
    // _localtime64_s, and a pre-1970 FILETIME makes the secure CRT call
    // _invalid_parameter -> abort -> TerminateProcess(0xC000000D) the first
    // time the game logs a line (hit on Vita3K during name entry).
    static auto days_from_civil=[](int y,unsigned m,unsigned d)->int64_t{
        y -= m <= 2; int64_t era = (y >= 0 ? y : y-399) / 400;
        unsigned yoe = (unsigned)(y - era*400);
        unsigned doy = (153*(m + (m>2 ? -3 : 9)) + 2)/5 + d-1;
        unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
        return era*146097 + (int64_t)doe - 719468; };
    static auto write_systemtime=[](Cpu&c,uint32_t p,struct tm&tmv,uint16_t ms){
        uint16_t st[8]={(uint16_t)(tmv.tm_year+1900),(uint16_t)(tmv.tm_mon+1),
                        (uint16_t)tmv.tm_wday,(uint16_t)tmv.tm_mday,
                        (uint16_t)tmv.tm_hour,(uint16_t)tmv.tm_min,
                        (uint16_t)tmv.tm_sec,ms};
        c.write(p,st,16); };
    K("SystemTimeToFileTime",2,[](Cpu&c){ uint32_t st=c.arg(0),ft=c.arg(1);
        if(!st||!ft) return 0u;
        uint16_t v[8]; c.read(st,v,16);
        int64_t days=days_from_civil(v[0],v[1],v[3]);
        uint64_t f=((uint64_t)(days*86400ll+(int64_t)v[4]*3600+v[5]*60+v[6])
                    +11644473600ull)*10000000ull+(uint64_t)v[7]*10000ull;
        c.write_u32(ft,(uint32_t)f); c.write_u32(ft+4,(uint32_t)(f>>32)); return 1u; });
    K("FileTimeToSystemTime",2,[](Cpu&c){ uint32_t ft=c.arg(0),st=c.arg(1);
        if(!st) return 0u;
        uint64_t f=ft?(((uint64_t)c.read_u32(ft+4)<<32)|c.read_u32(ft)):0;
        int64_t sec=(int64_t)(f/10000000ull)-11644473600ll; if(sec<0) sec=0;
        struct tm tmv; civil_from_unix(sec,&tmv);
        write_systemtime(c,st,tmv,(uint16_t)((f/10000ull)%1000)); return 1u; });
    K("FileTimeToLocalFileTime",2,[](Cpu&c){ if(c.arg(1)&&c.arg(0)){ uint32_t lo=c.read_u32(c.arg(0)),hi=c.read_u32(c.arg(0)+4);
        c.write_u32(c.arg(1),lo); c.write_u32(c.arg(1)+4,hi);} return 1u; });
    // WINMM: D2's game/network loops pace themselves on timeGetTime — a
    // constant 0 froze the connection state machine (client spun forever).
    { Shim s; s.argc=0; s.stdcall_cleanup=true; s.tag="WINMM.dll!timeGetTime";
      s.fn=[](Cpu&c)->uint32_t{ lw_count(g_lwTickN); return tick_bump(c,1); };
      br.register_shim("WINMM.dll","timeGetTime",s); }
    { Shim s; s.argc=1; s.stdcall_cleanup=true; s.tag="WINMM.dll!timeBeginPeriod";
      s.fn=[](Cpu&)->uint32_t{ return 0u; }; br.register_shim("WINMM.dll","timeBeginPeriod",s); }
    { Shim s; s.argc=1; s.stdcall_cleanup=true; s.tag="WINMM.dll!timeEndPeriod";
      s.fn=[](Cpu&)->uint32_t{ return 0u; }; br.register_shim("WINMM.dll","timeEndPeriod",s); }
    // Frequency of a modern Windows 10 machine (invariant TSC, 100 ns), instead
    // of exactly 1,000,000 which no real PC reports. The COUNTER follows the
    // same unit (wx86_perf_frequency): the duration measured by the guest is
    // strictly unchanged, only the granularity is.
    wx86_set_perf_frequency(10000000ull);
    K("QueryPerformanceCounter",1,[](Cpu&c){ uint32_t p=c.arg(0);
        const uint64_t mul=wx86_perf_frequency()/1000000ull;   // 10 : microsecondes -> 100 ns
        if(g_realclock&&!g_tickvirt){ uint64_t v=((uint64_t)g_tickOfs*1000ull+rt_now_us())*mul;
            c.write_u32(p,(uint32_t)v); c.write_u32(p+4,(uint32_t)(v>>32)); return 1u; }
        uint64_t v=(uint64_t)tick_bump(c,1000)*mul;
        c.write_u32(p,(uint32_t)v); c.write_u32(p+4,(uint32_t)(v>>32)); return 1u; });
    K("GetSystemTimeAsFileTime",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        uint64_t f=((uint64_t)rt_wall()+11644473600ull)*10000000ull;
        static bool once=false; if(!once){ once=true; char m[80];
            std::snprintf(m,sizeof m,"time: GSTAFT wall=%lld",(long long)rt_wall()); d2vita_progress(m); }
        c.write_u32(p,(uint32_t)f); c.write_u32(p+4,(uint32_t)(f>>32)); return 0u; });
    K("GetLocalTime",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        struct tm tmv; civil_from_unix((long long)rt_wall()-(long long)g_tzbias*60,&tmv);   // local = UTC - Bias
        static bool once=false; if(!once){ once=true; char m[80];
            std::snprintf(m,sizeof m,"time: GetLocalTime y=%d m=%d d=%d",tmv.tm_year+1900,tmv.tm_mon+1,tmv.tm_mday); d2vita_progress(m); }
        write_systemtime(c,p,tmv,0); return 0u; });
    K("GetSystemTime",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u;
        struct tm tmv; civil_from_unix((long long)rt_wall(),&tmv);
        static bool once=false; if(!once){ once=true; char m[80];
            std::snprintf(m,sizeof m,"time: GetSystemTime y=%d m=%d d=%d",tmv.tm_year+1900,tmv.tm_mon+1,tmv.tm_mday); d2vita_progress(m); }
        write_systemtime(c,p,tmv,0); return 0u; });
    K("GetTimeZoneInformation",1,[](Cpu&c){
        // The 172-byte TIME_ZONE_INFORMATION must be FILLED (leaving it made the
        // CRT read stack garbage as the bias). Bias=0 (UTC) was also a fingerprint
        // divergence for SID_AUTH_INFO — a real client reports its real offset.
        // Report the HOST's actual UTC offset (minutes; Bias = -gmtoff/60), or
        // D2_TZBIAS to override; default -60 (CET) matches the frFR locale.
        if(c.arg(0)){ std::vector<uint8_t> z(172,0);
            std::memcpy(z.data(),&g_tzbias,4);   // .Bias (LONG minutes) — same source as GetLocalTime
            c.write(c.arg(0),z.data(),172); }
        return 1u; });   // TIME_ZONE_ID_STANDARD (bias present)
}
