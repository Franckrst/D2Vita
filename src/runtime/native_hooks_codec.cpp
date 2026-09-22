// native_hooks_codec.cpp -- see native_hooks_codec.h. Globals/types crossing
// the file boundary lost their `static` (or moved to runtime/rt_host.h) to
// stay visible here. The "nested" group (cell loop/lightgrid/blend/RLE/
// collision, shared parallelism engine) stays in rt_boot.cpp.
#include "native_hooks_codec.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/layout.h"
#include "runtime/rt_host.h"
#include "runtime/dcc_native.h"
#include "runtime/scomp_audio.h"
#include "platform/vita_present.h"
#include "dyn86_memintrin.h"
#include "dyn86_intrin.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
using namespace d2rt;

// ---- ad hoc declarations (no dedicated header for these upstream either;
// same convention here) ----------------
extern "C" unsigned long long d2rt_audio_codec_n[4];   // 0=huff 1=adpcm-mono 2=adpcm-stereo 3=fallback
extern "C" int d2_intrin_114_register(uint32_t d2base);
extern "C" int d2_proj_verify;
extern "C" unsigned long long d2_proj_ver_n, d2_proj_ver_bad, d2_proj_ver_skip;
extern "C" uint32_t d2_proj_verify_trap;
extern "C" uint32_t d2_proj_verify_ret(void);
namespace d2rt { uint32_t scomp_explode_pkware_trunc(const uint8_t* in, uint32_t in_len,
                                                     uint8_t* out, uint32_t out_max);
                 uint32_t scomp_explode_pkware(const uint8_t* in, uint32_t in_len,
                                               uint8_t* out, uint32_t out_max);
                 }

void native_hooks_codec_install_celwatch(Cpu* cpu, Bridge& br){
    // ---- D2_CELWATCH: CelData cache LRU evictions --------------------------
    // sub_6092d0(this=ecx, edx) is the cache class's LRU eviction: edx==0
    // means "make room" (called from 0x609461 when usage+size exceeds the
    // cap), edx!=0 means explicit eviction. The CelData cache object is
    // ds:0x88db20; comparing it against ecx separates its saturation from
    // other caches'. Entry prologue is `push ebx`, mirrored in the faithful
    // fallback. Active only under D2_CELWATCH.
    if(g_celWatch && g_114 && g_d2base){
        static uint32_t s_evEntry=0, s_celObj=0;
        s_evEntry=g_d2base+0x2092d0; s_celObj=g_d2base+0x48db20;
        Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!celdata_evict";
        s.fn=[&br](Cpu&c)->uint32_t{
            const uint32_t E=c.reg(R_ESP);
            if(c.reg(R_EDX)==0){ if(c.reg(R_ECX)==s_celObj) ++g_celEvict; else ++g_celEvictOther; }
            c.write_u32(E-4,c.reg(R_EBX));                // faithful fallback: `push ebx`
            c.set_reg(R_ESP,E-8);
            br.redirect_next(s_evEntry+1);
            return c.reg(R_EAX); };
        br.register_shim("native.hook","celdata_evict",s);
        cpu->set_alternate(s_evEntry,br.shim_trap("native.hook","celdata_evict"));
        std::printf("celwatch: evictions LRU comptees (alternate sur Game+0x2092d0), periode=%u images\n",g_celWatch);
    }
}

