// path_cache.cpp -- see path_cache.h.
#include "path_cache.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <vector>
using namespace d2rt;

uint32_t host_filesize(const std::string& p){ FILE* f=std::fopen(p.c_str(),"rb"); if(!f) return 0; std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::fclose(f); return (uint32_t)(n>0?n:0); }
// Windows-style wildcard matching ("*" and "?"), case-insensitive. Replaces a
// naive "does the name end with what follows the star" test, which ignored
// everything BEFORE the star: "VITA.*" would match any file in either root,
// and a pattern with no star matched everything. That breaks D2's "delete all
// versions of the character" loop, which keeps calling
// FindFirstFileA/GetFileAttributesA/DeleteFileA as long as a match is found —
// with the naive matcher it always found one, so the loop never ended and the
// main thread stopped pumping messages.
// Windows quirk preserved: "*.*" matches ALL files, including ones with no dot.
bool wild_match(const char* pat, const char* s){
    if(!std::strcmp(pat,"*.*") || !std::strcmp(pat,"*")) return true;
    const char* star=nullptr; const char* ss=s;
    while(*s){
        char p=*pat;
        if(p=='?' || (p && std::tolower((unsigned char)p)==std::tolower((unsigned char)*s))){ ++pat; ++s; continue; }
        if(p=='*'){ star=pat++; ss=s; continue; }
        if(star){ pat=star+1; s=++ss; continue; }
        return false;
    }
    while(*pat=='*') ++pat;
    return *pat==0;
}
void fill_finddata(Cpu&c, uint32_t p, const std::string& name, uint32_t sz){
    std::vector<uint8_t> z(0x140,0); c.write(p,z.data(),0x140);
    c.write_u32(p+0x00, 0x80);            // dwFileAttributes = FILE_ATTRIBUTE_NORMAL
    c.write_u32(p+0x20, sz);              // nFileSizeLow
    uint32_t nl=(uint32_t)std::min(name.size(),(size_t)259); c.write(p+0x2c, name.c_str(), nl); c.write_u32(p+0x2c+((nl+3)&~3u),0);
}

// ---- Directory index: one readdir per DIRECTORY, not one per FILE ----------
//
// A write directory cluttered with old save slots can turn the very first
// frame's file opens into tens of seconds of work versus a few seconds for a
// fresh directory with the same entries — the guest does IDENTICAL work
// either way (same read counts); only the time to get there changes.
//
// The culprit is host_path()'s readdir fallback. Win32 resolves names
// case-INsensitively, but the Vita filesystem does not, hence a directory
// scan whenever the exact name fails. But that failure is the NORMAL case —
// every data file is looked up in g_writeRoot first, where it typically isn't
// — so EVERY open paid for a full scan. FAT/exFAT never shrinks a directory:
// slots left behind by deleted saves are still walked every time.
//
// This module replaces "one readdir per open" with "one readdir per directory
// per generation". It does NOT change resolution semantics: same order
// (g_writeRoot then g_dataRoot — a file the game just wrote must be found by
// the next read, or the game loops rebuilding it, the Realms.bin / .d2s
// pattern), and the same precedence of an EXACT name over a differently-cased
// one (the old direct fopen ran before any scan).
//
// Escape hatch: D2_PATHCACHE=0 restores the old scan-every-time behavior
// exactly. The default is the index ACTIVE: this is a fix, not an experiment.

// EXPLICIT ASCII lowercasing: neither tolower() nor strcasecmp(), whose
// folding depends on locale. The index key, the lookup key, AND the fallback
// scan's comparison all now go through the SAME notion of case. This is what
// guarantees the fallback and the index can never contradict each other in a
// loop ("index says absent -> scan finds it -> rebuild -> index still says
// absent"). This is not a resolution change, even under D2_PATHCACHE=0: the
// binary never calls setlocale anywhere, so strcasecmp was already doing
// ASCII folding, identically.
static std::string pc_lower(std::string s){
    for(char& ch : s) if(ch>='A' && ch<='Z') ch=(char)(ch+32);
    return s;
}
static bool pc_ieq(const char* a, const char* b){
    for(;;){ unsigned char x=(unsigned char)*a++, y=(unsigned char)*b++;
        if(x>='A'&&x<='Z') x=(unsigned char)(x+32);
        if(y>='A'&&y<='Z') y=(unsigned char)(y+32);
        if(x!=y) return false;
        if(!x)   return true; }
}

