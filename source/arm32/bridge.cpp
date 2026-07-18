// Import bridge: a guest call that lands on a sentinel PC runs the native arm64
// shim it stands for. Each wrapper marshals the AArch32 AAPCS (r0-r3 + stack)
// into a real call, translating guest pointers to host pointers. Unhandled
// imports are logged (this is how the set grows, log-driven).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <strings.h>
#include <vector>
#include <string>

namespace a32 {

struct Import { std::string name; };
static std::vector<Import> s_imports;

// Guest FILE* handle table: a guest "FILE*" is (index into s_files)+1, so 0 is
// a clean NULL. gfile() maps a handle back to the host FILE*.
static std::vector<FILE*> s_files;
static FILE* gfile(uint32_t h) { return (h && h <= s_files.size()) ? s_files[h-1] : nullptr; }

uint32_t bridgeRegister(const char* name) {
    for (size_t i = 0; i < s_imports.size(); i++)
        if (s_imports[i].name == name) return A32_SENTINEL_BASE + (uint32_t)i * 4;
    s_imports.push_back({name ? name : "?"});
    return A32_SENTINEL_BASE + (uint32_t)(s_imports.size() - 1) * 4;
}

// Convenience accessors over the guest CPU state / stack.
static inline uint32_t arg(CpuState& c, int i) {
    if (i < 4) return c.r[i];
    return *(uint32_t*)toHost(c.r[13] + (uint32_t)(i - 4) * 4);   // stacked args
}
static inline char*  hstr(uint32_t g) { return g ? (char*)toHost(g) : nullptr; }
static inline void*  hptr(uint32_t g) { return g ? (void*)toHost(g) : nullptr; }

// strlen bounded to the region — never scans past the guest window.
static uint32_t gStrlen(uint32_t g) {
    if (!g || g >= g_region) return 0;
    const char* p = (const char*)toHost(g);
    uint32_t max = (uint32_t)(g_region - g), n = 0;
    while (n < max && p[n]) n++;
    return n;
}
// Bounded copy: at most `n` bytes, and never past the region for either side.
static void gStrcpy(uint32_t d, uint32_t s, uint32_t n) {
    if (!d || !s) return;
    if (!guestValid(d, n) || !guestValid(s, n)) {
        uint32_t dmax = d < g_region ? g_region - d : 0;
        uint32_t smax = s < g_region ? g_region - s : 0;
        n = n < dmax ? n : dmax; n = n < smax ? n : smax;
    }
    if (n) memcpy(toHost(d), toHost(s), n);
}

// Returns true if it handled `name`; sets ret (r0) via out.
static bool dispatch(CpuState& c, const char* name, uint32_t& ret) {
    // ── allocator: must stay inside the guest region and return guest addrs ──
    if (!strcmp(name, "malloc"))  { ret = guestAlloc(arg(c,0)); return true; }
    if (!strcmp(name, "free"))    { guestFree(arg(c,0)); ret = 0; return true; }
    if (!strcmp(name, "calloc"))  { uint32_t n = arg(c,0)*arg(c,1); uint32_t p = guestAlloc(n); if (p) memset(toHost(p),0,n); ret = p; return true; }
    if (!strcmp(name, "realloc")) { ret = guestRealloc(arg(c,0), arg(c,1)); return true; }
    if (!strcmp(name, "memalign") || !strcmp(name, "aligned_alloc")) { ret = guestAlloc(arg(c,1)); return true; }
    if (!strcmp(name, "posix_memalign")) { uint32_t p = guestAlloc(arg(c,2)); if (arg(c,0)) *(uint32_t*)toHost(arg(c,0)) = p; ret = p ? 0 : 12; return true; }

    // ── mem/str: translate pointer args, return values may be guest pointers.
    //    Bounds-check every length against the guest region so a bad pointer or
    //    size can never write into the Core's own host memory. ──
    if (!strcmp(name, "memcpy") || !strcmp(name, "memmove")) {
        uint32_t d=arg(c,0), s=arg(c,1), n=arg(c,2);
        if (guestValid(d,n) && guestValid(s,n)) memmove(hptr(d), hptr(s), n);
        else compatLogFmt("arm32: %s OOB d=0x%x s=0x%x n=0x%x — skipped", name, d, s, n);
        ret = d; return true;
    }
    if (!strcmp(name, "memset")) {
        uint32_t d=arg(c,0), n=arg(c,2);
        if (guestValid(d,n)) memset(hptr(d), (int)arg(c,1), n);
        else compatLogFmt("arm32: memset OOB d=0x%x n=0x%x — skipped", d, n);
        ret = d; return true;
    }
    if (!strcmp(name, "memcmp")) {
        uint32_t a=arg(c,0), b=arg(c,1), n=arg(c,2);
        ret = (guestValid(a,n) && guestValid(b,n)) ? (uint32_t)memcmp(hptr(a), hptr(b), n) : 0;
        return true;
    }
    // String ops are bounded to the region so an unterminated guest string can't
    // walk off into host memory (gStrlen caps its scan at the region end).
    if (!strcmp(name, "strlen"))  { ret = gStrlen(arg(c,0)); return true; }
    if (!strcmp(name, "strcmp") || !strcmp(name, "strncmp")) {
        uint32_t a=arg(c,0), b=arg(c,1);
        uint32_t n = name[3]=='n' ? arg(c,2) : gStrlen(a)+1;
        if (!a || !b) ret = (a==b) ? 0 : 1;
        else ret = (uint32_t)strncmp(hstr(a), hstr(b), n);
        return true;
    }
    if (!strcmp(name, "strcpy"))  { gStrcpy(arg(c,0), arg(c,1), gStrlen(arg(c,1))+1); ret = arg(c,0); return true; }
    if (!strcmp(name, "strncpy")) { gStrcpy(arg(c,0), arg(c,1), arg(c,2)); ret = arg(c,0); return true; }
    if (!strcmp(name, "strcat"))  { uint32_t d=arg(c,0); gStrcpy(d + gStrlen(d), arg(c,1), gStrlen(arg(c,1))+1); ret = d; return true; }
    if (!strcmp(name, "strchr"))  { uint32_t L=gStrlen(arg(c,0)); void* p = arg(c,0)?memchr(hptr(arg(c,0)), (int)arg(c,1), L+1):nullptr; ret = p ? toGuest(p) : 0; return true; }

    // ── C++ static-init / exit registration: safe no-ops here ──
    if (!strcmp(name, "__cxa_atexit") || !strcmp(name, "__cxa_finalize") ||
        !strcmp(name, "__register_atfork") || !strcmp(name, "atexit")) { ret = 0; return true; }
    if (!strcmp(name, "__cxa_guard_acquire")) { uint8_t* g = (uint8_t*)hptr(arg(c,0)); ret = g && *g ? 0 : 1; return true; }
    if (!strcmp(name, "__cxa_guard_release")) { uint8_t* g = (uint8_t*)hptr(arg(c,0)); if (g) *g = 1; ret = 0; return true; }

    // ── more string / mem (all bounded to the region) ──
    if (!strcmp(name,"strnlen")) { uint32_t L=gStrlen(arg(c,0)), n=arg(c,1); ret = L<n?L:n; return true; }
    if (!strcmp(name,"strrchr")) { uint32_t s=arg(c,0),L=gStrlen(s),f=0; uint8_t ch=(uint8_t)arg(c,1);
        for(uint32_t i=0;i<L;i++) if(((uint8_t*)hptr(s))[i]==ch) f=s+i; ret=f; return true; }
    if (!strcmp(name,"memchr")) { uint32_t s=arg(c,0),n=arg(c,2); void* p=(s&&guestValid(s,n))?memchr(hptr(s),(int)arg(c,1),n):nullptr; ret=p?toGuest(p):0; return true; }
    if (!strcmp(name,"strstr")) { uint32_t h=arg(c,0),ne=arg(c,1); if(!h||!ne){ret=0;return true;} gStrlen(h); gStrlen(ne); char* p=strstr(hstr(h),hstr(ne)); ret=p?toGuest(p):0; return true; }
    if (!strcmp(name,"strcasecmp")||!strcmp(name,"strncasecmp")) {
        uint32_t a=arg(c,0),b=arg(c,1); uint32_t n=name[6]=='n'?arg(c,2):(gStrlen(a)+1);
        ret=(!a||!b)?(a==b?0:1):(uint32_t)strncasecmp(hstr(a),hstr(b),n); return true; }
    if (!strcmp(name,"strdup")) { uint32_t s=arg(c,0),L=gStrlen(s),p=guestAlloc(L+1);
        if(p){ if(L) memcpy(hptr(p),hptr(s),L); ((char*)hptr(p))[L]=0; } ret=p; return true; }
    if (!strcmp(name,"strncat")) { uint32_t d=arg(c,0),s=arg(c,1),n=arg(c,2),sl=gStrlen(s),dl=gStrlen(d);
        uint32_t cp=sl<n?sl:n; gStrcpy(d+dl,s,cp); if(guestValid(d+dl+cp,1)) ((char*)hptr(d+dl+cp))[0]=0; ret=d; return true; }

    // ── ctype / conversion ──
    if (!strcmp(name,"toupper")) { ret=(uint32_t)toupper((int)arg(c,0)); return true; }
    if (!strcmp(name,"tolower")) { ret=(uint32_t)tolower((int)arg(c,0)); return true; }
    if (!strcmp(name,"isalpha")) { ret=isalpha((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"isdigit")) { ret=isdigit((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"isspace")) { ret=isspace((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"atoi"))    { { char* s=hstr(arg(c,0)); ret=(uint32_t)atoi(s?s:""); } return true; }
    if (!strcmp(name,"strtol")||!strcmp(name,"strtoul")) {
        char* s=hstr(arg(c,0)); ret = s?(uint32_t)strtoul(s,nullptr,(int)arg(c,2)):0; return true; }

    // ── math: softfp ABI — double args in r0:r1 (and r2:r3), returned in r0:r1 ──
    {
        auto argD=[&](int r)->double{ uint64_t b=(uint64_t)arg(c,r)|((uint64_t)arg(c,r+1)<<32); double d; memcpy(&d,&b,8); return d; };
        auto retD=[&](double d){ uint64_t b; memcpy(&b,&d,8); ret=(uint32_t)b; c.r[1]=(uint32_t)(b>>32); };
        auto argF=[&](int r)->float{ uint32_t u=arg(c,r); float f; memcpy(&f,&u,4); return f; };
        auto retF=[&](float f){ uint32_t u; memcpy(&u,&f,4); ret=u; };
        double x=argD(0);
        if (!strcmp(name,"sqrt")) { retD(sqrt(x)); return true; }
        if (!strcmp(name,"sin"))  { retD(sin(x)); return true; }
        if (!strcmp(name,"cos"))  { retD(cos(x)); return true; }
        if (!strcmp(name,"tan"))  { retD(tan(x)); return true; }
        if (!strcmp(name,"asin")) { retD(asin(x)); return true; }
        if (!strcmp(name,"acos")) { retD(acos(x)); return true; }
        if (!strcmp(name,"atan")) { retD(atan(x)); return true; }
        if (!strcmp(name,"exp"))  { retD(exp(x)); return true; }
        if (!strcmp(name,"log"))  { retD(log(x)); return true; }
        if (!strcmp(name,"log10")){ retD(log10(x)); return true; }
        if (!strcmp(name,"floor")){ retD(floor(x)); return true; }
        if (!strcmp(name,"ceil")) { retD(ceil(x)); return true; }
        if (!strcmp(name,"round")){ retD(round(x)); return true; }
        if (!strcmp(name,"fabs")) { retD(fabs(x)); return true; }
        if (!strcmp(name,"atan2")){ retD(atan2(x, argD(2))); return true; }
        if (!strcmp(name,"pow"))  { retD(pow(x, argD(2))); return true; }
        if (!strcmp(name,"fmod")) { retD(fmod(x, argD(2))); return true; }
        if (!strcmp(name,"hypot")){ retD(hypot(x, argD(2))); return true; }
        // float variants (arg/ret in r0)
        float xf=argF(0);
        if (!strcmp(name,"sqrtf")){ retF(sqrtf(xf)); return true; }
        if (!strcmp(name,"sinf")) { retF(sinf(xf)); return true; }
        if (!strcmp(name,"cosf")) { retF(cosf(xf)); return true; }
        if (!strcmp(name,"fabsf")){ retF(fabsf(xf)); return true; }
        if (!strcmp(name,"floorf")){ retF(floorf(xf)); return true; }
        if (!strcmp(name,"ceilf")){ retF(ceilf(xf)); return true; }
        if (!strcmp(name,"powf")) { retF(powf(xf, argF(1))); return true; }
        if (!strcmp(name,"atan2f")){ retF(atan2f(xf, argF(1))); return true; }
    }

    // ── libc misc ──
    if (!strcmp(name,"abort") || !strcmp(name,"exit") || !strcmp(name,"_exit")) {
        compatLogFmt("arm32: guest called %s(%d) — stopping", name, arg(c,0));
        c.halt = true; c.halt_pc = c.r[15]; ret = 0; return true;
    }
    if (!strcmp(name,"getenv"))  { ret = 0; return true; }         // no env
    if (!strcmp(name,"rand"))    { ret = (uint32_t)rand(); return true; }
    if (!strcmp(name,"srand"))   { srand(arg(c,0)); ret = 0; return true; }
    if (!strcmp(name,"time"))    { uint32_t p=arg(c,0); if(p && guestValid(p,4)) *(uint32_t*)toHost(p)=0; ret = 0; return true; }
    if (!strcmp(name,"malloc_usable_size")) { ret = 0; return true; }

    // ── stdio file I/O — guest FILE* is an index into s_files; paths resolve
    //    against the game's asset dir (logged, so runs reveal what's opened). ──
    if (!strcmp(name,"fopen")) {
        const char* gp = hstr(arg(c,0)); const char* mode = hstr(arg(c,1));
        if (!gp) { ret = 0; return true; }
        std::string path = gp;
        if (path.rfind("sdmc:",0)!=0 && path.rfind("/",0)!=0) path = std::string(g_asset_dir) + gp;
        else if (path.rfind("assets/",0)==0) path = std::string(g_asset_dir) + (gp+7);
        FILE* f = fopen(path.c_str(), mode?mode:"rb");
        compatLogFmt("arm32: fopen(%s,%s) -> %s", gp, mode?mode:"?", f?"ok":"FAIL");
        if (!f) { ret = 0; return true; }
        s_files.push_back(f); ret = (uint32_t)s_files.size();   // handle = index+1
        return true;
    }
    if (!strcmp(name,"fclose")) { FILE* f=gfile(arg(c,0)); if(f){fclose(f); s_files[arg(c,0)-1]=nullptr;} ret=0; return true; }
    if (!strcmp(name,"fread")) {
        uint32_t p=arg(c,0), sz=arg(c,1)*arg(c,2); FILE* f=gfile(arg(c,3));
        ret = (f && guestValid(p,sz)) ? (uint32_t)(fread(hptr(p),1,sz,f)/(arg(c,1)?arg(c,1):1)) : 0; return true;
    }
    if (!strcmp(name,"fwrite")) {
        uint32_t p=arg(c,0), sz=arg(c,1)*arg(c,2); FILE* f=gfile(arg(c,3));
        ret = (f && guestValid(p,sz)) ? (uint32_t)(fwrite(hptr(p),1,sz,f)/(arg(c,1)?arg(c,1):1)) : 0; return true;
    }
    if (!strcmp(name,"fseek"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)fseek(f,(long)arg(c,1),(int)arg(c,2)):(uint32_t)-1; return true; }
    if (!strcmp(name,"ftell"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)ftell(f):(uint32_t)-1; return true; }
    if (!strcmp(name,"rewind")) { FILE* f=gfile(arg(c,0)); if(f) rewind(f); ret=0; return true; }
    if (!strcmp(name,"feof"))   { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)feof(f):1; return true; }
    if (!strcmp(name,"fgetc"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)fgetc(f):(uint32_t)-1; return true; }
    if (!strcmp(name,"fflush")) { FILE* f=gfile(arg(c,0)); if(f) fflush(f); ret=0; return true; }

    return false;
}

void bridgeCall(CpuState& c, uint32_t sentinel) {
    uint32_t idx = (sentinel - A32_SENTINEL_BASE) / 4;
    const char* name = (idx < s_imports.size()) ? s_imports[idx].name.c_str() : "?";

    uint32_t ret = 0;
    if (!dispatch(c, name, ret)) {
        static int warned = 0;
        if (warned < 200) { warned++; compatLogFmt("arm32: UNIMPL import %s (r0=0x%x r1=0x%x r2=0x%x)", name, c.r[0], c.r[1], c.r[2]); }
        ret = 0;   // best-effort stub
    }
    c.r[0] = ret;
    c.r[15] = c.r[14];                 // return to LR
    c.cpsr = (c.r[14] & 1) ? (c.cpsr | 0x20) : (c.cpsr & ~0x20u);  // LR bit0 sets Thumb
    c.r[15] &= ~1u;
}

}  // namespace a32