void native_hooks_codec_install_codecs(Cpu* cpu, Bridge& br){
    // NATIVESCOMP=1 (default) uses the native path, NATIVESCOMP=0 disables it.
    // 1.14d monolith RE: pklib explode statically linked at RVA 0x6b01b0
    // (located via the LenBits/ExLenBits const tables, RVA 0x34b6c8/0x34b698);
    // its single flat-buffer wrapper is RVA 0x15240 — stdcall(dst, src,
    // srcLen), ret 0xC, always returns 1. The mask dispatch happens UPSTREAM
    // (sector loop at RVA 0x16e30), so every call here is an actually-
    // compressed PKWARE block. Hooked with the translation-time alternate
    // (guest .text untouched — Warden-safe in all modes) and a faithful
    // fallback that re-enters the original body (emulated 'push ebp' +
    // entry+1) whenever the native path cannot serve the call.
    //
    // This isn't just an optimization: the guest's own x86 PKWARE decompressor
    // produces corrupted data when run through the dynarec on real ARM
    // hardware (a hardware-only dynarec bug, invisible under qemu-arm). The
    // native decoder (byte-exact, StormLib) bypasses that faulty path and
    // fixes both the "camp exit" (Gfx.cpp:1632) and "town load"
    // (CelDataHash.cpp:1420) crash families.
    // TODO upstream: identify the specific x86 instruction the dynarec
    // mistranslates in pklib explode, for a generic fix.
    const char* g_ns=getenv("NATIVESCOMP");
    if(g_114 && !br.module_base("D2gfx.dll") && !(g_ns && !strcmp(g_ns,"0"))){
        uint32_t GB=g_d2base;
        uint32_t entry=GB+0x15240;
        Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag="native!scomp_explode_114";
        auto scompBody=[entry,&br](Cpu&c)->uint32_t{
            uint32_t E=c.reg(R_ESP);                // -> retaddr (handler pops +4 after us)
            uint32_t dst=c.read_u32(E+4), src=c.read_u32(E+8), len=c.read_u32(E+12);
            // hostptr(va) only validates a single byte by default (cpu.h:52);
            // reading `len` bytes and writing up to 256 KiB behind such a
            // check could run past the arena. The SCompExplode twin
            // (0x41e000) already bounds both its buffers; here the
            // stdcall(dst,src,srcLen) ABI gives no output-capacity argument,
            // so we take the largest extent still inside the arena. Too
            // short (or out of arena) => n=0 => faithful fallback: the guest
            // does the work, visible behavior is unchanged, only host memory
            // corruption becomes impossible.
            const uint8_t* inH=(const uint8_t*)c.hostptr(src,len?len:1);
            uint32_t ocap=0x40000;
            uint8_t* outH=(uint8_t*)c.hostptr(dst,ocap);
            while(!outH && ocap>0x1000){ ocap>>=1; outH=(uint8_t*)c.hostptr(dst,ocap); }
            uint32_t n=0;
            static int forcefb=-1; if(forcefb<0) forcefb=getenv("D2_SCOMP_FORCEFB")?1:0;   // diag: force the fallback path
            if(!forcefb && inH && outH && len>1) n=d2rt::scomp_explode_pkware(inH,len,outH,ocap);
            if(n){ if(++g_scompN==1) d2vita_progress("scomp: explode natif actif (1er secteur servi)");
                c.set_reg(R_ESP,E+12);              // + handler's retaddr pop = ret 0xC
                return 1u; }                            // eax=1, like the original
            ++g_scompFB;                                // faithful fallback: original body
            c.write_u32(E-4,c.reg(R_EBP));          // emulates the entry 'push ebp'
            c.set_reg(R_ESP,E-8);                   // handler's +4 lands on E-4
            br.redirect_next(entry+1);                  // resumes at 'mov ebp,esp'
            return c.reg(R_EAX); };   // faithful fallback: EAX UNCHANGED
        s.fn=scompBody;
        br.register_shim("native.hook","scomp_explode_114",s);
        uint32_t trap=br.shim_trap("native.hook","scomp_explode_114");
        cpu->set_alternate(entry,trap);
        std::printf("native scomp (1.14d): Game+0x15240 => trap 0x%08x (alternate, no .text patch)\n",trap);
        d2vita_progress("native scomp (1.14d): hook alternate pose sur Game+0x15240");

        // --- Second PKWARE entry point: SCompExplode, VA 0x41e000 -----------
        // The hook above only covers the flat-buffer wrapper (0x415240).
        // pklib explode (0x6b01b0) has a second caller at 0x41e050, inside
        // SCompExplode — which SCompDecompress reaches through a function
        // POINTER, via the .rdata decompressor table at 0x6cfec8 (fn/mask
        // pairs; 0x41e000 = PKWARE mask). No `call rel32` targets it
        // directly, so the alternate must be captured at indirect-call
        // resolution time instead: LinkNext (dynarec.c) checks hasAlternate
        // on targets resolved at runtime.
        // This isn't just a speed concern: this path has the same
        // real-hardware dynarec data-corruption issue as the first PKWARE
        // entry point, so leaving it emulated would keep that bug reachable.
        // ABI: ecx = output buffer, edx = &size (in: capacity, out: bytes
        // produced), [esp+4] = input buffer, [esp+8] = input size, ret 0xc.
        // The caller ignores the returned EAX (0x41e885 overwrites it with
        // `mov eax,edi` before any use — verified by disassembly).
        if(!(getenv("NATIVEEXPL") && !strcmp(getenv("NATIVEEXPL"),"0"))){
            uint32_t e2=GB+0x1e000;
            // D2_EXPLVERIFY=1: byte-by-byte oracle. Decodes into a COPY of
            // the output buffer, writes nothing, lets the guest do the real
            // work, then compares both the produced bytes and the reported
            // size at return. This is the only way to distinguish "the
            // decoder produces different bytes" from "the port no longer
            // matches the allocation" — both show up as the same on-screen
            // divergence.
            static uint32_t xv_exit=0, xv_ra=0, xv_out=0, xv_cap=0, xv_pcb=0, xv_size=0, xv_in=0, xv_inlen=0;
            static bool xv_armed=false;
            static std::vector<uint8_t> xv_exp;
            static uint64_t xv_calls=0, xv_bad=0;
            { Shim xx; xx.argc=0; xx.stdcall_cleanup=false; xx.tag="native!scomp2_verify_exit";
              xx.fn=[&br](Cpu&c)->uint32_t{
                uint32_t geax=c.reg(R_EAX);
                if(xv_armed){ xv_armed=false; ++xv_calls;
                    std::vector<uint8_t> got(xv_cap);
                    c.read(xv_out,got.data(),xv_cap);
                    uint32_t gsize=c.read_u32(xv_pcb);
                    int first=-1; uint32_t bad=0;
                    for(uint32_t i=0;i<xv_cap;i++) if(got[i]!=xv_exp[i]){ if(first<0) first=(int)i; ++bad; }
                    g_xvCalls=xv_calls;
                    if(first>=0 || gsize!=xv_size){ ++g_xvBad;
                        if(g_xvBad==1){ g_xvFirst=first<0?0:(uint32_t)first;
                                        g_xvG=first<0?0:got[first]; g_xvN=first<0?0:xv_exp[first];
                                        g_xvSzG=gsize; g_xvSzN=xv_size;
                            char m[160]; std::snprintf(m,sizeof m,
                              "EXPLVERIFY: DIVERGENCE appel #%llu — %u/%u octets (1er +0x%x invite=%02x natif=%02x) taille invite=%u natif=%u",
                              (unsigned long long)xv_calls,bad,xv_cap,g_xvFirst,g_xvG,g_xvN,gsize,xv_size);
                            d2vita_progress(m); } }
                    if((first>=0 || gsize!=xv_size) && ++xv_bad<=12){
                        std::printf("[explverify] appel #%llu : %u/%u octets differents"
                                    " (1er +0x%x invite=%02x natif=%02x) taille invite=%u natif=%u\n",
                                    (unsigned long long)xv_calls,bad,xv_cap,first<0?0:first,
                                    first<0?0:got[first],first<0?0:xv_exp[first],gsize,xv_size);
                        // Capture the offending block (compressed input +
                        // both outputs) to replay both decoders offline.
                        if(xv_bad==1){
                            std::vector<uint8_t> inb(xv_inlen?xv_inlen:1);
                            c.read(xv_in,inb.data(),xv_inlen);
                            auto dump=[&](const char* nm,const uint8_t* p,uint32_t n){
                                char fn[256]; std::snprintf(fn,sizeof fn,"%s/d2_expl_%s.bin",g_writeRoot.c_str(),nm);
                                if(FILE* f=std::fopen(fn,"wb")){ std::fwrite(p,1,n,f); std::fclose(f);
                                    std::printf("[explverify]   -> %s (%u octets)\n",fn,n); } };
                            dump("in",inb.data(),xv_inlen);
                            dump("guest",got.data(),xv_cap);
                            dump("native",xv_exp.data(),xv_cap);
                        }
                    }
                    if(!(xv_calls%2000)) std::printf("[explverify] %llu appels compares, %llu divergences\n",
                                    (unsigned long long)xv_calls,(unsigned long long)xv_bad);
                    br.redirect_next(xv_ra); }
                c.set_reg(R_ESP,c.reg(R_ESP)-4);
                return geax; };
              br.register_shim("native.hook","scomp2_verify_exit",xx);
              xv_exit=br.shim_trap("native.hook","scomp2_verify_exit"); }
            Shim s2; s2.argc=0; s2.stdcall_cleanup=false; s2.tag="native!scomp_explode2_114";
            s2.fn=[e2,&br](Cpu&c)->uint32_t{
                uint32_t Rg[8]; c.regs_gp(Rg);                 // batched read (cpu.h regs_gp)
                uint32_t E=Rg[R_ESP];
                uint32_t out=Rg[R_ECX], pcb=Rg[R_EDX];
                uint32_t in=c.read_u32(E+4), inlen=c.read_u32(E+8);
                uint32_t cap=pcb?c.read_u32(pcb):0;
                uint32_t n=0;
                static const bool verify = getenv("D2_EXPLVERIFY")!=nullptr;
                if(verify && !xv_armed && cap && cap<=(8u<<20) && inlen>1){
                    const uint8_t* iH=(const uint8_t*)c.hostptr(in,inlen);
                    xv_exp.assign(cap,0);
                    // pre-fill with the CURRENT contents: bytes past the
                    // produced size are untouched by either side, so they
                    // must compare equal.
                    if(iH && c.read(out,xv_exp.data(),cap)){
                        xv_size=d2rt::scomp_explode_pkware_trunc(iH,inlen,xv_exp.data(),cap);
                        xv_out=out; xv_cap=cap; xv_pcb=pcb; xv_in=in; xv_inlen=inlen; xv_armed=true;
                        xv_ra=c.read_u32(E); c.write_u32(E,xv_exit); }
                    c.write_u32(E-4,c.reg(R_EBP));
                    c.set_reg(R_ESP,E-8);
                    br.redirect_next(e2+1);
                    return c.reg(R_EAX);
                }
                if(cap && inlen>1){
                    const uint8_t* iH=(const uint8_t*)c.hostptr(in,inlen);
                    uint8_t* oH=(uint8_t*)c.hostptr(out,cap);
                    // The guest write callback (0x41df00) TRUNCATES to
                    // capacity and lets the stream end there; we must match
                    // that, or a block that exactly fills the buffer would
                    // make us return 0 where the original returns the
                    // capacity.
                    if(iH && oH) n=d2rt::scomp_explode_pkware_trunc(iH,inlen,oH,cap);
                }
                if(n){ c.write_u32(pcb,n); ++g_scomp2N;
                    c.set_reg(R_ESP,E+12);              // ret 0xc
                    return 0u; }
                ++g_scomp2FB;
                c.write_u32(E-4,c.reg(R_EBP));
                c.set_reg(R_ESP,E-8);
                br.redirect_next(e2+1);
                return c.reg(R_EAX); };   // faithful fallback: EAX UNCHANGED
            br.register_shim("native.hook","scomp_explode2_114",s2);
            cpu->set_alternate(e2,br.shim_trap("native.hook","scomp_explode2_114"));
            std::printf("native scomp2 (1.14d): SCompExplode Game+0x1e000 (alternate, cible indirecte)\n");
        }
    }

    // ---- Storm Huffman + ADPCM served native (NATIVEHUFF / NATIVEADPCM) -----
    //
    // Covers all D2 audio bytes: d2speech is 100% mask 0x41 (Huffman + mono
    // ADPCM), music is mask 0x81 (Huffman + stereo ADPCM); the existing
    // PKWARE port only covers mask 0x08.
    //
    // Not just an optimization: same family of Storm code as PKWARE explode,
    // which corrupts data when its x86 runs through the dynarec on real ARM
    // hardware (invisible under qemu-arm). Quick test if audio crackles:
    // NATIVEHUFF=0 — if the noise disappears with the guest code, it wasn't
    // the dynarec; if it appears, it was.
    //
    // ABI verified by disassembly of Game.exe (0x41f9e0, 0x41e3a0, 0x41e400):
    //   ecx = output buffer; edx = &size (in: capacity, out: bytes produced);
    //   [esp+4] = input buffer; [esp+8] = input size; ret 0xc.
    // The caller IGNORES EAX: SCompDecompress overwrites the register with
    // `mov eax,edi` at 0x41e885, right after the `call eax` at 0x41e87d.
    // These three entries are targets of the .rdata decompressor table at
    // 0x6cfec0, walked from 0x6cfee8 to 0x6cfec8 as (mask, fn) pairs:
    //   0x08 -> 0x41e000 (PKWARE, already ported)   0x02 -> 0x41e340 (zlib)
    //   0x01 -> 0x41f9e0 (Huffman)                   0x80 -> 0x41e400 (ADPCM stereo)
    //   0x40 -> 0x41e3a0 (ADPCM mono)
    // None of these targets is hit by a `call rel32`: the translation-time
    // alternate captures them instead (LinkNext checks hasAlternate on
    // indirect targets resolved at runtime), so NO .text byte is patched —
    // Warden-safe in all modes.
    if(g_114 && !br.module_base("D2gfx.dll")){
        auto off=[&](const char* n){ const char* v=getenv(n); return !(v && !strcmp(v,"0")); };
        const bool wantHuff=off("NATIVEHUFF"), wantAdpcm=off("NATIVEADPCM");
        uint32_t GB=g_d2base;
        // `arm` only gates set_alternate. The trap slot itself is allocated
        // UNCONDITIONALLY: alloc_trap advances by 16 bytes per slot, so
        // gating registration on the knob would shift the numbering of every
        // later slot, making a NATIVEHUFF=0 leg differ from the control leg
        // by more than just the knob. Same rule as the 32 DirectSound slots
        // (ds_emul.h).
        auto poser=[&](const char* tag,uint32_t rva,int kind,bool arm){
            uint32_t entry=GB+rva;
            Shim s; s.argc=0; s.stdcall_cleanup=false; s.tag=tag;
            s.fn=[entry,&br,kind](Cpu&c)->uint32_t{
                uint32_t Rg[8]; c.regs_gp(Rg);
                const uint32_t E=Rg[R_ESP], out=Rg[R_ECX], pcb=Rg[R_EDX];
                const uint32_t in=c.read_u32(E+4), inlen=c.read_u32(E+8);
                const uint32_t cap=pcb?c.read_u32(pcb):0;
                uint32_t n=0;
                // Real bounds check on both ranges (hostptr(va) alone only
                // validates one byte): too short or out of arena => n=0 =>
                // faithful fallback, the guest does the work and the game
                // sees no difference.
                if(cap && inlen>1 && cap<=(8u<<20)){
                    const uint8_t* iH=(const uint8_t*)c.hostptr(in,inlen);
                    uint8_t* oH=(uint8_t*)c.hostptr(out,cap);
                    if(iH && oH) n = (kind==0)
                        ? d2rt::audio_huff_decompress(iH,inlen,oH,cap)
                        : d2rt::audio_adpcm_decompress(iH,inlen,oH,cap,kind==1?1:2);
                }
                if(n){ c.write_u32(pcb,n); ++d2rt_audio_codec_n[kind];
                    c.set_reg(R_ESP,E+12);              // + handler's pop = ret 0xc
                    return n; }
                ++d2rt_audio_codec_n[3];
                c.write_u32(E-4,c.reg(R_EBP));          // emulates the entry 'push ebp'
                c.set_reg(R_ESP,E-8);
                br.redirect_next(entry+1);              // resumes at 'mov ebp,esp'
                return c.reg(R_EAX); };                 // faithful fallback: EAX UNCHANGED
            br.register_shim("native.hook",tag,s);
            const uint32_t tr=br.shim_trap("native.hook",tag);   // ALWAYS: freezes the slot numbering
            if(arm) cpu->set_alternate(entry,tr);
        };
        poser("native!huff_114",0x1f9e0,0,wantHuff);
        poser("native!adpcm_mono_114",0x1e3a0,1,wantAdpcm);
        poser("native!adpcm_stereo_114",0x1e400,2,wantAdpcm);
        std::printf("native audio codecs (1.14d): huffman Game+0x1f9e0=%s, adpcm Game+0x1e3a0/0x1e400=%s"
                    " (alternate, no .text patch)\n", wantHuff?"ON":"off", wantAdpcm?"ON":"off");
    }
}

