// d2ini.cpp -- see d2ini.h.
//
// D2.ini: the same 58 options as the command line, read BEFORE it.
//
// Game.exe 1.14d reads them in sub_405450: a TABLE-DRIVEN loop over
// ds:0x705040, step 0x5c, length 0x14d8 => 58 entries. Each record:
//   +0x00 section (char[0x1b])  +0x1b key (char[0x36])  +0x51 type
//   +0x54 field (offset into the options struct)         +0x58 default
// type 0 = boolean (GetPrivateProfileIntA != 0 -> one byte), 1 = integer
// (dword), 2 = string (GetPrivateProfileStringA, 27 bytes). The filename is
// built next to the executable ("D2.ini", ds:0x6cc830).
//
// Our two stubs used to return the caller's default: the file didn't exist,
// so NONE of the 58 flags were reachable without rebuilding. They're wired
// here to a real file, resolved by host_path() like everything else (write
// directory first, then data directory).
//
// This is TOOLING: without a D2.ini on disk, behavior is EXACTLY the old one
// (no section found => caller's default).
#include "d2ini.h"
#include "runtime/path_cache.h"
#include "platform/vita_present.h"
#include <cctype>
#include <cstdio>
#include <map>
using namespace d2rt;

struct IniFile {
    // section (lowercase) -> key (lowercase) -> raw value
    std::map<std::string,std::map<std::string,std::string>> sec;
    bool loaded=false, present=false;
    std::string path;
};
static std::map<std::string,IniFile> g_iniCache;
static std::string ini_lower(const std::string& v){ std::string o=v;
    for(auto&ch:o) ch=(char)std::tolower((unsigned char)ch); return o; }
static std::string ini_trim(const std::string& v){
    size_t a=v.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return "";
    size_t b=v.find_last_not_of(" \t\r\n"); return v.substr(a,b-a+1); }
// The name goes through host_path(): Win32 resolves an absolute path, we take
// the base name and search the write directory then the data directory.
static IniFile& ini_get(const std::string& guestPath){
    size_t s=guestPath.find_last_of("\\/");
    std::string base = s==std::string::npos ? guestPath : guestPath.substr(s+1);
    std::string key=ini_lower(base);
    auto it=g_iniCache.find(key);
    if(it!=g_iniCache.end()) return it->second;
    IniFile& f=g_iniCache[key];
    f.loaded=true; f.path=host_path(base);
    FILE* fp=std::fopen(f.path.c_str(),"rb");
    if(!fp){ f.present=false;
        char m[192]; std::snprintf(m,sizeof m,"ini: %s ABSENT (%s) -> defauts de l'appelant",base.c_str(),f.path.c_str());
        std::printf("  [%s]\n",m); d2vita_progress(m);
        return f; }
    f.present=true;
    std::string cur; char line[512]; int nk=0;
    while(std::fgets(line,sizeof line,fp)){
        std::string L=ini_trim(line);
        if(L.empty()||L[0]==';'||L[0]=='#') continue;
        if(L[0]=='['){ size_t e=L.find(']'); if(e!=std::string::npos) cur=ini_lower(ini_trim(L.substr(1,e-1))); continue; }
        size_t eq=L.find('=');  if(eq==std::string::npos) continue;
        std::string k=ini_lower(ini_trim(L.substr(0,eq))), v=ini_trim(L.substr(eq+1));
        f.sec[cur][k]=v; nk++; }
    std::fclose(fp);
    { char m[192]; std::snprintf(m,sizeof m,"ini: %s LU (%s) — %zu section(s), %d cle(s)",
        base.c_str(),f.path.c_str(),f.sec.size(),nk);
      std::printf("  [%s]\n",m); d2vita_progress(m); }
    return f;
}
// Returns true + the raw value when the key REALLY exists; false otherwise
// (and the caller then serves the guest's default, as Win32 does).
bool ini_lookup(const std::string& guestPath,const std::string& section,
                       const std::string& key,std::string& out){
    IniFile& f=ini_get(guestPath); if(!f.present) return false;
    auto a=f.sec.find(ini_lower(section)); if(a==f.sec.end()) return false;
    auto b=a->second.find(ini_lower(key)); if(b==a->second.end()) return false;
    out=b->second; return true; }
