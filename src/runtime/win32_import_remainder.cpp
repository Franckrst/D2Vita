// win32_import_remainder.cpp -- see win32_import_remainder.h.
#include "win32_import_remainder.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/guest_thread.h"       // full d2rt::ThreadScheduler (g_sched->request_shutdown())
#include "runtime/win32_shims_gdi32.h"  // wx86_screen_w/wx86_screen_h
#include "runtime/d2ini.h"              // ini_lookup() for GetPrivateProfileStringA/IntA
#include "platform/vita_present.h"      // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

void win32_import_remainder_install(Bridge& br){
    bool trace = getenv("TRACE")!=nullptr;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("KERNEL32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))   // wrapper only when tracing can ever fire — else register the bare fn (one less std::function hop per call)
            s.fn=[fn,tag,trace](Cpu&c)->uint32_t{ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s);
    };
    auto U=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        std::string tag=std::string("USER32.dll!")+name;
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=tag;
        if(trace||getenv("TRACEAFTER"))
            s.fn=[fn,tag,trace](Cpu&c){ uint32_t r=fn(c); if(trace||g_traceOn) std::printf("    %3d %-40s -> 0x%08x\n",++g_calls,tag.c_str(),r); return r; };
        else s.fn=std::move(fn);
        br.register_shim("USER32.dll",name,s);
    };
    auto REG=[&](const char* dll,const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string(dll)+"!"+name; s.fn=std::move(fn);
        br.register_shim(dll,name,s); };

    // ---------------- KERNEL32: the remainder ----------------
    // The fourteen string, console, and date/time formatting shims that used
    // to live here moved to the engine's locale support.
    K("FatalAppExitA",2,[&br](Cpu&c){ char m[160]; std::string t=gread_mb(c,c.arg(1),-1);
        std::snprintf(m,sizeof m,"FatalAppExitA: %s",t.c_str()); d2vita_progress(m);
        g_stop=true; g_stopReason="FatalAppExitA"; if(g_sched) g_sched->request_shutdown();
        br.redirect_next(br.sentinel()); return 0u; });
    // RaiseException: exception 0x406D1388 (MSVC thread-name exception) is
    // raised then ALWAYS continued — swallowing it matches real Windows
    // behavior. Anything else = a real guest error -> named halt (no
    // corrupted frame).
    K("RaiseException",4,[&br](Cpu&c){ uint32_t code=c.arg(0);
        if(code==0x406D1388u) return 0u;
        char m[96]; std::snprintf(m,sizeof m,"RaiseException 0x%08x -> arret controle",code); d2vita_progress(m);
        g_stop=true; g_stopReason=m; if(g_sched) g_sched->request_shutdown();
        br.redirect_next(br.sentinel()); return 0u; });
    K("FindResourceA",3,[](Cpu&c){ set_lasterr(c,1814); return 0u; });            // RESOURCE_NAME_NOT_FOUND
    K("LoadResource",2,[](Cpu&){ return 0u; });
    K("LockResource",1,[](Cpu&){ return 0u; });
    K("SizeofResource",2,[](Cpu&){ return 0u; });
    K("FreeResource",1,[](Cpu&){ return 1u; });
    K("GlobalLock",1,[](Cpu&c){ return c.arg(0); });                              // GlobalAlloc returns the address
    K("GlobalUnlock",1,[](Cpu&){ return 1u; });
    K("GetFileTime",4,[](Cpu&c){ uint8_t z[8]={0};
        for(int i=1;i<4;i++) if(c.arg(i)) c.write(c.arg(i),z,8); return 1u; });
    K("GetTempFileNameA",4,[](Cpu&c){ static uint32_t uq=0x100;
        std::string path=gread_mb(c,c.arg(0),-1), pre=gread_mb(c,c.arg(1),-1);
        uint32_t u=c.arg(2)?c.arg(2):++uq; char t[300];
        std::snprintf(t,sizeof t,"%s\\%.3s%04X.TMP",path.c_str(),pre.c_str(),u&0xFFFF);
        if(c.arg(3)) c.write(c.arg(3),t,(uint32_t)std::strlen(t)+1); return u; });
    K("GetExitCodeProcess",2,[](Cpu&c){ if(c.arg(1)) c.write_u32(c.arg(1),0); return 1u; });
    K("GetProcessId",1,[](Cpu&){ return 1u; });
    K("ReadProcessMemory",5,[](Cpu&c){ uint32_t h=c.arg(0),src=c.arg(1),dst=c.arg(2),n=c.arg(3);
        if(h!=0xFFFFFFFFu){ set_lasterr(c,5); return 0u; }        // only the current process
        std::vector<uint8_t> b(n); c.read(src,b.data(),n); c.write(dst,b.data(),n);
        if(c.arg(4)) c.write_u32(c.arg(4),n); return 1u; });      // honest self-introspection
    K("CreateProcessA",10,[](Cpu&c){ set_lasterr(c,2); return 0u; });             // external launch: clean failure
    K("IsBadStringPtrA",2,[](Cpu&c){ return c.arg(0)?0u:1u; });
    // On exit, dump the guest CALLER CHAIN (return addresses on the stack in
    // the Game.exe .text range) into the progress file: on Vita3K the game
    // quits cleanly after name-entry WM_CHARs and stdout is lost — this is
    // the only way to see WHO decided to exit.
    static auto exit_chain=[](Cpu&c,const char* tag,uint32_t code){
        char m[200]; int p=std::snprintf(m,sizeof m,"%s code=%u chain:",tag,code);
        uint32_t lo=g_d2base+0x1000, hi=g_d2base+0x2cb5a1, esp=c.reg(R_ESP);
        for(int i=0,shown=0; i<512 && shown<10; i++){
            uint32_t v=c.read_u32(esp+i*4);
            if(v>=lo && v<hi){ p+=std::snprintf(m+p,(size_t)(sizeof m-p)," %08x",v); shown++; } }
        std::printf("[%s]\n",m); d2vita_progress(m); std::fflush(stdout); };
    // ExitProcess: Win32 terminates the WHOLE process and NEVER returns. The old
    // shim RETURNED to the guest, so the caller's `call ExitProcess; int3` guard
    // tripped the int3. Redirect the next guest fetch to the sentinel so NO guest
    // instruction after the CALL executes, then shut the scheduler down (all threads).
    K("ExitProcess",1,[&br](Cpu&c)->uint32_t{ exit_chain(c,"ExitProcess",c.arg(0));
        br.redirect_next(br.sentinel());
        g_stop=true; g_stopReason="ExitProcess"; if(g_sched) g_sched->request_shutdown();
        return c.arg(0); });
    K("TerminateProcess",2,[&br](Cpu&c)->uint32_t{ exit_chain(c,"TerminateProcess",c.arg(1));
        br.redirect_next(br.sentinel());
        g_stop=true; g_stopReason="TerminateProcess"; if(g_sched) g_sched->request_shutdown();
        return 1u; });
    K("FormatMessageA",7,[](Cpu&c){ uint32_t flags=c.arg(0),buf=c.arg(4); const char* m="Error"; int n=(int)std::strlen(m)+1;
        if(flags&0x100u){ uint32_t p=halloc(n); c.write(p,m,n); c.write_u32(buf,p);} else c.write(buf,m,n); return (uint32_t)(n-1); });
    // .ini readers — wired to a REAL file (see ini_get/ini_lookup). A missing
    // file or key => UNCHANGED Win32 semantics: the caller's default (IntA) /
    // lpDefault copied into the buffer (StringA). So the old behavior stays
    // the default, and D2's 58 flags become tunable by dropping a D2.ini into
    // the data or write directory.
    K("GetPrivateProfileStringA",6,[](Cpu&c){ uint32_t def=c.arg(2),out=c.arg(3),sz=c.arg(4);
        std::string s=def?gread_mb(c,def,-1):"";
        { std::string sect=c.arg(0)?gread_mb(c,c.arg(0),-1):"", key=c.arg(1)?gread_mb(c,c.arg(1),-1):"",
                      file=c.arg(5)?gread_mb(c,c.arg(5),-1):"", v;
          if(!file.empty() && !sect.empty() && !key.empty() && ini_lookup(file,sect,key,v)){
              s=v;
              if(env_filelog()) std::fprintf(stderr,"    [ini] [%s] %s = \"%s\"\n",sect.c_str(),key.c_str(),v.c_str()); } }
        if(sz&&s.size()>=sz) s.resize(sz-1);
        if(out) c.write(out,s.c_str(),(uint32_t)s.size()+1); return (uint32_t)s.size(); });
    K("GetPrivateProfileIntA",4,[](Cpu&c){
        std::string sect=c.arg(0)?gread_mb(c,c.arg(0),-1):"", key=c.arg(1)?gread_mb(c,c.arg(1),-1):"",
                    file=c.arg(3)?gread_mb(c,c.arg(3),-1):"", v;
        if(!file.empty() && !sect.empty() && !key.empty() && ini_lookup(file,sect,key,v)){
            uint32_t r=(uint32_t)strtoul(v.c_str(),nullptr,0);
            if(env_filelog()) std::fprintf(stderr,"    [ini] [%s] %s = %u\n",sect.c_str(),key.c_str(),r);
            return r; }
        return c.arg(2); });                                       // -> nDefault

    // ---------------- USER32: real clipboard + remainder ----------------
    static uint32_t s_clipFmt=0, s_clipPtr=0;
    U("OpenClipboard",1,[](Cpu&){ return 1u; });
    U("CloseClipboard",0,[](Cpu&){ return 1u; });
    U("EmptyClipboard",0,[](Cpu&){ s_clipFmt=0; s_clipPtr=0; return 1u; });
    U("SetClipboardData",2,[](Cpu&c){ s_clipFmt=c.arg(0); s_clipPtr=c.arg(1); return s_clipPtr; });
    U("GetClipboardData",1,[](Cpu&c){ return c.arg(0)==s_clipFmt?s_clipPtr:0u; });
    U("IsClipboardFormatAvailable",1,[](Cpu&c){ return (s_clipPtr&&c.arg(0)==s_clipFmt)?1u:0u; });
    U("GetKeyboardLayout",1,[](Cpu&){ uint32_t l=detect_lcid(); return (l<<16)|l; });
    U("IsIconic",1,[](Cpu&){ return 0u; });
    U("IsWindowVisible",1,[](Cpu&){ return 1u; });
    U("MoveWindow",6,[](Cpu&){ return 1u; });
    U("GetWindowPlacement",2,[](Cpu&c){ uint32_t p=c.arg(1); if(!p) return 0u;
        c.write_u32(p,44); c.write_u32(p+4,0); c.write_u32(p+8,1);               // length,flags,SW_SHOWNORMAL
        for(int i=3;i<7;i++) c.write_u32(p+4u*i,0);
        c.write_u32(p+28,0); c.write_u32(p+32,0); c.write_u32(p+36,wx86_screen_w()); c.write_u32(p+40,wx86_screen_h()); return 1u; });
    U("MonitorFromWindow",2,[](Cpu&){ return 0x87000009u; });
    U("GetMonitorInfoA",2,[](Cpu&c){ uint32_t p=c.arg(1); if(!p) return 0u;
        c.write_u32(p+4,0); c.write_u32(p+8,0); c.write_u32(p+12,wx86_screen_w()); c.write_u32(p+16,wx86_screen_h());   // rcMonitor
        c.write_u32(p+20,0); c.write_u32(p+24,0); c.write_u32(p+28,wx86_screen_w()); c.write_u32(p+32,wx86_screen_h()); // rcWork
        c.write_u32(p+36,1); return 1u; });                                       // MONITORINFOF_PRIMARY
    U("DrawTextA",5,[](Cpu&c){ uint32_t r=c.arg(3),flags=c.arg(4);
        if(r&&(flags&0x400u)){ int32_t len=(int32_t)c.arg(2); if(len<0){ std::string t=gread_mb(c,c.arg(1),-1); len=(int32_t)t.size(); }
            uint32_t l=c.read_u32(r),t2=c.read_u32(r+4);
            c.write_u32(r+8,l+(uint32_t)len*8u); c.write_u32(r+12,t2+16u); }      // DT_CALCRECT
        return 16u; });
    // wsprintfA/wvsprintfA: CDECL varargs (caller cleans up) -> manual Shim,
    // argc=0/cleanup=false. Mini-formatter %[-0width][l]{d,i,u,x,X,s,c,%}.
    { auto fmtA=[](Cpu& c,uint32_t fmtp,std::function<uint32_t()> next)->std::string{
          std::string f=gread_mb(c,fmtp,-1), out;
          for(size_t i=0;i<f.size()&&out.size()<1024;i++){
              char ch=f[i];
              if(ch!='%'){ out+=ch; continue; }
              std::string spec="%"; ++i;
              while(i<f.size()&&(f[i]=='-'||f[i]=='0'||f[i]=='#'||f[i]==' '||(f[i]>='1'&&f[i]<='9')||f[i]=='.')) spec+=f[i++];
              if(i<f.size()&&f[i]=='l') spec+=f[i++];
              if(i>=f.size()) break;
              char cv=f[i]; char tmp[64];
              if(cv=='%'){ out+='%'; continue; }
              if(cv=='s'){ uint32_t p=next(); out+= p?gread_mb(c,p,-1):std::string("(null)"); continue; }
              if(cv=='c'){ out+=(char)next(); continue; }
              if(cv=='d'||cv=='i'){ spec+="d"; std::snprintf(tmp,sizeof tmp,spec.c_str(),(int32_t)next()); out+=tmp; continue; }
              if(cv=='u'){ spec+="u"; std::snprintf(tmp,sizeof tmp,spec.c_str(),next()); out+=tmp; continue; }
              if(cv=='x'||cv=='X'){ spec+=cv; std::snprintf(tmp,sizeof tmp,spec.c_str(),next()); out+=tmp; continue; }
              out+=cv;                                            // unknown: literal
          }
          return out; };
      Shim ws; ws.argc=0; ws.stdcall_cleanup=false; ws.tag="USER32.dll!wsprintfA";
      ws.fn=[fmtA](Cpu&c)->uint32_t{ uint32_t E=c.reg(R_ESP);
          uint32_t buf=c.read_u32(E+4), fp=c.read_u32(E+8), ai=E+12;
          std::string r=fmtA(c,fp,[&](){ uint32_t v=c.read_u32(ai); ai+=4; return v; });
          if(buf) c.write(buf,r.c_str(),(uint32_t)r.size()+1);
          return (uint32_t)r.size(); };
      br.register_shim("USER32.dll","wsprintfA",ws);
      Shim wv=ws; wv.tag="USER32.dll!wvsprintfA";
      wv.fn=[fmtA](Cpu&c)->uint32_t{ uint32_t E=c.reg(R_ESP);
          uint32_t buf=c.read_u32(E+4), fp=c.read_u32(E+8), va=c.read_u32(E+12);
          std::string r=fmtA(c,fp,[&](){ uint32_t v=c.read_u32(va); va+=4; return v; });
          if(buf) c.write(buf,r.c_str(),(uint32_t)r.size()+1);
          return (uint32_t)r.size(); };
      br.register_shim("USER32.dll","wvsprintfA",wv); }

    // ---------------- ADVAPI32: NT services (never actually a service) -----
    // (A() used to live here before ADVAPI32 was extracted — REG() is the
    // same registration mechanism, already in scope at this point.)
    REG("ADVAPI32.dll","RegisterServiceCtrlHandlerA",2,[](Cpu&c){ set_lasterr(c,1063); return 0u; });
    REG("ADVAPI32.dll","SetServiceStatus",2,[](Cpu&){ return 0u; });
    REG("ADVAPI32.dll","StartServiceCtrlDispatcherA",1,[](Cpu&c){ set_lasterr(c,1063); return 0u; }); // FAILED_SERVICE_CONTROLLER_CONNECT

    // ---------------- ole32 / VERSION / PSAPI ------------------------------
    REG("ole32.dll","CoTaskMemFree",1,[](Cpu&){ return 0u; });
    // VerQueryValueA (VERSION.dll) and GetModuleInformation (PSAPI.DLL) ->
    // see win32_shims_version_install()/win32_shims_psapi_install() above.
    REG("PSAPI.DLL","GetModuleFileNameExA",4,[](Cpu&c)->uint32_t{ std::string path=module_path_for(c.arg(1));
        if(path.empty()){ set_lasterr(c,6); return 0u; }
        uint32_t b=c.arg(2),n=c.arg(3); if(!b||!n) return 0u; uint32_t len=(uint32_t)path.size(); if(len>=n) len=n-1;
        c.write(b,path.c_str(),len); uint8_t z=0; c.write(b+len,&z,1); return len; });
}