// LEAF lock. host_path() is called from file shims, so normally under the
// GIL; but the select shim RELEASES the GIL during its network waits, so two
// host threads can now enter file shims in parallel. This mutex is taken ONLY
// inside this module's functions, which acquire no other project lock (not
// the GIL, not the scheduler) and only call d2_crashlog AFTER releasing it. It
// therefore cannot enter any cycle: a leaf of the lock graph, by construction.
static pthread_mutex_t g_pcMu = PTHREAD_MUTEX_INITIALIZER;
PcLock::PcLock(){ pthread_mutex_lock(&g_pcMu); }
PcLock::~PcLock(){ pthread_mutex_unlock(&g_pcMu); }

// WHAT AN "ABSENT" ANSWER IS WORTH, AND WHY THE SAFETY NET IS PERIODIC
//
// A directory's index is the result of ONE readdir. An "absent" answer drawn
// from it is therefore not a guess: it's the verdict of a full directory scan,
// read once instead of once per file. Redoing a readdir on the FIRST absence
// of each name — the literal reading of a safety net — would report nothing
// as long as the directory hasn't changed: it would be the same scan, redone
// to answer the same thing, and on a cluttered console directory that is
// exactly the cost this index removes.
//
// The only thing that can make the index wrong is a directory change that
// didn't go through pc_dirty (an uncatalogued write site, present or future).
// The safety net targets exactly that and nothing else: every kMemoRescan
// lookups of the SAME name, an "absent" answer is re-verified by the
// HISTORICAL scan. If it finds the file, the correct path is served, the
// index is rebuilt, and it is written to crash.log.
//
// The property being protected: the failure this net must catch is one that
// BREAKS the game — a file written then read back as missing, or the game
// LOOPING to rebuild it. That loop replays the same name thousands of times,
// so the net fires after kMemoRescan rounds, corrects itself, and reports.
// The net's firing rate tracks the game's own insistence, which is exactly
// where the risk is. A name read only ONCE after an uncatalogued write can be
// misresolved for up to kMemoRescan reads: that window is FINITE, bounded by a
// counter (not by a filesystem property — FAT timestamps have 2 s granularity,
// using them would be a diagnostic that lies on console), shown in the
// summary, and pinned by the T7/T9 self-test.

PcStats g_pc;
std::map<std::string,DirIndex> g_pcDirs;

// D2_PATHCACHE: escape hatch. The DEFAULT is the index ACTIVE (this is a fix,
// not an experiment); any value starting with 0, n/N, f/F, or "of" disables it
// — "off", "false", "non", "no", "0" — because a hand-typed env.txt value
// should never leave the index active silently. "on" stays active. The
// effective state is WRITTEN to crash.log at startup (pc_announce): it is
// never inferred from the absence of a line.
bool pc_on(){
    static const bool v = [](){ const char* e=getenv("D2_PATHCACHE");
        if(!e || !*e) return true;
        char c=e[0];
        if(c=='0'||c=='n'||c=='N'||c=='f'||c=='F') return false;
        if((c=='o'||c=='O') && (e[1]=='f'||e[1]=='F')) return false;
        return true; }();
    return v;
}

// LAZY construction: one readdir per directory per generation. Called with
// THE LOCK HELD.
//
// A readdir that FAILS (opendir refused, or an error mid-scan) does not mean
// an EMPTY directory: it means NO information at all. Conflating the two
// would make the runtime spin (empty index -> fallback -> found -> "stale
// index" -> rebuild -> empty index ...), more expensive than the old code and
// noisy about it. So the directory is marked BLIND instead: its lookups fall
// back, unchanged, to the historical scan.
static void pc_build(const std::string& root, DirIndex& d){
    d.byLower.clear(); d.exact.clear(); d.absent.clear();
    d.entries=0; d.collisions=0; d.usable=false;
    if(DIR* dp=opendir(root.c_str())){
        // errno is reset to zero BEFORE EACH readdir and read right after: readdir
        // returns NULL both for "end of directory" and for "error", and only
        // errno tells them apart. Resetting it once before the loop would not
        // be enough — any call in the loop body (an allocation, say) can
        // dirty it and fabricate a false BLIND directory. This is the POSIX
        // idiom, and here it's the difference between "the directory is
        // empty" and "I don't know".
        for(;;){
            errno=0; struct dirent* e=readdir(dp);
            if(!e){ d.usable = (errno==0); break; }
            std::string nm=e->d_name;
            if(nm=="."||nm=="..") continue;      // never requested by the guest (see selfName in host_path)
            d.exact.insert(nm);
            if(!d.byLower.emplace(pc_lower(nm),nm).second) ++d.collisions;  // first one wins, same as the old readdir
            ++d.entries;
        }
        closedir(dp);
    }
    d.built=true; ++d.gen; ++g_pc.builds;
}