void native_hooks_codec_install_post(Cpu* cpu, Bridge& br, int* framePtr){
    // ---- Halt-316 room guard (D2_ROOMGUARD=1, experimental) -----------------
    // Root cause: the unit-placement loop (0x466820, iterates a room's
    // [.,+0x5c] node list) can NESTED-trigger the activation of a
    // neighbouring room whose async level stream just completed (room state
    // 2). The state-2 handler (0x6667d0, tail-jmp'd from the 0x66ee40
    // dispatcher) rebuilds the grid and MIGRATES nodes out of the very list
    // being iterated (0x666710: rebase in place + relink onto the
    // destination head) -> the iterator follows a next that now points into
    // another room's chain -> FindRoomForUnit gets a foreign/mid-rebase
    // coordinate -> Fog Halt 316/1420 family. Single-threaded iterator
    // invalidation, latent in D2 (a fast PC resolves state 2 before
    // placement ever runs); the port's slower async loads land it
    // mid-iteration.
    // Guard: while an 0x466820 iteration is on the stack, DEFER the state-2
    // handler by returning immediately — the room stays in state 2 and the
    // lifecycle re-runs the handler at its next touch (pure delay, no
    // semantic change, no .text patch). Faithful fallback outside iterations.
    if(g_114 && !br.module_base("D2gfx.dll") && getenv("D2_ROOMGUARD") && *getenv("D2_ROOMGUARD")=='1'){
        // Halt-316 containment: the unit-placement walk follows a room2 node
        // chain into a node that is DURABLY linked but whose coordinates were
        // never rebased for this list (an order-dependent leftover of D2's
        // room-to-room node migrations; the order depends on async
        // level-stream completion, so a fast PC never produces it). The read
        // is clean single-threaded — the bad node is installed long before.
        // Containment at the READ (D2_TOLERATE pattern): validate the
        // coordinates at the marshaller 0x466730 (CALL-reached; alternates
        // fire there) and SKIP creating that one unit when they are garbage
        // — a missing decoration instead of a fatal Fog Halt. The marshaller
        // returns via plain `ret` (caller's mov esp,ebp cleans up), so the
        // skip is a bare return-0.
        uint32_t GB=g_d2base;
        uint32_t marsh=GB+0x66730;                 // VA 0x466730
        static uint32_t s_marsh=0; s_marsh=marsh;
        Shim sm; sm.argc=0; sm.stdcall_cleanup=false; sm.tag="native!room_coord_guard";
        sm.fn=[&br](Cpu&c)->uint32_t{
            uint32_t Rg[8]; c.regs_gp(Rg);         // ESP+EDX+ESI: one batched read (cpu.h regs_gp)
            uint32_t E=Rg[R_ESP];
            uint32_t nY=c.read_u32(E+4);           // pushed arg
            uint32_t ty=c.read_u32(E+8);           // pushed [node+0x14] (unit type/class)
            uint32_t nX=Rg[R_EDX];
            uint32_t node=Rg[R_ESI];               // the room2 node being read
            static uint32_t okX=0, okY=0, okT=2; static bool okInit=false;
            bool badX=((int32_t)nX<0 || nX>0x7FFF), badY=((int32_t)nY<0 || nY>0x7FFF),
                 badT=((int32_t)ty<0 || ty>0x7FFF);
            if((badX||badY||badT) && okInit){
                // REPAIR at first contact: the node stays linked forever, and
                // every downstream consumer (creator here, room getters, the
                // migrator's rect tests) will assert on it sooner or later —
                // skipping only silences one of them. Clamp the un-rebased
                // fields to the last SANE neighbour of the same walk: one
                // decoration doubled onto its neighbour's tile instead of a
                // fatal Halt, and the node is sane from then on.
                if(badX){ nX=okX; }
                if(badY){ nY=okY; c.write_u32(E+4,nY); }
                if(badT){ ty=okT; c.write_u32(E+8,ty); }
                c.set_reg(R_EDX,nX);
                if(node>=0x00500000u+d2rt::layout_hi() && node<0x12800000u+d2rt::layout_hi()){
                    if(badX) c.write_u32(node+0x8, nX);       // raw field (out[]==0 for decos)
                    if(badY) c.write_u32(node+0x18,nY);
                    if(badT) c.write_u32(node+0x14,ty);
                }
                if(++g_rgSkip<=4){ std::printf("[roomguard] node repaired: nX=%08x nY=%08x ty=%08x node=%08x\n",nX,nY,ty,node);
                    char m[112]; std::snprintf(m,sizeof m,"roomguard: noeud repare (%08x/%08x @%08x)",nX,nY,node);
                    d2vita_progress(m); }
            } else if(!(badX||badY||badT)){ okX=nX; okY=nY; okT=ty; okInit=true; ++g_rgIter; }
            c.write_u32(E-4,Rg[R_EBP]);            // faithful: emulated 'push ebp' + entry+1
            c.set_reg(R_ESP,E-8);
            br.redirect_next(s_marsh+1);
            return Rg[R_EAX]; };                   // faithful fallback: EAX unchanged
        br.register_shim("native.hook","room_coord_guard",sm);
        cpu->set_alternate(marsh,br.shim_trap("native.hook","room_coord_guard"));
        std::printf("room guard v3: coord validation at %08x (alternate, no .text patch)\n",marsh);
        d2vita_progress("roomguard v3: validation coords au marshaller");
    }


    // ---- DCC decoder timing (D2_DCCPROF=1) ---------------------------------
    // The DCC decoder is 0x60bff0 (Codec.cpp), reached by a single call site
    // (0x5ff71e), no function pointer in data, no tail-jmp — so it's
    // alternate-safe. Timed by swapping the return address for a second
    // trap, to measure its real weight in the loading-window profile before
    // considering a native port (the other serious suspect is DRLG).
    if(g_114 && !br.module_base("D2gfx.dll") && getenv("D2_DCCPROF")){
        uint32_t GB=g_d2base;
        uint32_t dec=GB+0x20bff0;                 // VA 0x60bff0
        static uint32_t s_dec=0; s_dec=dec;
        static uint32_t s_exit=0;
        static uint32_t s_ra[8]={0}; static int s_depth=0;
        static uint64_t s_t0[8]={0};
        // exit trap: restores the real return address and accumulates timing
        Shim sx; sx.argc=0; sx.stdcall_cleanup=false; sx.tag="native!dcc_exit";
        sx.fn=[&br](Cpu&c)->uint32_t{
            if(s_depth>0){ --s_depth; g_dccUs += rt_now_us()-s_t0[s_depth]; ++g_dccN;
                br.redirect_next(s_ra[s_depth]); }
            c.set_reg(R_ESP, c.reg(R_ESP)-4);     // the bridge will add +4 back: ESP unchanged
            return c.reg(R_EAX); };               // do NOT overwrite the decoder's return value
        br.register_shim("native.hook","dcc_exit",sx);
        s_exit = br.shim_trap("native.hook","dcc_exit");
        Shim sd; sd.argc=0; sd.stdcall_cleanup=false; sd.tag="native!dcc_enter";
        sd.fn=[&br](Cpu&c)->uint32_t{
            uint32_t E=c.reg(R_ESP);
            if(s_depth<8){ s_ra[s_depth]=c.read_u32(E); s_t0[s_depth]=rt_now_us(); ++s_depth;
                c.write_u32(E, s_exit); }        // the epilogue's `ret 0x1c` will jump to the exit trap
            c.write_u32(E-4,c.reg(R_EBP));       // faithful fallback: push ebp + entry+1
            c.set_reg(R_ESP,E-8);
            br.redirect_next(s_dec+1);
            return c.reg(R_EAX); };              // faithful fallback: EAX unchanged
        br.register_shim("native.hook","dcc_enter",sd);
        cpu->set_alternate(dec,br.shim_trap("native.hook","dcc_enter"));
        d2vita_progress("dccprof: chrono pose sur le decodeur DCC");
        std::printf("dccprof: alternate sur 0x%08x (sortie 0x%08x)\n",dec,s_exit);
    }

    // ---- Native DCC decoder: ON by default, NATIVEDCC=0 to disable ----------
    // Was env-only (default off) until 2026-09-22, even though it had been in
    // the validated game env since 06/09 — the fff1904 "game env becomes the
    // compiled default" pass missed it, and the VPK ships no env.txt, so no
    // player ever ran it. Oracle replayed before flipping the default
    // (tools/oracle_dcc.sh, 4000 frames): image fingerprint identical to the
    // control, 281 decodes served, cross-oracle 281 compares / 0 divergence.
    // Per-thread profiling showed that the frame drops on newly-seen terrain
    // are not caused by Storm itself (sector reads/decrypt: negligible,
    // explode already native) but by D2's DCC decoder (Codec.cpp,
    // 0x60b200-0x60d040), called by callback 0x5ff6e0 on the I/O thread
    // (async sprite loading). Single call site (0x5ff71e), no tail-jmp —
    // alternate-safe.
    // Calling convention (LTCG): ecx = DCC bytes starting at the first
    // requested direction, edx = pool; stack: len, hdr (+0x18 = nframes,
    // +0x1c = direction offset table), pFlags (obj+0x20; output offset table
    // at pFlags+0xc+i*4), startDir, nDirs, pOutSize, hdr+0xa0; ret 0x1c;
    // EAX = buffer allocated by 0x40b430(ecx=pool, edx=size, file, line, 0).
    // The port decodes on the host, then has the GUEST allocate the output
    // buffer (stub: mov ecx/edx; push 0; push 0x561; push file; call
    // [alloc]; jmp [return trap]) and copies the decoded chain into it; it
    // does not take critical section 0x8f03cc (guest decoder global state,
    // untouched by the native path). Any native decoder failure falls back
    // to the faithful guest path.
    // D2_DCCVERIFY=1: cross oracle — decodes natively into a host buffer,
    // lets the guest decode too, and compares both byte-for-byte (chain,
    // offset table, size).
    if(const char* dccEnv = getenv("NATIVEDCC"); g_114 && !br.module_base("D2gfx.dll") && !(dccEnv && !strcmp(dccEnv,"0"))){
        uint32_t GB=g_d2base;
        const uint32_t entry=GB+0x20bff0;               // VA 0x60bff0
        const uint32_t allocVA=GB+0xb430;               // VA 0x40b430
        const uint32_t fileStr=GB+0x2e61c4;             // VA 0x6e61c4 (".\\SRC\\Codec.cpp")
        static const bool verify = getenv("D2_DCCVERIFY")!=nullptr;
        struct Pending { uint32_t key; uint32_t ra, pFlags, start, ndirs, pOut, len; d2rt::DccResult r; bool used; };
        static Pending pend[8]; static uint32_t stubCode[8]={0}, stubData[8]={0};
        static uint32_t trapRet=0, trapVer=0;
        if(getenv("D2_DCCPROF")) d2vita_progress("ATTENTION: NATIVEDCC remplace l'alternate de D2_DCCPROF sur 0x60bff0");
        auto findPend=[](uint32_t key)->Pending*{ for(auto& p:pend) if(p.used && p.key==key) return &p; return nullptr; };
        auto allocPend=[](uint32_t key)->int{ for(int i=0;i<8;i++) if(!pend[i].used){ pend[i].used=true; pend[i].key=key; return i; } return -1; };
        // Allocation return trap (phase 2): ESP = E from entry, [E] = original return address.
        { Shim sr; sr.argc=0; sr.stdcall_cleanup=false; sr.tag="native!dcc_alloc_ret";
          sr.fn=[findPend](Cpu&c)->uint32_t{
            uint32_t E=c.reg(R_ESP); uint32_t buf=c.reg(R_EAX);
            Pending* p=findPend(E);
            if(!p){ d2_crashlog("NATIVEDCC: retour d'allocation sans decodage en attente (ESP=%08x)",E);
                    c.set_reg(R_ESP,E+0x1c); return buf; }
            if(buf){
                // Written segment by segment (not as one block): the bytes
                // between segments are left as the allocator produced them,
                // matching the guest which never touches them either.
                for(auto& b:p->r.blocks) c.write(buf+b.first,p->r.chain.data()+b.first,b.second);
                g_dccNatBytes+=p->r.chain.size(); g_dccNatFrames+=p->r.frames;
                if(++g_dccNatN==1) d2vita_progress("dcc: decodeur natif actif (1er sprite servi)");
            }
            p->used=false;
            c.set_reg(R_ESP,E+0x1c);                     // ret 0x1c; the bridge pops [E]
            return buf; };
          br.register_shim("native.hook","dcc_alloc_ret",sr);
          trapRet=br.shim_trap("native.hook","dcc_alloc_ret"); }
        // Oracle exit trap: the guest just executed `ret 0x1c` (ESP = E+0x20).
        { Shim sv; sv.argc=0; sv.stdcall_cleanup=false; sv.tag="native!dcc_verify_exit";
          sv.fn=[&br,findPend](Cpu&c)->uint32_t{
            uint32_t Ex=c.reg(R_ESP); uint32_t geax=c.reg(R_EAX);
            Pending* p=findPend(Ex-0x20);
            if(p){
                ++g_dccVerN;
                uint32_t gsize=c.read_u32(p->pOut);
                std::vector<uint8_t> got(gsize?gsize:1);
                bool okr = geax && gsize && c.read(geax,got.data(),gsize);
                uint32_t bad=0; int first=-1;
                if(okr){ for(auto& b:p->r.blocks){ if(b.first+b.second>gsize) { ++bad; if(first<0) first=(int)b.first; continue; }
                    for(uint32_t i=b.first;i<b.first+b.second;i++) if(got[i]!=p->r.chain[i]){ if(first<0) first=(int)i; ++bad; } } }
                uint32_t tbad=0;
                for(uint32_t i=0;i<p->ndirs;i++){ uint32_t gt=c.read_u32(p->pFlags+0xc+(p->start+i)*4);
                    if(i<p->r.dir_off.size() && gt!=p->r.dir_off[i]) ++tbad; }
                bool div = !okr || bad || tbad || gsize!=p->r.chain.size();
                if(div && ++g_dccVerBad<=12){
                    char m[220]; std::snprintf(m,sizeof m,"DCCVERIFY: DIVERGENCE #%llu — invite eax=%08x taille=%u natif=%zu ; %u octets (1er +0x%x inv=%02x nat=%02x) ; table %u/%u ; dirs %u..%u",
                        (unsigned long long)g_dccVerN,geax,gsize,p->r.chain.size(),bad,first<0?0:first,
                        (first>=0&&okr)?got[first]:0,(first>=0&&first<(int)p->r.chain.size())?p->r.chain[first]:0,tbad,p->ndirs,p->start,p->start+p->ndirs-1);
                    std::printf("[dccverify] %s\n",m); d2vita_progress(m); }
                if(!(g_dccVerN%100)){ char m[120]; std::snprintf(m,sizeof m,"[dccverify] %llu decodages compares, %llu divergences",
                    (unsigned long long)g_dccVerN,(unsigned long long)g_dccVerBad); std::printf("%s\n",m); d2vita_progress(m); }
                uint32_t ra=p->ra; p->used=false;
                br.redirect_next(ra);
            } else d2_crashlog("NATIVEDCC: sortie d'oracle sans decodage en attente (ESP=%08x)",Ex);
            c.set_reg(R_ESP,Ex-4);                       // the bridge adds 4 back: ESP unchanged
            return geax; };
          br.register_shim("native.hook","dcc_verify_exit",sv);
          trapVer=br.shim_trap("native.hook","dcc_verify_exit"); }
        Shim sd; sd.argc=0; sd.stdcall_cleanup=false; sd.tag="native!dcc_decode_114";
        sd.fn=[entry,allocVA,fileStr,&br,findPend,allocPend](Cpu&c)->uint32_t{
            uint32_t E=c.reg(R_ESP);
            auto fallback=[&](const char* why){
                ++g_dccNatFB;
                if(g_dccNatFB<=8){ char m[120]; std::snprintf(m,sizeof m,"dcc: repli invite #%llu (%s)",(unsigned long long)g_dccNatFB,why);
                    std::printf("[dcc] %s\n",m); d2vita_progress(m); }
                c.write_u32(E-4,c.reg(R_EBP));           // emulated push ebp + entry+1
                c.set_reg(R_ESP,E-8);
                br.redirect_next(entry+1);
                return c.reg(R_EAX); };                  // faithful fallback: EAX unchanged
            uint32_t len=c.read_u32(E+4), hdr=c.read_u32(E+8), pFlags=c.read_u32(E+12);
            uint32_t start=c.read_u32(E+16), ndirs=c.read_u32(E+20), pOut=c.read_u32(E+24);
            uint32_t data=c.reg(R_ECX), pool=c.reg(R_EDX);
            uint32_t nframes=c.read_u32(hdr+0x18);
            if(!len || len>0x2000000 || !ndirs || ndirs>64 || !nframes || nframes>4096 || start>64) return fallback("arguments");
            if(findPend(E)) return fallback("reentrance");
            int pi=allocPend(E); if(pi<0) return fallback("table pleine");
            Pending& p=pend[pi];
            p.ra=c.read_u32(E); p.pFlags=pFlags; p.start=start; p.ndirs=ndirs; p.pOut=pOut; p.len=len;
            std::vector<uint8_t> in(len);
            if(!c.read(data,in.data(),len)){ p.used=false; return fallback("lecture"); }
            int rc=d2rt::dcc_decode_native(in.data(),len,nframes,ndirs,p.r);
            if(rc){ p.used=false; return fallback(d2rt::dcc_error_name(rc)); }
            // output offset table and total size, mirroring 0x60c087..0x60c0ed:
            // OutSizeCoded is read at T[i]-T[start] in the data (table hdr+0x1c).
            uint32_t total=0; bool tableOk=true;
            for(uint32_t i=0;i<ndirs;i++){
                uint32_t rel=c.read_u32(hdr+0x1c+(start+i)*4)-c.read_u32(hdr+0x1c+start*4);
                uint32_t osz=(rel+4<=len)?c.read_u32(data+rel):0;
                if(total!=p.r.dir_off[i] || osz!=p.r.dir_size[i]) tableOk=false;
                total+=osz;
            }
            if(!tableOk || total!=p.r.chain.size()){ p.used=false; return fallback("table/offsets"); }
            if(verify){
                // the guest decodes; we compare at the exit trap, installed in place of the return address
                c.write_u32(E,trapVer);
                c.write_u32(E-4,c.reg(R_EBP));
                c.set_reg(R_ESP,E-8);
                br.redirect_next(entry+1);
                return c.reg(R_EAX);
            }
            for(uint32_t i=0;i<ndirs;i++) c.write_u32(pFlags+0xc+(start+i)*4,p.r.dir_off[i]);
            c.write_u32(pOut,total);
            // guest allocation stub (per-slot data, code written once)
            if(!stubCode[pi]){
                uint32_t dd=misc(0x10); stubData[pi]=dd;
                uint32_t st=misc(0x30); stubCode[pi]=st;
                if(!dd || !st){ p.used=false; return fallback("misc"); }
                std::vector<uint8_t> b;
                auto imm=[&](uint32_t a){ for(int k=0;k<4;k++) b.push_back((uint8_t)((a>>(8*k))&0xff)); };
                b.push_back(0x8B); b.push_back(0x0D); imm(dd+0);          // mov ecx,[dd+0]  (pool)
                b.push_back(0x8B); b.push_back(0x15); imm(dd+4);          // mov edx,[dd+4]  (size)
                b.push_back(0x6A); b.push_back(0x00);                     // push 0
                b.push_back(0x68); imm(0x561);                            // push 0x561 (line)
                b.push_back(0x68); imm(fileStr);                          // push file
                b.push_back(0xFF); b.push_back(0x15); imm(dd+8);          // call [dd+8]  (0x40b430, ret 0xc)
                b.push_back(0xFF); b.push_back(0x25); imm(dd+12);         // jmp  [dd+12] (return trap)
                c.write(st,b.data(),(uint32_t)b.size());
            }
            uint32_t dd=stubData[pi];
            c.write_u32(dd+0,pool); c.write_u32(dd+4,total); c.write_u32(dd+8,allocVA); c.write_u32(dd+12,trapRet);
            c.set_reg(R_ESP,E-4);                        // the bridge adds 4 back: ESP = E, [E] = original return address
            br.redirect_next(stubCode[pi]);
            return c.reg(R_EAX); };
        br.register_shim("native.hook","dcc_decode_114",sd);
        cpu->set_alternate(entry,br.shim_trap("native.hook","dcc_decode_114"));
        std::printf("native dcc: decodeur DCC Game+0x20bff0 (alternate%s)\n",verify?", oracle D2_DCCVERIFY":"");
        d2vita_progress(verify?"dcc: oracle croise D2_DCCVERIFY arme sur 0x60bff0":"dcc: decodeur natif arme sur 0x60bff0 (NATIVEDCC)");
    }

    // ---- CRT memcpy/memset served with NO TRAP: D2_MEMINTRIN ----------------
    // DEFAULT = 3 since 2026-09-22 (D2_MEMINTRIN=0 disables). Console, patrol
    // bench on the real save, 3 controls dispersed by 0.33 %: 20.97 -> 21.93
    // frames/s (+4.6 %), the largest single gain of the campaign. Arming proven
    // on hardware: helper calls 24 250 123 -> 4 248 (99.85 % of them served by
    // the emitted sequence, with no call at all). Mode 1 alone gives +2.8 %, so
    // avoiding the crossing is worth 1.8 points on its own — the dominant size
    // bucket is 32-63 bytes, a regime where a fixed call cost is ruinous
    // (~205 ns per call of pure plumbing, measured by mode 2).
    // Oracle: tools/oracle_memintrin.sh PASS — 5 identical frame fingerprints,
    // cross-oracle 4 816 757 calls / 0 divergence.
    // Contract, metrics and guards: src/dynarec86/dyn86_memintrin.h.
    //   D2_MEMINTRIN=1  serve natively
    //   D2_MEMINTRIN=2  PROFILE ONLY: count calls/sizes/alignments, then let
    //                   the guest do the work (no state change)
    //   D2_MEMINTRIN=3  mode 1 PLUS the INLINE SHORT PATH: the translator
    //                   emits the copy itself for sizes below 64 bytes, with
    //                   no call at all (dyn86_memfast.h). 99.85% of memcpy
    //                   calls and 83.7% of memset calls are that short.
    //   D2_MEMFASTCHECK=1  arms the oracle OF THE INLINE PATH (guards + byte
    //                   for byte comparison at each served call)
    //   D2_MEMINTRIN_MAX=<hex> size cap for the native path (default 0x1000000)
    // Recognized AT TRANSLATION TIME (no alternate, no trap): the block that
    // STARTS at the function entry calls a native C helper. 186 direct call
    // sites for memcpy, 1135 for memset, no incoming jmp, no data reference
    // (verified by disassembly of Game.exe 1.14d): the block-start address
    // catches them all.
    if(g_114 && !br.module_base("D2gfx.dll")){
        const char* mk = getenv("D2_MEMINTRIN");
        int mode = (mk && mk[0]) ? atoi(mk) : DYN86_MI_MODE_FAST;
        if(mode==1 || mode==2 || mode==DYN86_MI_MODE_FAST){
            if(const char* mx = getenv("D2_MEMINTRIN_MAX")){
                unsigned long v = strtoul(mx,nullptr,16);
                if(v >= 0x100 && v <= 0x40000000ul) dyn86_mi_maxn = (uint32_t)v; }
            const uint32_t cpyVA = g_d2base + 0x2829c0;   // VA 0x6829c0 memcpy/memmove
            const uint32_t setVA = g_d2base + 0x281ef0;   // VA 0x681ef0 memset
            // Fingerprint check: refuse to arm if the entry bytes aren't the
            // expected 1.14d ones (a different binary means a different
            // function).
            static const uint8_t sigC[8]={0x55,0x8b,0xec,0x57,0x56,0x8b,0x75,0x0c};
            static const uint8_t sigS[8]={0x8b,0x54,0x24,0x0c,0x8b,0x4c,0x24,0x04};
            uint8_t gotC[8]={0}, gotS[8]={0};
            cpu->read(cpyVA,gotC,8); cpu->read(setVA,gotS,8);
            bool okC = !memcmp(gotC,sigC,8), okS = !memcmp(gotS,sigS,8);
            if(!okC && !okS){
                std::printf("memintrin: REFUSE — empreintes d'entree inconnues "
                            "(cpy %02x%02x%02x%02x, set %02x%02x%02x%02x)\n",
                            gotC[0],gotC[1],gotC[2],gotC[3],gotS[0],gotS[1],gotS[2],gotS[3]);
                d2vita_progress("memintrin: REFUSE (empreintes d'entree inconnues)");
            } else {
                // D2_MEMVERIFY cross oracle: a RETURN trap (same pattern as
                // d2_light_verify_exit). The helper replaces the return
                // address on the guest stack with this trap and falls back;
                // the guest copies with its own translated code; here we
                // compare, then resume at the original address.
                if((mode==1 || mode==DYN86_MI_MODE_FAST) && getenv("D2_MEMVERIFY")){
                    Shim mv; mv.argc=0; mv.stdcall_cleanup=false; mv.tag="native!memintrin_verify_exit";
                    mv.fn=[&br](Cpu&c)->uint32_t{
                        uint32_t geax=c.reg(R_EAX);
                        uint32_t ra=dyn86_mi_verify_ret();
                        if(ra) br.redirect_next(ra);
                        else   d2_crashlog("memverify: retour d'oracle sans comparaison en attente");
                        c.set_reg(R_ESP,c.reg(R_ESP)-4);   // the bridge will pop: we compensate for it
                        return geax; };
                    br.register_shim("native.hook","memintrin_verify_exit",mv);
                    dyn86_mi_arm_verify(br.shim_trap("native.hook","memintrin_verify_exit"));
                    std::printf("memintrin: oracle croise D2_MEMVERIFY arme (trap 0x%08x)\n",
                                (unsigned)dyn86_mi_verify_trap);
                }
                // D2_MEMINTRIN_ONLY=cpy|set: arm only one of the two
                // functions, so a measurement difference can be bisected
                // between them instead of memcpy+memset being one
                // indivisible number.
                const char* only = getenv("D2_MEMINTRIN_ONLY");
                if(only && !strcmp(only,"cpy")) okS=false;
                if(only && !strcmp(only,"set")) okC=false;
                dyn86_mi_set_sse2va(g_d2base + 0x594c88);   // ds:0x994c88 = CRT SSE2 flag
                // Read BEFORE dyn86_mi_arm(): the translator reads
                // dyn86_mi_fastchk when it emits, and arm() is what freezes
                // the whole set of translation-time flags.
                if(getenv("D2_MEMFASTCHECK")) dyn86_mi_fastchk = 1;
                dyn86_mi_arm(mode, okC?cpyVA:0, okS?setVA:0);
                std::printf("memintrin: mode=%d memcpy=%s memset=%s maxn=0x%x span=0x%x enligne=%d oracle-enligne=%d\n",
                            mode, okC?"arme":"ETEINT", okS?"arme":"ETEINT",
                            (unsigned)dyn86_mi_maxn, (unsigned)dyn86_mi_span,
                            dyn86_mi_fast, dyn86_mi_fastchk);
                d2vita_progress(mode==2 ? "memintrin: PROFIL SEUL (D2_MEMINTRIN=2)"
                              : (dyn86_mi_fast ? "memintrin: memcpy/memset natifs + chemin court EN LIGNE (D2_MEMINTRIN=3)"
                                               : "memintrin: memcpy/memset natifs armes (D2_MEMINTRIN=1)"));
            }
        }
    }

    // ---- D2Vita: native intrinsics (D2_INTRIN) -------------------------------
    // Generic mechanism: src/dynarec86/dyn86_intrin.h. Game-specific data
    // (which function, which ABI, which arithmetic): src/runtime/d2_intrin_114.cpp.
    //
    // Target 1: Game+0x10dd60 (VA 0x50dd60), world-to-screen projection --
    // the hottest function in the game, called several thousand times per
    // frame. A trap-based approach isn't viable here (the trap would cost
    // more than the function body); this mechanism uses no trap at all.
    if(g_114 && !br.module_base("D2gfx.dll")){
        const char* ik = getenv("D2_INTRIN");
        if(ik && ik[0] && strcmp(ik,"0")){
            const uint32_t projVA = g_d2base + 0x10dd60;
            // Entry fingerprint: refuse to arm if the bytes don't match the
            // expected 1.14d ones (a different binary means a different
            // function). Same guard as memintrin -- it keeps native code
            // from running against a function that isn't the one that was
            // disassembled.
            //   55 8b ec 2b 0d <abs32>   push ebp; mov ebp,esp; sub ecx,[0x880b60]
            // Only the first 5 bytes are constant: the absolute operand that
            // follows is RELOCATED by the PE loader (Game.exe is not loaded
            // at 0x400000 under D2LAYOUT=compact), so hardcoding it would
            // always refuse to arm. Checking the 5 opcode bytes AND the
            // relocated value is stronger than an 8-byte fingerprint: it also
            // proves the relocation is the expected one, so the global VAs
            // computed by d2_intrin_114_register are correct too.
            static const uint8_t sigP[5]={0x55,0x8b,0xec,0x2b,0x0d};
            uint8_t gotP[9]={0}; cpu->read(projVA,gotP,9);
            uint32_t absOp = (uint32_t)gotP[5] | ((uint32_t)gotP[6]<<8)
                           | ((uint32_t)gotP[7]<<16) | ((uint32_t)gotP[8]<<24);
            const uint32_t wantOp = g_d2base + 0x480b60;
            if(memcmp(gotP,sigP,5) || absOp != wantOp){
                char m[192]; std::snprintf(m,sizeof m,
                    "intrin: REFUSE — 0x50dd60 inattendu (octets %02x%02x%02x%02x%02x, "
                    "abs=0x%08x attendu 0x%08x)",
                    gotP[0],gotP[1],gotP[2],gotP[3],gotP[4],
                    (unsigned)absOp,(unsigned)wantOp);
                std::printf("%s\n",m); d2vita_progress(m);
            } else {
                if(getenv("D2_PROJVERIFY")){
                    // Cross oracle: a RETURN trap (same pattern as
                    // D2_MEMVERIFY). The helper replaces the return address
                    // on the guest stack with this trap and falls back; the
                    // guest computes with its own code; at the trap we
                    // compare BEFORE the caller can touch its locals, then
                    // resume at the original address.
                    d2_proj_verify = 1;
                    Shim pv; pv.argc=0; pv.stdcall_cleanup=false;
                    pv.tag="native!proj_verify_exit";
                    pv.fn=[&br](Cpu& c)->uint32_t{
                        uint32_t geax=c.reg(R_EAX);
                        uint32_t ra=d2_proj_verify_ret();
                        if(ra) br.redirect_next(ra);
                        else   d2_crashlog("projverify: retour d'oracle sans comparaison en attente");
                        c.set_reg(R_ESP,c.reg(R_ESP)-4);   // the bridge will pop: we compensate for it
                        return geax; };
                    br.register_shim("native.hook","proj_verify_exit",pv);
                    d2_proj_verify_trap = br.shim_trap("native.hook","proj_verify_exit");
                }
                if(d2_intrin_114_register(g_d2base)){
                    int im = atoi(ik); if(im != 2) im = 1;
                    dyn86_intrin_on = im;
                    char m[160]; std::snprintf(m,sizeof m,
                        "intrin: ARME mode=%d n=%d (proj 0x50dd60)%s", dyn86_intrin_on, dyn86_intrin_n,
                        d2_proj_verify ? " + ORACLE D2_PROJVERIFY (ne sert PAS)" : "");
                    std::printf("%s\n",m); d2vita_progress(m);
                } else {
                    d2vita_progress("intrin: REFUSE — enregistrement impossible");
                }
            }
        }
    }

    // ---- Fog raise snapshot (D2_RAISESNAP=1; off by default) ---------------
    // Costly: copies 0x48 bytes of guest memory dword by dword and pays a
    // full trap round-trip on EVERY Fog raise call, so it must be armed
    // explicitly rather than left on.
    // Names the sprite-family culprit: at the raise entry (VA 0x6001f0,
    // CALL-reached from 0x4dbda3 & co), [esp]=retaddr identifies the raise
    // SITE and [esp+4]=&record holds the failing cel/component identity
    // (rec+0 = caller's [ebp+0x14] arg, rec+5 = component byte). Zero I/O
    // here; the ring is dumped at Crash.txt.
    if(g_114 && !br.module_base("D2gfx.dll") && getenv("D2_RAISESNAP")){
        uint32_t GB=g_d2base;
        uint32_t raise=GB+0x2001f0;                // VA 0x6001f0 (push ebp entry)
        static uint32_t s_raise=0; s_raise=raise;
        Shim rr; rr.argc=0; rr.stdcall_cleanup=false; rr.tag="native!fog_raise_snap";
        rr.fn=[&br,framePtr](Cpu&c)->uint32_t{
            uint32_t E=c.reg(R_ESP);
            RaiseRec& r=g_raiseRing[g_raiseN++&7];
            r.ra=c.read_u32(E); r.rec=c.read_u32(E+4);
            r.esi=c.reg(R_ESI); r.edi=c.reg(R_EDI); r.frame=(uint32_t)(*framePtr);
            for(int i=0;i<0x48;i+=4){ uint32_t v=(r.rec>=0x00500000u+d2rt::layout_hi()&&r.rec<0x12800000u+d2rt::layout_hi())?c.read_u32(r.rec+i):0;
                std::memcpy(r.body+i,&v,4); }
            c.write_u32(E-4,c.reg(R_EBP));         // faithful: emulated 'push ebp' + entry+1
            c.set_reg(R_ESP,E-8);
            br.redirect_next(s_raise+1);
            return c.reg(R_EAX); };              // faithful fallback: EAX unchanged
        br.register_shim("native.hook","fog_raise_snap",rr);
        cpu->set_alternate(raise,br.shim_trap("native.hook","fog_raise_snap"));
        std::printf("fog raise snapshot: alternate at %08x\n",raise);
    }
}

