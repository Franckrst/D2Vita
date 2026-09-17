// kernel32_filemapping.cpp -- see kernel32_filemapping.h.
#include "kernel32_filemapping.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_files.h"        // WxFMap, wx86_file_maps(), wx86_fmap_next_id()
#include "runtime/guest_region.h"       // full wx86::GuestRegion (alloc/free/size_of)
#include "runtime/win32_shims_memory.h" // wx86_gzero
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
using namespace d2rt;

using FMap = WxFMap;   // engine (guest_files.h); same alias as tools/rt_boot.cpp's own
// Separate reference to the SAME underlying engine table as tools/rt_boot.cpp's
// own g_fmaps (still needed there too, e.g. CloseHandle in kernel32_files.cpp) --
// wx86_file_maps() always returns the one true table.
static std::map<uint32_t,FMap>& g_fmaps = wx86_file_maps();

// ALIAS: the implementation lives in the engine (win32_shims_memory.h,
// wx86_gzero). It's the Win32 guarantee behind HEAP_ZERO_MEMORY /
// LMEM_ZEROINIT / MEM_COMMIT, and the other port carried the same copy.
static void gzero(Cpu& c, uint32_t va, uint32_t n){ wx86_gzero(c,va,n); }

void kernel32_filemapping_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // Real file mapping for the CLASSIC in-Game.exe checkrevision (which
    // memory-maps the game executables to hash them). NOT the lockdown DLL:
    // it imports none of these functions. CreateFileMapping(hFile,...) records the
    // source path; MapViewOfFile reads the file into a guest buffer and returns
    // its address. hFile==INVALID_HANDLE_VALUE (0xFFFFFFFF) = pagefile-backed
    // (anonymous) mapping -> zeroed buffer of the requested size.
    K("CreateFileMappingA",6,[](Cpu&c){ uint32_t hFile=c.arg(0);
        uint32_t szLo=c.arg(4);
        FMap m; m.view=0;
        if(hFile!=0xFFFFFFFFu){ auto it=g_filePathByH.find(hFile);
            if(it!=g_filePathByH.end()){ m.path=it->second;
                struct stat st; m.size=(stat(m.path.c_str(),&st)==0)?(uint32_t)st.st_size:szLo; }
            else { set_lasterr(c,6); return 0u; } }        // bad handle
        else { m.size=szLo; }                              // anonymous
        uint32_t h=wx86_fmap_next_id(); g_fmaps[h]=m; set_lasterr(c,0);
        if(env_filelog()) std::fprintf(stderr,"    [CreateFileMapping] h=0x%08x path=%s size=%u\n",h,m.path.c_str(),m.size);
        return h; });
    K("CreateFileMappingW",6,[](Cpu&c){ uint32_t hFile=c.arg(0),szLo=c.arg(4);
        FMap m; m.view=0; if(hFile!=0xFFFFFFFFu){ auto it=g_filePathByH.find(hFile);
            if(it!=g_filePathByH.end()){ m.path=it->second; struct stat st;
                m.size=(stat(m.path.c_str(),&st)==0)?(uint32_t)st.st_size:szLo; } else { set_lasterr(c,6); return 0u; } }
        else m.size=szLo;
        uint32_t h=wx86_fmap_next_id(); g_fmaps[h]=m; set_lasterr(c,0); return h; });
    K("MapViewOfFile",5,[](Cpu&c)->uint32_t{ uint32_t h=c.arg(0);
        auto it=g_fmaps.find(h); if(it==g_fmaps.end()){ set_lasterr(c,6); return 0u; }
        FMap& m=it->second;
        uint64_t off=((uint64_t)c.arg(2)<<32)|c.arg(3); uint32_t want=c.arg(4);
        uint32_t sz = want ? want : (m.size>(uint32_t)off ? m.size-(uint32_t)off : 0);
        if(!sz) sz=0x1000;
        uint32_t buf=d2_valloc(sz); if(!buf){ set_lasterr(c,8); return 0u; }
        ++g_fmapN; g_fmapB+=sz;
        if(!m.path.empty()){
            FILE* fp=std::fopen(m.path.c_str(),"rb");           // g_filePathByH stores the resolved host path
            if(fp){ if(off) std::fseek(fp,(long)off,SEEK_SET);
                std::vector<uint8_t> b(sz,0); size_t got=std::fread(b.data(),1,sz,fp);
                std::fclose(fp); c.write(buf,b.data(),sz);
                if(env_filelog()) std::fprintf(stderr,"    [MapViewOfFile] h=0x%08x -> 0x%08x sz=%u read=%zu\n",h,buf,sz,got); }
            else gzero(c,buf,sz);
        } else gzero(c,buf,sz);
        m.view=buf; return buf; });
    // A mapped view held forever is an arena leak on long sessions. Well-behaved
    // Win32 code never touches a view after UnmapViewOfFile — return it to the arena.
    K("UnmapViewOfFile",1,[](Cpu&c){ uint32_t a=c.arg(0);
        uint32_t bs=g_vaA.size_of(a);
        if(bs && g_vaA.free(a)){ ++g_unmapN; g_unmapB+=bs; }
        return 1u; });
}
