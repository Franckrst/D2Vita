// win32_shims_shell32.cpp -- see win32_shims_shell32.h.
#include "win32_shims_shell32_d2.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstring>
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_shell32_d2_install(Bridge& br){
    auto SH=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("SHELL32.dll!")+name;
        s.fn=[fn](Cpu&c){ return fn(c); };
        br.register_shim("SHELL32.dll",name,s); };
    // SHGetFolderPathA(hwnd,csidl,hToken,flags,pszPath) -> the game dir + S_OK.
    SH("SHGetFolderPathA",5,[](Cpu&c){ uint32_t b=c.arg(4); const char* s="C:\\Diablo II";
        if(b) c.write(b,s,(uint32_t)std::strlen(s)+1); return 0u; });
}
