// kernel32_fsinfo.cpp -- see kernel32_fsinfo.h.
#include "kernel32_fsinfo.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
using namespace d2rt;

void kernel32_fsinfo_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // spc*bps*freeclusters is computed by D2 in 32-bit: 8*512*0x100000 = 4GB
    // overflowed to 0 -> "Espace disque insuffisant". Report ~1GB (positive
    // even as int32): 8*512*0x3FFFF = 0x3FFFC000.
    K("GetDiskFreeSpaceA",5,[](Cpu&c){ if(c.arg(1))c.write_u32(c.arg(1),8); if(c.arg(2))c.write_u32(c.arg(2),512);
        if(c.arg(3))c.write_u32(c.arg(3),0x3FFFF); if(c.arg(4))c.write_u32(c.arg(4),0x7FFFF); return 1u; });
    // D2's char-creation disk check uses the Ex variant (ULARGE_INTEGER out):
    // unshimmed it reported 0 bytes free -> "Espace disque insuffisant".
    K("GetDiskFreeSpaceExA",4,[](Cpu&c){ auto put64=[&](uint32_t p,uint64_t v){ if(p){ c.write_u32(p,(uint32_t)v); c.write_u32(p+4,(uint32_t)(v>>32)); } };
        put64(c.arg(1),2ull<<30); put64(c.arg(2),8ull<<30); put64(c.arg(3),2ull<<30); return 1u; });
    K("GetFullPathNameA",4,[](Cpu&c){ std::string n=gread_mb(c,c.arg(0),-1); uint32_t buf=c.arg(2),part=c.arg(3);
        std::string full="C:\\"+n; c.write(buf,full.c_str(),(uint32_t)full.size()+1);
        if(part){ size_t s=full.find_last_of("\\/"); c.write_u32(part, buf+(uint32_t)(s==std::string::npos?0:s+1)); }
        return (uint32_t)full.size(); });
    // ⚠ PRE-EXISTING BUG, KEPT AS-IS. The indices are off by one slot from
    // Win32 (lpVolumeNameBuffer is arg1, not arg2): the last write lands on
    // arg(7) = nFileSystemNameSize, which is a SIZE (0x104 = MAX_PATH), so at
    // guest address 0x104. This never caused damage because guest page 0 is
    // mapped (the main thread's TIB lives there): the write is absorbed. As
    // soon as the TIB moves up (D2LAYOUT=haut), page 0 no longer exists and
    // the write FAULTS — which is how the bug was found. The indices are NOT
    // fixed: the values written (0x12345678 into lpFileSystemFlags, 255 into
    // lpFileSystemNameBuffer) are the ones the game has validated against.
    // The write is kept ONLY if the target is mapped — true under the old
    // layout, so behavior stays byte-identical there, and false only where
    // the old code was writing into thin air.
    K("GetVolumeInformationA",8,[](Cpu&c){ uint32_t vn=c.arg(2),vs=c.arg(3);
        auto wr=[&c](uint32_t p,uint32_t v){ if(p && c.mapped(p)) c.write_u32(p,v); };
        if(vn&&vs&&c.mapped(vn)){ const char* lbl="DIABLO2"; uint32_t n=(uint32_t)std::strlen(lbl)+1; if(n>vs)n=vs; c.write(vn,lbl,n); }
        wr(c.arg(5),0x12345678);   // serial
        wr(c.arg(6),255);          // max component len
        wr(c.arg(7),0);            // fs flags
        return 1u; });
    K("GetLogicalDriveStringsA",2,[](Cpu&c){ uint32_t sz=c.arg(0),buf=c.arg(1);
        const char d[]="C:\\\0"; uint32_t need=5;  // "C:\"+NUL + final NUL
        if(buf && sz>=need) c.write(buf,d,need);
        return need-1; });
    K("GetLogicalDrives",0,[](Cpu&){ return 0x4u; });   // C:
    K("GetCurrentDirectoryA",2,[](Cpu&c){ uint32_t buf=c.arg(1); const char* d="C:\\Diablo II"; c.write(buf,d,(uint32_t)std::strlen(d)+1); return (uint32_t)std::strlen(d); });
    K("GetComputerNameA",2,[](Cpu&c){ uint32_t b=c.arg(0),ps=c.arg(1); const char* n="VITA"; c.write(b,n,5); if(ps) c.write_u32(ps,4); return 1u; });
    K("GetTempPathW",2,[](Cpu&c){ uint32_t b=c.arg(1); const char16_t* d=u"C:\\Temp\\";
        uint32_t n=0; while(d[n]) n++; if(b) c.write(b,d,(n+1)*2); return n; });
}