// Invalidation by GENERATION: mark the directory to rebuild; no incremental
// update is attempted (more fragile, no measurable gain).
void pc_dirty(const std::string& root){
    if(root.empty() || !pc_on()) return;
    PcLock lk;
    auto it=g_pcDirs.find(root);
    if(it!=g_pcDirs.end() && it->second.built){ it->second.built=false; ++g_pc.invalidations; }
}

// The HISTORICAL scan: fopen of the exact name, then a case-insensitive
// readdir. Stays the ONLY path under D2_PATHCACHE=0, serves as the noisy
// fallback when re-verification is due, and as the normal path for a BLIND
// directory. g_pc.scans counts scans ACTUALLY paid for: the only counter
// directly comparable to a kernel getdents64 count.
static bool pc_scan(const std::string& root, const std::string& base, std::string& out){
    if(root.empty()) return false;
    std::string exact = root + "/" + base;
    FILE* t=std::fopen(exact.c_str(),"rb"); if(t){ std::fclose(t); out=exact; return true; }
    if(DIR* d=opendir(root.c_str())){
        if(pc_on()){ PcLock lk; ++g_pc.scans; }   // outside the index: no lock held on the historical path
        struct dirent* e;
        while((e=readdir(d))){
            if(pc_ieq(e->d_name,base.c_str())){ out=root+"/"+e->d_name; closedir(d); return true; } }
        closedir(d);
    }
    return false;
}

// Index verdict for ONE directory.
//   PC_HIT      : found, `out` filled in.
//   PC_ABSENT   : absent, and the current generation's readdir is authoritative.
//   PC_NEEDSCAN : absent, but the periodic re-verification is DUE — the caller
//                 must redo the historical scan HERE, before moving to the
//                 next directory (the write-then-data ordering must hold even
//                 when the safety net fires).
//   PC_BLIND    : the directory could not be read — the index has NO opinion.
enum PcVerdict { PC_HIT, PC_ABSENT, PC_NEEDSCAN, PC_BLIND };
static PcVerdict pc_lookup(const std::string& root, const std::string& base,
                           std::string& out, uint32_t& gen){
    gen=0;
    if(root.empty()) return PC_ABSENT;          // root not configured: same as the old `if(root.empty()) continue`
    PcLock lk;
    DirIndex& d = g_pcDirs[root];
    if(!d.built) pc_build(root,d);
    ++g_pc.lookups;
    if(!d.usable){ ++g_pc.blind; return PC_BLIND; }
    if(d.exact.count(base)){ out=root+"/"+base; return PC_HIT; }     // exact name first (old direct fopen)
    std::string lo=pc_lower(base);
    auto it=d.byLower.find(lo);
    if(it!=d.byLower.end()){ out=root+"/"+it->second; return PC_HIT; }
    uint32_t& c = d.absent[lo];                 // 0 if the name has never been requested yet
    if(++c <= kMemoRescan){ ++g_pc.memo; return PC_ABSENT; }
    ++g_pc.rearm; gen=d.gen; return PC_NEEDSCAN;   // c stays > kMemoRescan until pc_note_absent resolves it
}

// The fallback scan runs WITHOUT the lock held (it does I/O). Another thread
// may therefore have invalidated and rebuilt the directory meanwhile. The two
// functions below guard against this with the SAME generation check:
// recording a verdict drawn from a stale generation — whether "absent" or
// "the index lied" — would itself be a diagnostic that lies.
static void pc_note_absent(const std::string& root, const std::string& base, uint32_t gen){
    if(root.empty()) return;
    PcLock lk;
    auto it=g_pcDirs.find(root);
    if(it!=g_pcDirs.end() && it->second.built && it->second.gen==gen)
        it->second.absent[pc_lower(base)]=1;    // verified absent: the window restarts at zero
}
// Returns true if the directory was ACTUALLY stale (same generation as the
// lookup); invalidates it and counts it. Otherwise it's a RACE, not the index
// lying: counted separately, never reported as "STALE INDEX".
static bool pc_invalidate_stale(const std::string& root, uint32_t gen){
    if(root.empty()) return false;
    PcLock lk;
    auto it=g_pcDirs.find(root);
    if(it==g_pcDirs.end()) return false;
    if(!it->second.built || it->second.gen!=gen){ ++g_pc.races; return false; }
    it->second.built=false; ++g_pc.stale;
    return true;
}

