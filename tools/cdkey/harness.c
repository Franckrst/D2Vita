// Wine harness: maps Game.exe 1.14d at 0x400000 and calls its CD-key file
// encrypt/decrypt routines directly (no network, dummy keys).
//   harness.exe enc <hex-plaintext>      -> hex blob (0x523060, password 0x522bc0)
//   harness.exe dec <hex-blob>           -> "ok=<0|1> len=<n> hex=<plaintext>" (0x5232b0)
//   harness.exe pw                       -> hex dump of the 20-byte password
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long g_seed;
DWORD g_5232b0,g_522e10,g_522bc0,g_523060; static DWORD g_delta;
#define VA(x) ((DWORD)(x)+g_delta)
void my_srand(unsigned s){ g_seed=s; }
int  my_rand(void){ g_seed=g_seed*0x343fd+0x269ec3; return (g_seed>>16)&0x7fff; }
long my_time(long* t){ if(t)*t=0x12345678; return 0x12345678; }

static void jmp_to(DWORD at, void* fn){
    BYTE* p=(BYTE*)at; p[0]=0xE9; DWORD rel=(DWORD)fn-(at+5); memcpy(p+1,&rel,4);
}
static int hexval(char c){ return c<='9'?c-'0':(c|0x20)-'a'+10; }
static int unhex(const char* s, BYTE* out){ int n=0; while(s[0]&&s[1]){ out[n++]=(BYTE)(hexval(s[0])*16+hexval(s[1])); s+=2;} return n; }

// register-argument thunks
// int call_5232b0(BYTE* buf, DWORD size)   ecx=buf edi=size
int call_5232b0(BYTE* buf, DWORD size);
__asm__(".globl _call_5232b0\n_call_5232b0:\n push %ebp\n mov %esp,%ebp\n push %edi\n push %esi\n push %ebx\n"
        " mov 8(%ebp),%ecx\n mov 12(%ebp),%edi\n call *_g_5232b0\n pop %ebx\n pop %esi\n pop %edi\n pop %ebp\n ret\n");
// int call_522e10(BYTE* buf, DWORD size, char* pw)  ecx=buf edx=size push pw ; ret 4
int call_522e10(BYTE* buf, DWORD size, char* pw);
__asm__(".globl _call_522e10\n_call_522e10:\n push %ebp\n mov %esp,%ebp\n push %edi\n push %esi\n push %ebx\n"
        " mov 8(%ebp),%ecx\n mov 12(%ebp),%edx\n push 16(%ebp)\n call *_g_522e10\n pop %ebx\n pop %esi\n pop %edi\n pop %ebp\n ret\n");
// void call_522bc0(char* out, DWORD n)   ecx=out edx=n
void call_522bc0(char* out, DWORD n);
__asm__(".globl _call_522bc0\n_call_522bc0:\n push %ebp\n mov %esp,%ebp\n push %edi\n push %esi\n push %ebx\n"
        " mov 8(%ebp),%ecx\n mov 12(%ebp),%edx\n call *_g_522bc0\n pop %ebx\n pop %esi\n pop %edi\n pop %ebp\n ret\n");
// void call_523060(BYTE* buf, DWORD len, DWORD bufsize, char* pw)  ecx=buf edx=len ; push pw ; push bufsize ; ret 8
void call_523060(BYTE* buf, DWORD len, DWORD bufsize, char* pw);
__asm__(".globl _call_523060\n_call_523060:\n push %ebp\n mov %esp,%ebp\n push %edi\n push %esi\n push %ebx\n"
        " mov 8(%ebp),%ecx\n mov 12(%ebp),%edx\n push 20(%ebp)\n push 16(%ebp)\n call *_g_523060\n pop %ebx\n pop %esi\n pop %edi\n pop %ebp\n ret\n");

int main(int argc, char** argv){
    FILE* f=fopen("Z:\\home\\doudou\\d2-vita-refs\\1.14d\\Game.exe","rb");
    if(!f){ printf("open Game.exe KO\n"); return 1; }
    static BYTE file[4*1024*1024]; size_t fl=fread(file,1,sizeof file,f); fclose(f);
    IMAGE_DOS_HEADER* dh=(IMAGE_DOS_HEADER*)file; IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(file+dh->e_lfanew);
    DWORD sz=nt->OptionalHeader.SizeOfImage;
    BYTE* base=VirtualAlloc(NULL, sz, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if(!base){ printf("VirtualAlloc KO\n"); return 1; }
    g_delta=(DWORD)base-nt->OptionalHeader.ImageBase;
    memcpy(base,file,nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sh=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++){
        DWORD n = sh[i].SizeOfRawData < sh[i].Misc.VirtualSize ? sh[i].SizeOfRawData : sh[i].Misc.VirtualSize;
        if(sh[i].PointerToRawData+n<=fl) memcpy(base+sh[i].VirtualAddress,file+sh[i].PointerToRawData,n);
    }
    {   IMAGE_DATA_DIRECTORY* rd=&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        BYTE* r=base+rd->VirtualAddress, *e=r+rd->Size; int cnt=0;
        while(r<e){ IMAGE_BASE_RELOCATION* b=(IMAGE_BASE_RELOCATION*)r; if(!b->SizeOfBlock) break;
            WORD* w=(WORD*)(r+8); int k=(b->SizeOfBlock-8)/2;
            for(int j=0;j<k;j++) if((w[j]>>12)==3){ DWORD* t=(DWORD*)(base+b->VirtualAddress+(w[j]&0xfff)); *t+=g_delta; cnt++; }
            r+=b->SizeOfBlock; }
        fprintf(stderr,"base=%p relocs=%d\n",base,cnt); }
    g_5232b0=VA(0x5232b0); g_522e10=VA(0x522e10); g_522bc0=VA(0x522bc0); g_523060=VA(0x523060);
    (void)fl;

    // srand/rand/time go through the MSVC CRT's _getptd (TLS not set up here),
    // so replace them with exact cdecl equivalents of the MS LCG.
    jmp_to(VA(0x687454),(void*)my_srand);   // jmp: [esp+4] = seed, return path untouched
    jmp_to(VA(0x687461),(void*)my_rand);
    jmp_to(VA(0x6850e6),(void*)my_time);

    if(argc>=2 && !strcmp(argv[1],"pw")){
        char pw[32]; call_522bc0(pw,0x14);
        for(int i=0;i<0x14;i++) printf("%02x",(BYTE)pw[i]); printf("\n"); return 0;
    }
    if(argc>=3 && !strcmp(argv[1],"enc")){
        static BYTE buf[4096]; int n=unhex(argv[2],buf);
        DWORD bs = ((n&0x3f)? n+(0x40-(n&0x3f)) : n) + 8;   // = 0x523020(n)
        char pw[32]; call_522bc0(pw,0x14);
        call_523060(buf,n,bs,pw);
        for(DWORD i=0;i<bs;i++) printf("%02x",buf[i]); printf("\n"); return 0;
    }
    if(argc>=3 && !strcmp(argv[1],"dec")){
        static BYTE buf[4096]; int n=unhex(argv[2],buf);
        static BYTE b2[4096]; memcpy(b2,buf,n);
        int ok=call_5232b0(buf,n);
        char pw[32]; call_522bc0(pw,0x14);
        int len=call_522e10(b2,n,pw);
        printf("ok=%d len=%d hex=",ok,len);
        for(int i=0;i<len && i<n;i++) printf("%02x",b2[i]); printf("\n"); return 0;
    }
    printf("usage\n"); return 2;
}
