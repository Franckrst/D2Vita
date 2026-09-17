// kernel32_w_variants.cpp -- see kernel32_w_variants.h.
#include "kernel32_w_variants.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/path_cache.h"   // host_path()
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/stat.h>
using namespace d2rt;

void kernel32_w_variants_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    // 1.14d's save-path code (hit at world load) walks the filesystem through
    // the W APIs; unshimmed they ESP-drift and the game thread rets into
    // stack garbage (the deterministic eip=0x117a4 derail on Vita — desktop
    // escaped only because its write dirs pre-exist, so the W create/probe
    // branch never ran). Narrow UTF-16 to ASCII and mirror the A logic.
    // (wnarrow() itself lives at file scope in tools/rt_boot.cpp -- the
    // remaining KERNEL32 W-variants there and kernel32_files.cpp's CreateFileW
    // need it too; see runtime/rt_host.h.)
    K("GetFileAttributesW",1,[](Cpu&c){ std::string n=wnarrow(c,c.arg(0));
        struct stat st;
        if(stat(host_path(n).c_str(),&st)==0) return S_ISDIR(st.st_mode)?0x10u:0x80u;
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        if(stat((g_writeRoot+"/"+base).c_str(),&st)==0) return S_ISDIR(st.st_mode)?0x10u:0x80u;
        set_lasterr(c,2); return 0xFFFFFFFFu; });
    K("CreateDirectoryW",2,[](Cpu&c){ std::string n=wnarrow(c,c.arg(0));
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        mkdir((g_writeRoot+"/"+base).c_str(),0755); pc_dirty(g_writeRoot); return 1u; });
    // CreateFileW is registered by kernel32_files_install() (alongside
    // CreateFileA/do_create_file) -- same install call, no separate K() here.
    K("DeleteFileW",1,[](Cpu&c){ std::string n=wnarrow(c,c.arg(0));
        size_t s=n.find_last_of("\\/"); std::string base=s==std::string::npos?n:n.substr(s+1);
        std::remove((g_writeRoot+"/"+base).c_str()); pc_dirty(g_writeRoot); return 1u; });
    K("CopyFileW",3,[](Cpu&c){ std::string a=wnarrow(c,c.arg(0)), b=wnarrow(c,c.arg(1));
        auto bn=[](const std::string&q){ size_t s=q.find_last_of("\\/"); return s==std::string::npos?q:q.substr(s+1); };
        std::string src=host_path(a), dst=g_writeRoot+"/"+bn(b);
        FILE* fi=std::fopen(src.c_str(),"rb"); if(!fi){ set_lasterr(c,2); return 0u; }
        FILE* fo=std::fopen(dst.c_str(),"wb"); if(!fo){ std::fclose(fi); set_lasterr(c,5); return 0u; }
        char buf[4096]; size_t k; while((k=fread(buf,1,sizeof buf,fi))>0) fwrite(buf,1,k,fo);
        std::fclose(fi); std::fclose(fo); pc_dirty(g_writeRoot); return 1u; });
    K("FindFirstFileW",2,[](Cpu&c){ set_lasterr(c,2); return 0xFFFFFFFFu; });  // probes only (init MPQ scans use A)
}
