// netguard_lock.cpp -- see netguard_lock.h.
#include "netguard_lock.h"
#include "runtime/net_guard.h"        // wx86_net_set_private_only
#include "platform/vita_present.h"    // d2vita_progress
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// --- NETWORK LOCK, OPEN BY DEFAULT FOR PUBLIC PLAY -----------------------
// HOUSE RULE: the client may reach an official server by default. The one
// thing that stays unconditional: no packet may leave for an official
// server while ANY debugging/automation tool is active, because each one
// would make the client NON-FAITHFUL as seen by the server (fabricated
// response via stub CheckRevision, rewritten destination, skewed clock, or
// automated input -- bot-like behavior). D2_LOCAL_ONLY=1 opts back into the
// old restrictive behavior (local allowlist only) for development/testing.
void netguard_lock_install(){
    {   const char* e=getenv("D2_LOCAL_ONLY");
        bool prive = e && *e && std::strcmp(e,"0")!=0;
        if(prive) d2vita_progress("netguard: D2_LOCAL_ONLY actif -- verrou local force");
        // UNCONDITIONAL: refuse official traffic while any debugging/automation
        // tool is active, lock or no lock. An ordinary player never has these
        // set -- this only ever bites a developer's own bench run. Tool NAMES
        // are logged, never their values (D2_PASSWORD is one of them).
        if(!prive){
            auto nonvide=[](const char* n){ const char* v=getenv(n); return v && *v; };
            auto pas1=[](const char* n){ const char* v=getenv(n); return v && *v && std::strcmp(v,"1")!=0; };
            struct { const char* nom; bool actif; } outils[]={
                {"D2CR_STUB",      nonvide("D2CR_STUB")},        // fabricated version
                {"D2BNCS_LOCAL",   nonvide("D2BNCS_LOCAL")},     // rewritten destination
                {"D2_RTSCALE",     pas1("D2_RTSCALE")},          // accelerated clock
                {"D2_INGAME_SLOW", pas1("D2_INGAME_SLOW")},      // slowed in-game clock
                {"D2_VIRTCLOCK",   nonvide("D2_VIRTCLOCK")},     // virtual clock
                {"D2_FAKEWALL",    nonvide("D2_FAKEWALL")},      // fake wall clock
                {"D2_AUTOBNET",    nonvide("D2_AUTOBNET")},      // automatic click
                {"D2_AUTOLOGIN",   nonvide("D2_AUTOLOGIN")},     // automatic login
                {"D2_ACCOUNT",     nonvide("D2_ACCOUNT")},
                {"D2_PASSWORD",    nonvide("D2_PASSWORD")},
                {"D2CMDFILE",      nonvide("D2CMDFILE")},        // remotely injected input
                {"D2SCRIPT",       nonvide("D2SCRIPT")},         // scripted input
            };
            std::string actifs;
            for(auto& o:outils) if(o.actif){ if(!actifs.empty()) actifs+=","; actifs+=o.nom; }
            if(!actifs.empty()){
                prive=true;
                char m[256]; std::snprintf(m,sizeof m,"netguard: outil(s) de mise au point actif(s) : %s -- trafic public REFUSE",actifs.c_str());
                std::fprintf(stderr,"[%s]\n",m); d2vita_progress(m); }
        }
        if(!prive){
            // Safety net: if the string CheckRevision hashes isn't Game.exe
            // 1.14d's (host path miss -> falls back to 0.0.0.0), the network is
            // cut before sending. Only set if the caller hasn't chosen one.
            if(!getenv("D2CR_REQUIRE_VERSION")) setenv("D2CR_REQUIRE_VERSION","1.14.3.71",1);
            d2vita_progress("netguard: trafic public autorise (D2CR_REQUIRE_VERSION arme)"); }
        wx86_net_set_private_only(prive); }
}