// Summary bounded by TIME (not by a modulo count): resolution throughput
// depends entirely on the game. At most one line every 5 s, and only if a
// lookup happened since the last one — silence then means "nobody is opening
// files anymore", never "writing stopped".
//
// TWO UNITS, LABELED AS SUCH. "resolutions" counts calls to host_path;
// "directory lookups" counts index queries, up to two per resolution (the
// data directory is only consulted if the write directory didn't answer). The
// two do not add up to each other, and the line no longer presents one as a
// breakdown of the other. The number to watch on console is "scans": that is
// the work this fix exists to remove, and it's directly comparable to a
// getdents64 count on the host.
static constexpr unsigned kPcReportMs = 5000;   // same TIME bound as the starvation and select summaries
void pc_report(bool force){
    if(!pc_on()) return;
    uint32_t builds,inval,look,hw,hd,memo,fb,stale,rearm,miss,scans,self,blind,races;
    char dirs[160]; int w=0; dirs[0]=0;
    {
        PcLock lk;
        if(!force && g_pc.lookups==g_pc.lastLookups) return;   // nothing new: bail BEFORE the syscall
        uint64_t now=rt_now_ms();                              // syscall: never paid for when there's nothing to report
        if(!force && g_pc.lastReport && now-g_pc.lastReport < kPcReportMs) return;
        g_pc.lastReport=now; g_pc.lastLookups=g_pc.lookups;
        builds=g_pc.builds; inval=g_pc.invalidations; look=g_pc.lookups;
        hw=g_pc.hitW; hd=g_pc.hitD; memo=g_pc.memo; fb=g_pc.fallbacks;
        stale=g_pc.stale; rearm=g_pc.rearm; miss=g_pc.misses;
        scans=g_pc.scans; self=g_pc.selfnames; blind=g_pc.blind; races=g_pc.races;
        for(auto& kv : g_pcDirs){
            if(w > (int)sizeof dirs - 48) break;
            const std::string& r=kv.first; size_t s=r.find_last_of('/');
            std::string nm = (s==std::string::npos) ? r : r.substr(s+1);
            w+=std::snprintf(dirs+w,sizeof dirs-(size_t)w,"%s%s=%u%s%s%s",
                             w?",":"", nm.c_str(), kv.second.entries,
                             kv.second.built?"":"/perime",
                             kv.second.usable?"":"/AVEUGLE",
                             kv.second.collisions?"/casse-double":"");
        }
    }
    d2_crashlog("pathcache: resolutions=%u (ecriture=%u donnees=%u introuvables=%u auto=%u) | "
                "balayages=%u (constructions=%u parcours=%u) | "
                "consultations-dossier=%u dont-absent-sans-balayage=%u | "
                "invalidations=%u re-verifications=%u replis=%u dont-INDEX-PERIME=%u | "
                "courses=%u aveugles=%u | entrees %s",
                hw+hd+miss+self, hw,hd,miss,self,
                builds+scans, builds, scans,
                look, memo,
                inval, rearm, fb, stale,
                races, blind, dirs[0]?dirs:"-");
}

// The switch's state is WRITTEN once, as soon as the write directory (hence
// crash.log) is known. Without this line, a run with D2_PATHCACHE=0 leaves no
// index trace at all, forcing the state to be INFERRED from an absence of
// lines — exactly the reasoning this project refuses to rely on. On console,
// where the knob is set blind in env.txt, this is the only way to know what a
// run actually did.
void pc_announce(){
    const char* e=getenv("D2_PATHCACHE");
    if(pc_on())
        d2_crashlog("pathcache: index de repertoire ACTIF (D2_PATHCACHE=%s) — re-verification par balayage toutes les %u reponses « absent » d'un meme nom",
                    e&&*e?e:"(non pose, defaut=actif)", kMemoRescan);
    else
        d2_crashlog("pathcache: index de repertoire DESACTIVE (D2_PATHCACHE=%s) — resolution par balayage historique, exactement comme avant le correctif",
                    e&&*e?e:"(vide)");
}

std::string host_path(const std::string& guest){
    // Take the basename (strip drive/dir) and resolve case-INsensitively
    // (Win32 semantics). Game-CREATED files (saves, logs) land in g_writeRoot,
    // so search it FIRST — a written file must be found by later reads, else
    // the game loops rebuilding it (Realms.bin / .d2s pattern). This ORDER IS
    // DELIBERATE and does not change; the index honors it too, including when
    // the safety net fires (a directory's scan happens BEFORE the next
    // directory is consulted, see the loop below).
    size_t s=guest.find_last_of("\\/"); std::string base = s==std::string::npos?guest:guest.substr(s+1);
    const std::string roots[2] = { g_writeRoot, g_dataRoot };
    // Names readdir does NOT return but the old direct fopen used to resolve:
    // the empty name (a guest path ending in "\", which then meant the
    // directory itself — fopen of a directory succeeds under glibc), "." and
    // "..". The index cannot hold them without lying about the directory's
    // contents, so they always go through the historical scan instead. This is
    // not a theoretical case: the noisy fallback exists precisely because it
    // is real. It is the only work this fix does not remove: it is COUNTED
    // ("auto=" in the summary) and its real scans count toward "scans", so any
    // leftover scans on console have a name instead of being a mystery.
    const bool selfName = base.empty() || base=="." || base=="..";
    if(!pc_on() || selfName){           // D2_PATHCACHE=0 : balayage HISTORIQUE, a l'identique
        if(selfName && pc_on()){ PcLock lk; ++g_pc.selfnames; }
        for(const std::string& root : roots){ std::string out;
            if(pc_scan(root,base,out)) return out; }
        return g_dataRoot + "/" + base;
    }
    // One directory after another, IN ORDER. When the index has no opinion
    // (unreadable directory) or its "absent" answer is due for
    // re-verification, the historical scan is redone RIGHT HERE, before
    // moving to the next directory: without that, a stale index on the WRITE
    // directory could be masked by a legitimate hit in the DATA directory,
    // precedence would be inverted, and the safety net would say nothing at all.
    //
    // WHAT THIS NET DOES NOT COVER, and it must be said: it only fires on an
    // "absent" verdict. A false POSITIVE (a file deleted but still in the
    // index) yields a path whose fopen will fail — exactly what the old code
    // did for a missing file — UNLESS the same base name also exists in the
    // other directory, in which case resolution would stop at the write
    // directory instead of serving the data directory's copy. That would
    // require an uncatalogued deletion: every deletion site in the runtime
    // (DeleteFileA/W, MoveFileA) invalidates the index.
    for(int i=0;i<2;i++){
        std::string out; uint32_t gen=0;
        PcVerdict v = pc_lookup(roots[i],base,out,gen);
        if(v==PC_HIT){
            { PcLock lk; if(i==0) ++g_pc.hitW; else ++g_pc.hitD; }
            pc_report(false);
            return out;
        }
        if(v==PC_ABSENT) continue;                     // the current generation's readdir is authoritative
        if(v==PC_BLIND){                               // unreadable directory: reported, capped at 4 times
            uint32_t n; { PcLock lk; n=g_pc.blind; }
            if(n<=4) d2_crashlog("pathcache: dossier ILLISIBLE (opendir/readdir en echec) — %s : balayage historique conserve pour ce dossier",
                                 roots[i].c_str());
        }
        { PcLock lk; ++g_pc.fallbacks; }
        if(pc_scan(roots[i],base,out)){
            bool perime = (v==PC_NEEDSCAN) && pc_invalidate_stale(roots[i],gen);
            if(perime){
                uint32_t n; { PcLock lk; n=g_pc.stale; }
                if(n<=8)   // rate-limited: detail line AND forced summary, otherwise an index lying in a loop floods crash.log
                    d2_crashlog("pathcache: INDEX PERIME — « %s » trouve par balayage alors que l'index le disait absent (%s) ; dossier reconstruit",
                                base.c_str(), roots[i].c_str());
                pc_report(n<=8);
            } else pc_report(false);
            return out;
        }
        if(v==PC_NEEDSCAN) pc_note_absent(roots[i],base,gen);   // verified absent: the window restarts at zero
    }
    { PcLock lk; ++g_pc.misses; }
    pc_report(false);
    return g_dataRoot + "/" + base;
}
