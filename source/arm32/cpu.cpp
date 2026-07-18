// ARMv7-A + Thumb interpreter. This is an intentionally partial first cut: the
// common instruction classes are implemented with correct NZCV flags, and
// anything else logs its encoding and halts, so hardware runs drive which
// instructions to add next (the same log-driven method the shim table used).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <switch.h>
#include <cmath>
#include <cstring>

namespace a32 {

// Safety: the interpreter runs on the loader thread. A runaway loop (interpreter
// bug or wild PC) must never wedge the console — bound it by both instruction
// count and wall-clock time, and let the UI thread abort it.
static volatile bool g_abort = false;
void requestAbort() { g_abort = true; }

// CPSR bit helpers.
enum { C_N=1u<<31, C_Z=1u<<30, C_C=1u<<29, C_V=1u<<28, C_T=1u<<5 };
// 16-bit Thumb data-processing ops set flags implicitly — except inside an IT
// block, where only the compare ops (which clear this) may. When set, the flag
// setters below are no-ops.
static bool g_it_suppress = false;
static inline bool nf(CpuState& c){return c.cpsr&C_N;} static inline bool zf(CpuState& c){return c.cpsr&C_Z;}
static inline bool cf(CpuState& c){return c.cpsr&C_C;} static inline bool vf(CpuState& c){return c.cpsr&C_V;}
static inline void setNZ(CpuState& c, uint32_t r){ if(g_it_suppress)return; c.cpsr = (c.cpsr&~(C_N|C_Z)) | (r&0x80000000?C_N:0) | (r==0?C_Z:0); }
static inline void setC(CpuState& c, bool v){ if(g_it_suppress)return; c.cpsr = v ? c.cpsr|C_C : c.cpsr&~C_C; }
static inline void setV(CpuState& c, bool v){ if(g_it_suppress)return; c.cpsr = v ? c.cpsr|C_V : c.cpsr&~C_V; }

static uint32_t addWithCarry(CpuState& c, uint32_t a, uint32_t b, uint32_t cin, bool setflags) {
    uint64_t us = (uint64_t)a + b + cin;
    int64_t  ss = (int64_t)(int32_t)a + (int32_t)b + cin;
    uint32_t r = (uint32_t)us;
    if (setflags) { setNZ(c, r); setC(c, us >> 32); setV(c, ((int64_t)(int32_t)r != ss)); }
    return r;
}

static bool condPass(CpuState& c, uint32_t cond) {
    switch (cond) {
        case 0x0: return zf(c);            case 0x1: return !zf(c);
        case 0x2: return cf(c);            case 0x3: return !cf(c);
        case 0x4: return nf(c);            case 0x5: return !nf(c);
        case 0x6: return vf(c);            case 0x7: return !vf(c);
        case 0x8: return cf(c) && !zf(c);  case 0x9: return !cf(c) || zf(c);
        case 0xA: return nf(c) == vf(c);   case 0xB: return nf(c) != vf(c);
        case 0xC: return !zf(c) && (nf(c)==vf(c)); case 0xD: return zf(c) || (nf(c)!=vf(c));
        default:  return true;             // 0xE always, 0xF handled by caller
    }
}

// All guest memory access is bounds-checked so a wild pointer or stack can
// never read/write the Core's own host memory (which could corrupt the system).
// OOB reads return 0; OOB writes are dropped (both logged, rate-limited).
static void memFault(const char* op, uint32_t g) {
    static int n = 0;
    if (n < 64) { n++; compatLogFmt("arm32: OOB %s at guest 0x%x — contained", op, g); }
}
static inline uint32_t rd32(uint32_t g){ if (!guestValid(g,4)) { memFault("rd32", g); return 0; } return *(uint32_t*)toHost(g); }
static inline void      wr32(uint32_t g, uint32_t v){ if (!guestValid(g,4)) { memFault("wr32", g); return; } *(uint32_t*)toHost(g) = v; }
static inline uint32_t rd16(uint32_t g){ if (!guestValid(g,2)) { memFault("rd16", g); return 0; } return *(uint16_t*)toHost(g); }
static inline void      wr16(uint32_t g, uint16_t v){ if (!guestValid(g,2)) { memFault("wr16", g); return; } *(uint16_t*)toHost(g) = v; }
static inline uint32_t rd8 (uint32_t g){ if (!guestValid(g,1)) { memFault("rd8", g); return 0; } return *(uint8_t*)toHost(g); }
static inline void      wr8 (uint32_t g, uint8_t v){ if (!guestValid(g,1)) { memFault("wr8", g); return; } *(uint8_t*)toHost(g) = v; }

// Barrel shifter for ARM data-processing operand2 (immediate-shift form).
static uint32_t shiftImm(CpuState& c, uint32_t val, uint32_t type, uint32_t amt, bool setc, bool& carry) {
    carry = cf(c);
    switch (type) {
        case 0: if (amt) { carry = (val >> (32-amt)) & 1; val <<= amt; } break;               // LSL
        case 1: if (!amt) amt = 32; carry = (val >> (amt-1)) & 1; val = amt>=32?0:val>>amt; break; // LSR
        case 2: { if (!amt) amt = 32; carry = ((int32_t)val >> (amt>=32?31:amt-1)) & 1; val = (uint32_t)((int32_t)val >> (amt>=32?31:amt)); break; } // ASR
        case 3: if (amt) { carry = (val>>(amt-1))&1; val = (val>>amt)|(val<<(32-amt)); } break; // ROR
    }
    (void)setc; return val;
}

// ── VFP (floating point) ────────────────────────────────────────────────────
// The VFP encoding is identical between ARM and Thumb-2 (Thumb just forces
// cond=AL), so both paths build an ARM-style 32-bit word and call execVFP.
static inline float  u2f(uint32_t u){ float f; memcpy(&f,&u,4); return f; }
static inline uint32_t f2u(float f){ uint32_t u; memcpy(&u,&f,4); return u; }
static inline double u2d(uint64_t u){ double d; memcpy(&d,&u,8); return d; }
static inline uint64_t d2u(double d){ uint64_t u; memcpy(&u,&d,8); return u; }

static uint32_t getS(CpuState& c, uint32_t n){ return (uint32_t)(c.vfp[n>>1] >> ((n&1)*32)); }
static void     setS(CpuState& c, uint32_t n, uint32_t v){
    uint64_t& d = c.vfp[n>>1]; uint32_t sh=(n&1)*32;
    d = (d & ~(0xFFFFFFFFull<<sh)) | ((uint64_t)v<<sh);
}

// VFP condition flags live in FPSCR; mirror them into a small store and copy to
// CPSR on VMRS APSR_nzcv.
static uint32_t g_fpscr_nzcv = 0;

static bool execVFP(CpuState& c, uint32_t w) {
    uint32_t cpn = (w >> 8) & 0xF;               // coprocessor 10 (F32) / 11 (F64)
    if (cpn != 10 && cpn != 11) return false;
    bool dp = (cpn == 11);                        // double precision

    // VLDR / VSTR / VLDM / VSTM (bits 27-25 == 110)
    if ((w & 0x0E000000) == 0x0C000000) {
        bool P=(w>>24)&1, U=(w>>23)&1, Wb=(w>>21)&1, L=(w>>20)&1;
        uint32_t rn=(w>>16)&0xF, vd=(w>>12)&0xF, imm8=w&0xFF;
        uint32_t D=(w>>22)&1;
        if (P && !Wb) {                           // VLDR/VSTR (single reg)
            uint32_t addr = U ? c.r[rn]+imm8*4 : c.r[rn]-imm8*4;
            if (dp) { uint32_t d=(D<<4)|vd; if(L) c.vfp[d]=((uint64_t)rd32(addr+4)<<32)|rd32(addr); else { wr32(addr,(uint32_t)c.vfp[d]); wr32(addr+4,(uint32_t)(c.vfp[d]>>32)); } }
            else    { uint32_t s=(vd<<1)|D; if(L) setS(c,s,rd32(addr)); else wr32(addr,getS(c,s)); }
            return true;
        }
        // VLDM/VSTM
        uint32_t regs = dp ? (imm8/2) : imm8;
        uint32_t addr = U ? c.r[rn] : c.r[rn]-imm8*4;
        uint32_t first = dp ? ((D<<4)|vd) : ((vd<<1)|D);
        for (uint32_t i=0;i<regs;i++) {
            if (dp) { uint32_t d=first+i; if(L) c.vfp[d]=((uint64_t)rd32(addr+4)<<32)|rd32(addr); else { wr32(addr,(uint32_t)c.vfp[d]); wr32(addr+4,(uint32_t)(c.vfp[d]>>32)); } addr+=8; }
            else    { uint32_t s=first+i; if(L) setS(c,s,rd32(addr)); else wr32(addr,getS(c,s)); addr+=4; }
        }
        if (Wb) c.r[rn] = U ? c.r[rn]+imm8*4 : c.r[rn]-imm8*4;
        return true;
    }

    // VMOV core <-> single, VMOV core-pair <-> double, VMSR/VMRS (bits 27-24==1110, bit4==1)
    if ((w & 0x0F000010) == 0x0E000010) {
        bool toArm=(w>>20)&1;
        if (((w>>21)&0x7)==0 && ((w>>8)&0xF)==10) {          // VMOV Sn,Rt / Rt,Sn
            uint32_t sn=((w>>16)&0xF)<<1 | ((w>>7)&1), rt=(w>>12)&0xF;
            if (toArm) c.r[rt]=getS(c,sn); else setS(c,sn,c.r[rt]);
            return true;
        }
        if (((w>>21)&0x7)==7) {                               // VMRS/VMSR
            uint32_t rt=(w>>12)&0xF;
            if (toArm) { if(rt==15) c.cpsr=(c.cpsr&~0xF0000000u)|(g_fpscr_nzcv&0xF0000000u); else c.r[rt]=g_fpscr_nzcv; }
            else g_fpscr_nzcv=c.r[rt];
            return true;
        }
    }
    // VMOV core-pair <-> double/two-single (bits 27-21 == 1100 010)
    if ((w & 0x0FE00000) == 0x0C400000) {
        bool toArm=(w>>20)&1; uint32_t rt=(w>>12)&0xF, rt2=(w>>16)&0xF;
        if ((w>>8&0xF)==11) { uint32_t d=((w>>5&1)<<4)|(w&0xF);
            if (toArm){ c.r[rt]=(uint32_t)c.vfp[d]; c.r[rt2]=(uint32_t)(c.vfp[d]>>32);} else c.vfp[d]=((uint64_t)c.r[rt2]<<32)|c.r[rt]; }
        else { uint32_t m=((w&0xF)<<1)|((w>>5)&1);
            if (toArm){ c.r[rt]=getS(c,m); c.r[rt2]=getS(c,m+1);} else { setS(c,m,c.r[rt]); setS(c,m+1,c.r[rt2]); } }
        return true;
    }

    // VFP data processing (bits 27-24 == 1110). The opcode is {bit23,bit21,bit20}
    // (bit22 is the D register bit), plus bit6.
    if ((w & 0x0F000000) == 0x0E000000) {
        uint32_t D=(w>>22)&1, vn=(w>>16)&0xF, vd=(w>>12)&0xF, N=(w>>7)&1, M=(w>>5)&1, vm=w&0xF;
        uint32_t o = (((w>>23)&1)<<2) | (((w>>21)&1)<<1) | ((w>>20)&1), op6=(w>>6)&1;
        uint32_t d = dp?((D<<4)|vd):((vd<<1)|D), n = dp?((N<<4)|vn):((vn<<1)|N), m = dp?((M<<4)|vm):((vm<<1)|M);
        auto rN=[&](uint32_t r){ return dp?u2d(c.vfp[r]):(double)u2f(getS(c,r)); };
        auto wD=[&](double v){ if(dp) c.vfp[d]=d2u(v); else setS(c,d,f2u((float)v)); };
        switch (o) {
            case 0: wD(rN(d) + (op6 ? -(rN(n)*rN(m)) : (rN(n)*rN(m)))); return true;  // VMLA/VMLS
            case 2: wD(rN(n)*rN(m)); return true;                                     // VMUL
            case 3: wD(op6 ? rN(n)-rN(m) : rN(n)+rN(m)); return true;                 // VADD/VSUB
            case 4: wD(rN(n)/rN(m)); return true;                                     // VDIV
            case 7: {                                                                // extension family
                uint32_t opc2=vn, opc3=(w>>6)&3;
                if (opc3 == 0) break;                       // VMOV immediate — rare, log
                switch (opc2) {
                    case 0x0: wD((opc3&2) ? fabs(rN(m)) : rN(m)); return true;         // VABS / VMOV reg
                    case 0x1: wD((opc3&2) ? sqrt(rN(m)) : -rN(m)); return true;        // VSQRT / VNEG
                    case 0x4: case 0x5: {                                             // VCMP / VCMPE
                        double a=rN(d), b=(opc2==5)?0.0:rN(m);
                        g_fpscr_nzcv = (a==b)?0x60000000u : (a<b)?0x80000000u : 0x20000000u;
                        return true;
                    }
                    case 0x7: if (opc3&2) {                                           // VCVT double<->single
                        if (dp) setS(c,(vd<<1)|D, f2u((float)u2d(c.vfp[(M<<4)|vm])));
                        else    c.vfp[(D<<4)|vd] = d2u((double)u2f(getS(c,(vm<<1)|M)));
                        return true;
                    } break;
                    case 0x8: {                                                       // VCVT int->float (Sm)
                        int32_t iv=(int32_t)getS(c,(vm<<1)|M); bool uns=!op6;
                        wD(uns ? (double)(uint32_t)iv : (double)iv); return true;
                    }
                    case 0xC: case 0xD: {                                             // VCVT float->int (to Sd)
                        double v=rN(m); bool uns=(opc2==0xC);
                        setS(c,(vd<<1)|D, uns ? (uint32_t)(v<0?0:v) : (uint32_t)(int32_t)v);
                        return true;
                    }
                }
                break;
            }
        }
    }
    compatLogFmt("arm32: UNIMPL VFP 0x%08x", w);
    return false;
}
// ── Linux ARM (EABI) syscalls via SVC #0 (number in r7, args r0-r6) ─────────
// Bionic and the CRT issue raw syscalls during init. Handle the common ones;
// stub the rest to 0 + log (rate-limited) so execution continues to the next
// real blocker rather than halting on every unknown number.
uint32_t g_tls = 0;   // ARM thread pointer (set via __ARM_NR_set_tls, read via CP15 c13)
static void execSyscall(CpuState& c) {
    uint32_t nr = c.r[7];
    switch (nr) {
        case 4:   /*write*/   { uint32_t fd=c.r[0], n=c.r[2]; if(fd==1||fd==2){ char*s=(char*)toHost(c.r[1]); if(guestValid(c.r[1],n)){ static char b[256]; uint32_t m=n<255?n:255; memcpy(b,s,m); b[m]=0; compatLogFmt("arm32 guest: %s", b);} } c.r[0]=n; return; }
        case 20:  /*getpid*/  c.r[0]=1; return;
        case 45:  /*brk*/     c.r[0]=0; return;      // malloc uses the guest allocator, not brk
        case 78:  /*gettimeofday*/ { if(guestValid(c.r[0],8)){ uint64_t us=armTicksToNs(armGetSystemTick())/1000; wr32(c.r[0],(uint32_t)(us/1000000)); wr32(c.r[0]+4,(uint32_t)(us%1000000)); } c.r[0]=0; return; }
        case 91:  /*munmap*/  c.r[0]=0; return;
        case 120: /*clone*/   compatLog("arm32: clone (thread) — not supported yet, returning EAGAIN"); c.r[0]=(uint32_t)-11; return;
        case 122: /*uname*/   c.r[0]=0; return;
        case 125: /*mprotect*/c.r[0]=0; return;      // guest memory is all RW in our region
        case 146: /*writev*/  { uint32_t iov=c.r[1], cnt=c.r[2], tot=0; for(uint32_t i=0;i<cnt;i++){ uint32_t len=rd32(iov+i*8+4); tot+=len; } c.r[0]=tot; return; }
        case 162: /*nanosleep*/ c.r[0]=0; return;
        case 174: /*rt_sigaction*/ case 175: /*rt_sigprocmask*/ case 186: /*sigaltstack*/ c.r[0]=0; return;
        case 192: /*mmap2*/   { uint32_t len=c.r[1]; uint32_t p=guestAlloc(len); if(p && guestValid(p,len)) memset(toHost(p),0,len); c.r[0]=p?p:(uint32_t)-1; return; }
        case 199: /*getuid32*/ case 200: /*getgid32*/ case 201: /*geteuid32*/ case 202: /*getegid32*/ c.r[0]=0; return;
        case 220: /*madvise*/ c.r[0]=0; return;
        case 224: /*gettid*/  c.r[0]=1; return;
        case 240: /*futex*/   c.r[0]=0; return;      // single-threaded: never contended
        case 248: /*exit_group*/ case 1: /*exit*/ compatLog("arm32: guest exit"); c.halt=true; c.halt_pc=c.r[15]; return;
        case 263: /*clock_gettime*/ { if(guestValid(c.r[1],8)){ uint64_t ns=armTicksToNs(armGetSystemTick()); wr32(c.r[1],(uint32_t)(ns/1000000000ull)); wr32(c.r[1]+4,(uint32_t)(ns%1000000000ull)); } c.r[0]=0; return; }
        case 0xf0002: /*__ARM_NR_cacheflush*/ c.r[0]=0; return;
        case 0xf0005: /*__ARM_NR_set_tls*/ g_tls=c.r[0]; c.r[0]=0; return;
        default: {
            static int warned=0; if(warned<80){ warned++; compatLogFmt("arm32: UNIMPL syscall %u (0x%x) r0=0x%x pc=0x%x", nr, nr, c.r[0], c.r[15]); }
            c.r[0]=0; return;
        }
    }
}

// Is this ARM-style word a VFP/coproc-10/11 load-store or data-processing insn?
static inline bool isVFP(uint32_t w) {
    uint32_t cpn = (w >> 8) & 0xF;
    if (cpn != 10 && cpn != 11) return false;
    return (w & 0x0E000000) == 0x0C000000 || (w & 0x0F000000) == 0x0E000000;
}

// Shift by a register amount (differs from immediate at boundary cases).
static uint32_t shiftReg(CpuState& c, uint32_t val, uint32_t type, uint32_t amt, bool& carry) {
    carry = cf(c);
    if (amt == 0) return val;
    switch (type) {
        case 0: if (amt>=32){carry=amt==32?val&1:0; return 0;} carry=(val>>(32-amt))&1; return val<<amt;                 // LSL
        case 1: if (amt>=32){carry=amt==32?(val>>31)&1:0; return 0;} carry=(val>>(amt-1))&1; return val>>amt;            // LSR
        case 2: if (amt>=32){carry=(val>>31)&1; return (uint32_t)((int32_t)val>>31);} carry=((int32_t)val>>(amt-1))&1; return (uint32_t)((int32_t)val>>amt); // ASR
        case 3: { amt&=31; if(!amt) return val; carry=(val>>(amt-1))&1; return (val>>amt)|(val<<(32-amt)); }             // ROR
    }
    return val;
}

// ── ARM (32-bit) step ───────────────────────────────────────────────────────
static void stepArm(CpuState& c) {
    g_it_suppress = false;              // ARM has no IT; flags behave normally
    uint32_t pc = c.r[15];
    uint32_t insn = rd32(pc);
    c.r[15] = pc + 4;                       // default advance; PC reads as +8 in ARM
    uint32_t cond = insn >> 28;
    if (cond != 0xF && !condPass(c, cond)) return;

    // VFP / coprocessor 10-11
    if (isVFP(insn)) { if (execVFP(c, insn)) return; c.halt = true; c.halt_pc = pc; return; }

    // CLZ / REV / REV16 / REVSH (precise masks — decoded before the generic
    // data-processing and load/store branches that would misclassify them).
    if ((insn & 0x0FFF0FF0) == 0x016F0F10) { uint32_t rd=(insn>>12)&0xF; c.r[rd]=c.r[insn&0xF]?(uint32_t)__builtin_clz(c.r[insn&0xF]):32; return; }
    if ((insn & 0x0FFF0FF0) == 0x06BF0F30) { uint32_t rd=(insn>>12)&0xF; c.r[rd]=__builtin_bswap32(c.r[insn&0xF]); return; }
    if ((insn & 0x0FFF0FF0) == 0x06BF0FB0) { uint32_t rd=(insn>>12)&0xF, v=c.r[insn&0xF]; c.r[rd]=((v&0xFF00FF00)>>8)|((v&0x00FF00FF)<<8); return; }
    if ((insn & 0x0FFF0FF0) == 0x06FF0FB0) { uint32_t rd=(insn>>12)&0xF; c.r[rd]=(int32_t)(int16_t)__builtin_bswap16((uint16_t)c.r[insn&0xF]); return; }

    // SVC / SWI (syscall) — cond 1111 imm24
    if ((insn & 0x0F000000) == 0x0F000000) { execSyscall(c); return; }

    // Coprocessor 15 (system control): TLS read (TPIDRURO) + barriers/cache (nop)
    if ((insn & 0x0F000010) == 0x0E000010 && ((insn >> 8) & 0xF) == 15) {
        if ((insn >> 20) & 1) {                      // MRC (read into Rd)
            uint32_t rd=(insn>>12)&0xF, crn=(insn>>16)&0xF, op2=(insn>>5)&7;
            c.r[rd] = (crn==13 && op2==3) ? g_tls : 0;   // c13,c0,3 = thread pointer
        }
        return;                                      // MCR (barrier/cache) = no-op
    }

    // Branch / BL (cond 101)
    if ((insn & 0x0E000000) == 0x0A000000) {
        int32_t off = (int32_t)(insn << 8) >> 6;         // sign-extend imm24<<2
        if (insn & 0x01000000) c.r[14] = c.r[15];        // BL: LR = next
        c.r[15] = c.r[15] + 4 + off;                     // +8 pipeline
        return;
    }
    // BX / BLX (register)
    if ((insn & 0x0FFFFFD0) == 0x012FFF10) {
        uint32_t rm = c.r[insn & 0xF];
        if (insn & 0x20) c.r[14] = pc + 4;               // BLX
        c.cpsr = (rm & 1) ? (c.cpsr|C_T) : (c.cpsr&~C_T);
        c.r[15] = rm & ~1u;
        return;
    }
    // MOVW / MOVT (16-bit immediate)
    if ((insn & 0x0FF00000) == 0x03000000 || (insn & 0x0FF00000) == 0x03400000) {
        uint32_t rd = (insn >> 12) & 0xF;
        uint32_t imm16 = ((insn >> 4) & 0xF000) | (insn & 0xFFF);
        if (insn & 0x00400000) c.r[rd] = (c.r[rd] & 0xFFFF) | (imm16 << 16);  // MOVT
        else                   c.r[rd] = imm16;                               // MOVW
        return;
    }
    // Multiply MUL/MLA (bits 27-22=0, 7-4=1001) — decode before data-processing
    if ((insn & 0x0FC000F0) == 0x00000090) {
        uint32_t rd=(insn>>16)&0xF, rn=(insn>>12)&0xF, rs=(insn>>8)&0xF, rm=insn&0xF;
        bool A=(insn>>21)&1, S=(insn>>20)&1;
        uint32_t r = c.r[rm]*c.r[rs] + (A?c.r[rn]:0);
        c.r[rd]=r; if (S) setNZ(c,r);
        return;
    }
    // Long multiply UMULL/UMLAL/SMULL/SMLAL (bits 27-23=00001, 7-4=1001)
    if ((insn & 0x0F8000F0) == 0x00800090) {
        uint32_t rdhi=(insn>>16)&0xF, rdlo=(insn>>12)&0xF, rs=(insn>>8)&0xF, rm=insn&0xF;
        bool sign=(insn>>22)&1, A=(insn>>21)&1, S=(insn>>20)&1;
        uint64_t res = sign ? (uint64_t)((int64_t)(int32_t)c.r[rm]*(int64_t)(int32_t)c.r[rs])
                            : (uint64_t)c.r[rm]*(uint64_t)c.r[rs];
        if (A) res += ((uint64_t)c.r[rdhi]<<32) | c.r[rdlo];
        c.r[rdlo]=(uint32_t)res; c.r[rdhi]=(uint32_t)(res>>32);
        if (S) { c.cpsr = (c.cpsr&~(C_N|C_Z)) | ((c.r[rdhi]&0x80000000)?C_N:0) | ((res==0)?C_Z:0); }
        return;
    }
    // Extra load/store: LDRH/STRH/LDRSB/LDRSH (bits 27-25=0, bit7=1, bit4=1, SH!=00)
    if ((insn & 0x0E000090) == 0x00000090 && ((insn>>5)&3) != 0) {
        bool P=(insn>>24)&1, U=(insn>>23)&1, I=(insn>>22)&1, W=(insn>>21)&1, L=(insn>>20)&1;
        uint32_t rn=(insn>>16)&0xF, rd=(insn>>12)&0xF, sh=(insn>>5)&3;
        uint32_t off = I ? ((((insn>>8)&0xF)<<4)|(insn&0xF)) : c.r[insn&0xF];
        uint32_t base = c.r[rn], addr = P ? (U?base+off:base-off) : base;
        if (L) {
            if (sh==1) c.r[rd]=rd16(addr);
            else if (sh==2) c.r[rd]=(int32_t)(int8_t)rd8(addr);
            else c.r[rd]=(int32_t)(int16_t)rd16(addr);
        } else if (sh==1) {
            wr16(addr, (uint16_t)c.r[rd]);              // STRH (LDRD/STRD sh=2/3 unhandled)
        }
        if (!P) addr = U?base+off:base-off;
        if ((!P||W) && rn!=rd) c.r[rn]=addr;
        return;
    }

    // Data processing (bits 27-26 == 00), immediate / reg-imm-shift / reg-reg-shift
    if ((insn & 0x0C000000) == 0x00000000) {
        uint32_t op = (insn >> 21) & 0xF;
        bool     S  = (insn >> 20) & 1;
        uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        uint32_t opnd; bool shco = cf(c);
        if (insn & 0x02000000) {                          // immediate operand2
            uint32_t imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
            opnd = rot ? (imm >> rot) | (imm << (32-rot)) : imm;
            if (rot) shco = opnd >> 31;
        } else if ((insn & 0x10) == 0) {                  // register, immediate shift
            uint32_t rm = c.r[insn & 0xF], type = (insn >> 5) & 3, amt = (insn >> 7) & 0x1F;
            opnd = shiftImm(c, rm, type, amt, S, shco);
        } else {                                          // register, register shift
            uint32_t rm = c.r[insn & 0xF], type = (insn >> 5) & 3, amt = c.r[(insn >> 8) & 0xF] & 0xFF;
            opnd = shiftReg(c, rm, type, amt, shco);
        }
        uint32_t vn = c.r[rn], res = 0; bool wb = true;
        switch (op) {
            case 0x0: res = vn & opnd; break;                         // AND
            case 0x1: res = vn ^ opnd; break;                         // EOR
            case 0x2: res = addWithCarry(c, vn, ~opnd, 1, S); wb=true; goto flags_done; // SUB
            case 0x3: res = addWithCarry(c, ~vn, opnd, 1, S); goto flags_done;          // RSB
            case 0x4: res = addWithCarry(c, vn, opnd, 0, S); goto flags_done;           // ADD
            case 0x5: res = addWithCarry(c, vn, opnd, cf(c), S); goto flags_done;       // ADC
            case 0x6: res = addWithCarry(c, vn, ~opnd, cf(c), S); goto flags_done;      // SBC
            case 0x8: res = vn & opnd; wb=false; break;               // TST
            case 0x9: res = vn ^ opnd; wb=false; break;               // TEQ
            case 0xA: addWithCarry(c, vn, ~opnd, 1, true); wb=false; goto flags_done;   // CMP
            case 0xB: addWithCarry(c, vn, opnd, 0, true); wb=false; goto flags_done;    // CMN
            case 0xC: res = vn | opnd; break;                         // ORR
            case 0xD: res = opnd; break;                              // MOV
            case 0xE: res = vn & ~opnd; break;                        // BIC
            case 0xF: res = ~opnd; break;                             // MVN
        }
        if (S) { setNZ(c, res); setC(c, shco); }
    flags_done:
        if (wb && op != 0x8 && op != 0x9 && op != 0xA && op != 0xB) {
            c.r[rd] = res;
            if (rd == 15) { c.cpsr = (res & 1) ? (c.cpsr|C_T):(c.cpsr&~C_T); c.r[15] = res & ~1u; }
        }
        return;
    }
    // Single data transfer LDR/STR (bits 27-26 == 01), immediate or reg offset
    if ((insn & 0x0C000000) == 0x04000000) {
        bool I=(insn>>25)&1, P=(insn>>24)&1, U=(insn>>23)&1, B=(insn>>22)&1, W=(insn>>21)&1, L=(insn>>20)&1;
        uint32_t rn=(insn>>16)&0xF, rd=(insn>>12)&0xF, off;
        if (!I) off = insn & 0xFFF;
        else { uint32_t rm=c.r[insn&0xF], type=(insn>>5)&3, amt=(insn>>7)&0x1F; bool co; off=shiftImm(c,rm,type,amt,false,co); }
        uint32_t base = (rn==15) ? (pc+8) : c.r[rn];
        uint32_t addr = P ? (U?base+off:base-off) : base;
        if (L) c.r[rd] = B ? rd8(addr) : rd32(addr);
        else   { if (B) wr8(addr,(uint8_t)c.r[rd]); else wr32(addr, c.r[rd]); }
        if (!P) addr = U?base+off:base-off;
        if ((!P || W) && rn != 15 && rn != rd) c.r[rn] = addr;
        if (L && rd == 15) { c.cpsr=(c.r[15]&1)?(c.cpsr|C_T):(c.cpsr&~C_T); c.r[15]&=~1u; }
        return;
    }
    // Block transfer LDM/STM (push/pop)
    if ((insn & 0x0E000000) == 0x08000000) {
        bool P=(insn>>24)&1, U=(insn>>23)&1, W=(insn>>21)&1, L=(insn>>20)&1;
        uint32_t rn=(insn>>16)&0xF, list=insn&0xFFFF;
        uint32_t addr=c.r[rn], n=__builtin_popcount(list);
        uint32_t base = U ? addr : addr - n*4;
        uint32_t a = base + (P == U ? 4 : 0) - (U?0:0);
        a = U ? (P?addr+4:addr) : (P?addr-n*4:addr-n*4+4);
        for (int i=0;i<16;i++) if (list&(1<<i)) {
            if (!guestValid(a,4)) { c.halt=true; c.halt_pc=pc; return; }
            if (L) c.r[i]=rd32(a); else wr32(a, c.r[i]);
            a+=4;
        }
        if (W) c.r[rn] = U ? addr + n*4 : addr - n*4;
        if (L && (list&0x8000)) { c.cpsr=(c.r[15]&1)?(c.cpsr|C_T):(c.cpsr&~C_T); c.r[15]&=~1u; }
        return;
    }

    compatLogFmt("arm32: UNIMPL ARM insn=0x%08x pc=0x%x", insn, pc);
    c.halt = true; c.halt_pc = pc;
}

// Thumb ExpandImm_C (12-bit modified immediate → 32-bit).
static uint32_t thumbExpandImm(uint32_t imm12, CpuState& c, bool& carry) {
    carry = cf(c);
    if ((imm12 & 0xC00) == 0) {               // imm12[11:10] == 00
        uint32_t imm8 = imm12 & 0xFF;
        switch ((imm12 >> 8) & 3) {
            case 0: return imm8;
            case 1: return (imm8 << 16) | imm8;
            case 2: return (imm8 << 24) | (imm8 << 8);
            default: return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
        }
    }
    uint32_t rot = (imm12 >> 7) & 0x1F;
    uint32_t val = 0x80 | (imm12 & 0x7F);
    uint32_t r = (val >> rot) | (val << (32 - rot));
    carry = r >> 31;
    return r;
}

// ── Thumb-2 32-bit instruction step ─────────────────────────────────────────
static void stepThumb32(CpuState& c, uint32_t pc, uint16_t hw1) {
    uint16_t hw2 = rd16(pc + 2);
    c.r[15] = pc + 4;

    // VFP / coprocessor 10-11 (same layout as ARM once the halfwords are joined)
    { uint32_t w = ((uint32_t)hw1 << 16) | hw2;
      if (isVFP(w)) { if (execVFP(c, w)) return; c.halt = true; c.halt_pc = pc; return; } }

    // ── Branches & misc control (11110 ... 1x) ──
    if ((hw1 & 0xF800) == 0xF000 && (hw2 & 0x8000)) {
        uint32_t s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
        uint32_t imm10 = hw1 & 0x3FF, imm11 = hw2 & 0x7FF;
        if (hw2 & 0x4000) {                    // BL / BLX (hw2[14]=1)
            uint32_t i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
            int32_t off = (int32_t)((s<<24)|(i1<<23)|(i2<<22)|(imm10<<12)|(imm11<<1));
            off = (off << 7) >> 7;             // sign-extend 25-bit
            c.r[14] = (pc + 4) | 1;
            uint32_t target = pc + 4 + off;
            if (!(hw2 & 0x1000)) { c.cpsr &= ~C_T; target &= ~3u; }  // BLX
            c.r[15] = target & ~1u;
            return;
        }
        if (hw2 & 0x1000) {                    // B.W unconditional (T4)
            uint32_t i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
            int32_t off = (int32_t)((s<<24)|(i1<<23)|(i2<<22)|(imm10<<12)|(imm11<<1));
            off = (off << 7) >> 7;
            c.r[15] = pc + 4 + off;
            return;
        }
        // B<cond>.W (T3)
        uint32_t cond = (hw1 >> 6) & 0xF;
        int32_t off = (int32_t)((s<<20)|(j2<<19)|(j1<<18)|((hw1&0x3F)<<12)|(imm11<<1));
        off = (off << 11) >> 11;
        if (condPass(c, cond)) c.r[15] = pc + 4 + off;
        return;
    }

    // ── Data processing (modified immediate) — 11110 x0xxx ... 0 ──
    if ((hw1 & 0xFA00) == 0xF000 && !(hw2 & 0x8000)) {
        uint32_t op = (hw1 >> 5) & 0xF, rn = hw1 & 0xF, rd = (hw2 >> 8) & 0xF;
        bool S = (hw1 >> 4) & 1;
        uint32_t imm12 = ((hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xFF);
        bool co; uint32_t imm = thumbExpandImm(imm12, c, co), vn = c.r[rn], r = 0;
        switch (op) {
            case 0x0: r = vn & imm; if(rd==15){setNZ(c,r);setC(c,co);return;} break;   // AND/TST
            case 0x1: r = vn & ~imm; break;                                            // BIC
            case 0x2: r = vn | imm; break;                                             // ORR/MOV(rn=15)
            case 0x3: r = vn | ~imm; break;                                            // ORN/MVN
            case 0x4: r = vn ^ imm; if(rd==15){setNZ(c,r);setC(c,co);return;} break;   // EOR/TEQ
            case 0x8: r = addWithCarry(c, vn, imm, 0, S); if(rd==15){return;} c.r[rd]=r; if(S)return; return; // ADD/CMN
            case 0xA: r = addWithCarry(c, vn, imm, cf(c), S); c.r[rd]=r; return;        // ADC
            case 0xB: r = addWithCarry(c, vn, ~imm, cf(c), S); c.r[rd]=r; return;       // SBC
            case 0xD: r = addWithCarry(c, vn, ~imm, 1, S); if(rd==15){return;} c.r[rd]=r; return; // SUB/CMP
            case 0xE: r = addWithCarry(c, ~vn, imm, 1, S); c.r[rd]=r; return;           // RSB
            default: break;
        }
        c.r[rd] = r;
        if (S) { setNZ(c, r); setC(c, co); }
        return;
    }

    // ── Plain binary immediate: MOVW/MOVT, ADDW/SUBW, bitfield — 11110 x1xxx ─
    if ((hw1 & 0xFA00) == 0xF200 && !(hw2 & 0x8000)) {
        uint32_t op = (hw1 >> 4) & 0x1F, rn = hw1 & 0xF, rd = (hw2 >> 8) & 0xF;
        uint32_t i = (hw1 >> 10) & 1, imm3 = (hw2 >> 12) & 7, imm8 = hw2 & 0xFF;
        if (op == 0x04 || op == 0x0C) {        // MOVW / MOVT
            uint32_t imm16 = ((hw1 & 0xF) << 12) | (i << 11) | (imm3 << 8) | imm8;
            if (op == 0x0C) c.r[rd] = (c.r[rd] & 0xFFFF) | (imm16 << 16);   // MOVT
            else            c.r[rd] = imm16;                                // MOVW
            return;
        }
        if (op == 0x00 || op == 0x0A) {        // ADDW / SUBW (12-bit imm)
            uint32_t imm = (i << 11) | (imm3 << 8) | imm8;
            c.r[rd] = (op == 0x0A) ? c.r[rn] - imm : c.r[rn] + imm;
            return;
        }
        uint32_t msb = hw2 & 0x1F, lsb = (imm3 << 2) | ((hw2 >> 6) & 3);
        if (op == 0x14 || op == 0x1C) {        // SBFX / UBFX
            uint32_t width = msb + 1, v = c.r[rn] >> lsb;
            v &= (width >= 32) ? 0xFFFFFFFF : ((1u << width) - 1);
            if (op == 0x14 && (v & (1u << (width - 1)))) v |= ~((1u << width) - 1);  // sign-extend
            c.r[rd] = v;
            return;
        }
        if (op == 0x16) {                      // BFI / BFC
            uint32_t width = msb - lsb + 1, mask = (width>=32?0xFFFFFFFF:((1u<<width)-1)) << lsb;
            uint32_t src = (rn == 15) ? 0 : c.r[rn];         // BFC when rn==15
            c.r[rd] = (c.r[rd] & ~mask) | ((src << lsb) & mask);
            return;
        }
    }

    // ── Load/store single, immediate (11111 00x) ──
    if ((hw1 & 0xFE00) == 0xF800) {
        uint32_t size = (hw1 >> 5) & 3, L = (hw1 >> 4) & 1;
        uint32_t rn = hw1 & 0xF, rt = (hw2 >> 12) & 0xF;
        bool sign = (hw1 >> 8) & 1;
        uint32_t addr, wback = 0, wbval = 0;
        if (hw2 & 0x0800) {                    // T4: 8-bit imm, index/wback (bit10=P,9=U,8=W)
            uint32_t imm8 = hw2 & 0xFF; bool P=(hw2>>10)&1, U=(hw2>>9)&1, W=(hw2>>8)&1;
            uint32_t off = U ? c.r[rn] + imm8 : c.r[rn] - imm8;
            addr = P ? off : c.r[rn];
            if (!P || W) { wback = rn + 1; wbval = off; }
        } else if (rn == 15) {                 // literal
            uint32_t imm12 = hw2 & 0xFFF;
            addr = ((pc + 4) & ~3u) + ((hw1 & 0x80) ? imm12 : (uint32_t)-(int32_t)imm12);
        } else if ((hw1 & 0x0080)) {           // T3: 12-bit positive imm
            addr = c.r[rn] + (hw2 & 0xFFF);
        } else {                               // register offset (T2)
            uint32_t rm = hw2 & 0xF, sh = (hw2 >> 4) & 3;
            addr = c.r[rn] + (c.r[rm] << sh);
        }
        if (L) {
            uint32_t v = size==0 ? rd8(addr) : size==1 ? rd16(addr) : rd32(addr);
            if (sign && size==0) v = (int32_t)(int8_t)v;
            if (sign && size==1) v = (int32_t)(int16_t)v;
            c.r[rt] = v;
        } else {
            if (size==0) wr8(addr,(uint8_t)c.r[rt]); else if (size==1) wr16(addr,(uint16_t)c.r[rt]); else wr32(addr,c.r[rt]);
        }
        if (wback) c.r[wback-1] = wbval;
        return;
    }

    // ── Load/store multiple (11101 00) — LDM/STM/PUSH.W/POP.W ──
    if ((hw1 & 0xFE40) == 0xE800 || (hw1 & 0xFE40) == 0xE900) {
        bool L=(hw1>>4)&1, U=(hw1>>7)&1, W=(hw1>>5)&1;
        uint32_t rn=hw1&0xF, list=hw2, n=__builtin_popcount(list);
        uint32_t addr = U ? c.r[rn] : c.r[rn]-n*4;
        for (int i=0;i<16;i++) if (list&(1<<i)) { if(L) c.r[i]=rd32(addr); else wr32(addr,c.r[i]); addr+=4; }
        if (W) c.r[rn] = U ? c.r[rn]+n*4 : c.r[rn]-n*4;
        if (L && (list&0x8000)) { c.cpsr=(c.r[15]&1)?(c.cpsr|C_T):(c.cpsr&~C_T); c.r[15]&=~1u; }
        return;
    }

    // ── Data processing (shifted register) — 11101 01 ──
    if ((hw1 & 0xFE00) == 0xEA00) {
        uint32_t op=(hw1>>5)&0xF, rn=hw1&0xF, rd=(hw2>>8)&0xF, rm=hw2&0xF;
        bool S=(hw1>>4)&1;
        uint32_t amt=((hw2>>12)&7)<<2 | ((hw2>>6)&3), type=(hw2>>4)&3;
        bool co; uint32_t opnd=shiftImm(c, c.r[rm], type, amt, S, co), vn=c.r[rn], r=0;
        switch (op) {
            case 0x0: r=vn&opnd; if(rd==15){setNZ(c,r);return;} break;      // AND/TST
            case 0x1: r=vn&~opnd; break;                                    // BIC
            case 0x2: r=(rn==15)?opnd:(vn|opnd); break;                     // ORR/MOV
            case 0x3: r=(rn==15)?~opnd:(vn|~opnd); break;                   // ORN/MVN
            case 0x4: r=vn^opnd; if(rd==15){setNZ(c,r);return;} break;      // EOR/TEQ
            case 0x8: r=addWithCarry(c,vn,opnd,0,S); if(rd==15)return; break;// ADD/CMN
            case 0xD: r=addWithCarry(c,vn,~opnd,1,S); if(rd==15)return; break;// SUB/CMP
            case 0xE: r=addWithCarry(c,~vn,opnd,1,S); break;                // RSB
            default: c.r[rd]=vn; return;
        }
        c.r[rd]=r;
        if (S && op!=0x8 && op!=0xD && op!=0xE) { setNZ(c,r); setC(c,co); }
        return;
    }

    // ── Multiply (11111 0110) — MUL/MLA/MLS ──
    if ((hw1 & 0xFF80) == 0xFB00) {
        uint32_t rn=hw1&0xF, ra=(hw2>>12)&0xF, rd=(hw2>>8)&0xF, rm=hw2&0xF, op2=(hw2>>4)&0xF;
        uint32_t prod=c.r[rn]*c.r[rm];
        c.r[rd] = (ra==15) ? prod : (op2 ? c.r[ra]-prod : c.r[ra]+prod);  // MUL / MLA / MLS
        return;
    }
    // ── Long multiply / divide (11111 011 1) — UMULL/SMULL/UDIV/SDIV ──
    if ((hw1 & 0xFF80) == 0xFB80) {
        uint32_t rn=hw1&0xF, rdlo=(hw2>>12)&0xF, rdhi=(hw2>>8)&0xF, rm=hw2&0xF;
        uint32_t op=(hw1>>4)&7, op2=(hw2>>4)&0xF;
        if (op==1 && op2==0xF) { c.r[rdhi] = c.r[rm] ? (uint32_t)((int32_t)c.r[rn]/(int32_t)c.r[rm]) : 0; return; } // SDIV → rdhi is Rd
        if (op==3 && op2==0xF) { c.r[rdhi] = c.r[rm] ? c.r[rn]/c.r[rm] : 0; return; }                              // UDIV
        uint64_t res = (op&4) ? (uint64_t)((int64_t)(int32_t)c.r[rn]*(int64_t)(int32_t)c.r[rm])
                              : (uint64_t)c.r[rn]*(uint64_t)c.r[rm];
        if (op&2) res += ((uint64_t)c.r[rdhi]<<32)|c.r[rdlo];   // accumulate (UMLAL/SMLAL)
        c.r[rdlo]=(uint32_t)res; c.r[rdhi]=(uint32_t)(res>>32);
        return;
    }

    // ── TBB/TBH (table branch) — 1110 1000 1101 Rn ──
    if ((hw1 & 0xFFF0) == 0xE8D0 && (hw2 & 0xFFE0) == 0xF000) {
        uint32_t rn=hw1&0xF, rm=hw2&0xF, H=(hw2>>4)&1;
        uint32_t base = (rn==15) ? (pc+4) : c.r[rn];
        uint32_t idx = H ? rd16(base + 2*c.r[rm]) : rd8(base + c.r[rm]);
        c.r[15] = pc + 4 + 2*idx;
        return;
    }
    // ── CLZ — 1111 1010 1011 Rn : 1111 Rd 1000 Rm ──
    if ((hw1 & 0xFFF0) == 0xFAB0 && (hw2 & 0xF0F0) == 0xF080) {
        uint32_t rm=hw2&0xF, rd=(hw2>>8)&0xF;
        c.r[rd] = c.r[rm] ? (uint32_t)__builtin_clz(c.r[rm]) : 32;
        return;
    }
    // ── REV/REV16/RBIT/REVSH — 1111 1010 1001 Rn ──
    if ((hw1 & 0xFFF0) == 0xFA90 && (hw2 & 0xF0C0) == 0xF080) {
        uint32_t rm=hw2&0xF, rd=(hw2>>8)&0xF, v=c.r[rm];
        switch ((hw2>>4)&3) {
            case 0: c.r[rd]=__builtin_bswap32(v); break;
            case 1: c.r[rd]=((v&0xFF00FF00)>>8)|((v&0x00FF00FF)<<8); break;
            case 2: { uint32_t r=0; for(int i=0;i<32;i++) if(v&(1u<<i)) r|=1u<<(31-i); c.r[rd]=r; } break;
            case 3: c.r[rd]=(int32_t)(int16_t)__builtin_bswap16((uint16_t)v); break;
        }
        return;
    }
    // ── SXTB/SXTH/UXTB/UXTH (+AB/AH accumulate) — 1111 1010 0xxx Rn ──
    if ((hw1 & 0xFF80) == 0xFA00 && (hw2 & 0xF080) == 0xF080) {
        uint32_t rm=hw2&0xF, rd=(hw2>>8)&0xF, rn=hw1&0xF, rot=((hw2>>4)&3)*8;
        uint32_t v = rot ? ((c.r[rm]>>rot)|(c.r[rm]<<(32-rot))) : c.r[rm], ext;
        switch ((hw1>>4)&7) {
            case 0: ext=(int32_t)(int16_t)v; break;   // SXTH
            case 1: ext=v&0xFFFF; break;              // UXTH
            case 4: ext=(int32_t)(int8_t)v; break;    // SXTB
            case 5: ext=v&0xFF; break;                // UXTB
            default: ext=v;
        }
        c.r[rd] = (rn==15) ? ext : c.r[rn]+ext;
        return;
    }

    compatLogFmt("arm32: UNIMPL Thumb2 %04x %04x pc=0x%x", hw1, hw2, pc);
    c.halt = true; c.halt_pc = pc;
}

// ── Thumb step (16-bit + 32-bit Thumb-2) ────────────────────────────────────
static void stepThumb(CpuState& c) {
    uint32_t pc = c.r[15];
    uint16_t op = *(uint16_t*)toHost(pc);
    bool is32 = (op & 0xE000) == 0xE000 && (op & 0x1800) != 0;

    // Inside an IT block, this instruction executes conditionally. Advance the
    // IT state regardless, and skip (just step PC) when its condition fails.
    bool inIT = c.itstate != 0;
    if (inIT) {
        uint32_t cond = c.itstate >> 4;
        bool exec = condPass(c, cond);
        c.itstate = (c.itstate & 7) ? ((c.itstate & 0xE0) | ((c.itstate << 1) & 0x1F)) : 0;
        if (!exec) { c.r[15] = pc + (is32 ? 4 : 2); return; }
    }

    c.r[15] = pc + 2;

    // 32-bit Thumb-2 (respects its own S bits, so no implicit-flag suppression)
    if (is32) { g_it_suppress = false; stepThumb32(c, pc, op); return; }

    // 16-bit inside an IT block: suppress implicit flag updates (compare ops
    // below re-enable them, since setting flags is their whole purpose).
    g_it_suppress = inIT;

    // IT (if-then): set up conditional execution for the next 1-4 instructions.
    if ((op & 0xFF00) == 0xBF00 && (op & 0x000F) != 0) { c.itstate = op & 0xFF; return; }
    // NOP/hints (BF00, and the 16-bit hint space) — ignore.
    if ((op & 0xFF0F) == 0xBF00) return;
    // Push/Pop
    if ((op & 0xFE00) == 0xB400) {          // PUSH
        uint32_t list = (op & 0xFF) | ((op & 0x100) ? 0x4000 : 0);   // R bit → LR
        uint32_t n = __builtin_popcount(list), a = c.r[13] - n*4; c.r[13] = a;
        for (int i=0;i<15;i++) if (list&(1<<i)) { wr32(a, c.r[i]); a+=4; }
        return;
    }
    if ((op & 0xFE00) == 0xBC00) {          // POP
        uint32_t list = (op & 0xFF) | ((op & 0x100) ? 0x8000 : 0);   // R bit → PC
        uint32_t a = c.r[13];
        for (int i=0;i<16;i++) if (list&(1<<i)) { c.r[i]=rd32(a); a+=4; }
        c.r[13] = a;
        if (list & 0x8000) { c.cpsr=(c.r[15]&1)?(c.cpsr|C_T):(c.cpsr&~C_T); c.r[15]&=~1u; }
        return;
    }
    // BX / BLX register
    if ((op & 0xFF00) == 0x4700) {
        uint32_t rm = c.r[(op>>3)&0xF];
        if (op & 0x80) c.r[14] = (pc + 2) | 1;              // BLX
        c.cpsr = (rm&1)?(c.cpsr|C_T):(c.cpsr&~C_T);
        c.r[15] = rm & ~1u;
        return;
    }
    // Unconditional branch B (11100)
    if ((op & 0xF800) == 0xE000) {
        int32_t off = ((int32_t)(op & 0x7FF) << 21) >> 20;
        c.r[15] = pc + 4 + off;
        return;
    }
    // Conditional branch B<cc> — and SVC (0xDF__), UDF (0xDE__)
    if ((op & 0xF000) == 0xD000) {
        uint32_t cond = (op>>8)&0xF;
        if (cond == 0xF) { execSyscall(c); return; }               // SVC
        if (cond == 0xE) { /* UDF permanently undefined */ return; }
        if (condPass(c, cond)) { int32_t off=((int32_t)(op&0xFF)<<24)>>23; c.r[15]=pc+4+off; }
        return;
    }
    // MOV/CMP/ADD/SUB immediate (001xx)
    if ((op & 0xE000) == 0x2000) {
        uint32_t sub=(op>>11)&3, rd=(op>>8)&7, imm=op&0xFF;
        switch (sub) {
            case 0: c.r[rd]=imm; setNZ(c,imm); break;                       // MOV
            case 1: g_it_suppress=false; addWithCarry(c, c.r[rd], ~imm, 1, true); break; // CMP
            case 2: c.r[rd]=addWithCarry(c, c.r[rd], imm, 0, true); break;  // ADD
            case 3: c.r[rd]=addWithCarry(c, c.r[rd], ~imm, 1, true); break; // SUB
        }
        return;
    }
    // Add/sub register or 3-bit immediate (00011)
    if ((op & 0xF800) == 0x1800) {
        uint32_t rd=op&7, rn=(op>>3)&7, arg=(op>>6)&7;
        bool imm=(op>>10)&1, sub=(op>>9)&1;
        uint32_t b = imm ? arg : c.r[arg];
        c.r[rd] = sub ? addWithCarry(c, c.r[rn], ~b, 1, true)
                      : addWithCarry(c, c.r[rn], b, 0, true);
        return;
    }
    // Shift by immediate: LSL/LSR/ASR (000xx, not 00011)
    if ((op & 0xE000) == 0x0000) {
        uint32_t rd=op&7, rm=(op>>3)&7, amt=(op>>6)&0x1F, type=(op>>11)&3;
        bool co=cf(c); uint32_t v=shiftImm(c, c.r[rm], type, amt, true, co);
        c.r[rd]=v; setNZ(c,v); setC(c,co);
        return;
    }
    // ALU register ops (010000)
    if ((op & 0xFC00) == 0x4000) {
        uint32_t rd=op&7, rm=(op>>3)&7, aop=(op>>6)&0xF, a=c.r[rd], b=c.r[rm];
        bool co=cf(c); uint32_t r=a;
        switch (aop) {
            case 0x0: r=a&b; setNZ(c,r); break;                        // AND
            case 0x1: r=a^b; setNZ(c,r); break;                        // EOR
            case 0x2: r=shiftImm(c,a,0,b&0xFF,true,co); setNZ(c,r); setC(c,co); break; // LSL reg
            case 0x3: r=shiftImm(c,a,1,b&0xFF,true,co); setNZ(c,r); setC(c,co); break; // LSR reg
            case 0x4: r=shiftImm(c,a,2,b&0xFF,true,co); setNZ(c,r); setC(c,co); break; // ASR reg
            case 0x5: r=addWithCarry(c,a,b,cf(c),true); break;         // ADC
            case 0x6: r=addWithCarry(c,a,~b,cf(c),true); break;        // SBC
            case 0x7: r=shiftImm(c,a,3,b&0xFF,true,co); setNZ(c,r); setC(c,co); break; // ROR reg
            case 0x8: g_it_suppress=false; setNZ(c,a&b); return;       // TST (no wb)
            case 0x9: r=addWithCarry(c,0,~b,1,true); break;            // NEG (RSB #0)
            case 0xA: g_it_suppress=false; addWithCarry(c,a,~b,1,true); return; // CMP (no wb)
            case 0xB: g_it_suppress=false; addWithCarry(c,a,b,0,true); return;  // CMN (no wb)
            case 0xC: r=a|b; setNZ(c,r); break;                        // ORR
            case 0xD: r=a*b; setNZ(c,r); break;                        // MUL
            case 0xE: r=a&~b; setNZ(c,r); break;                       // BIC
            case 0xF: r=~b; setNZ(c,r); break;                         // MVN
        }
        c.r[rd]=r;
        return;
    }
    // Hi-register ADD/CMP/MOV (010001) — BX/BLX (0x4700) already handled above
    if ((op & 0xFC00) == 0x4400) {
        uint32_t aop=(op>>8)&3, rd=((op&0x80)>>4)|(op&7), rm=(op>>3)&0xF;
        uint32_t vm = (rm==15)?(pc+4):c.r[rm];
        switch (aop) {
            case 0: { uint32_t v=c.r[rd]+vm; if(rd==15){c.r[15]=v&~1u;} else c.r[rd]=v; } return; // ADD
            case 1: g_it_suppress=false; addWithCarry(c, c.r[rd], ~vm, 1, true); return; // CMP (hi)
            case 2: if(rd==15){c.cpsr=(vm&1)?(c.cpsr|C_T):(c.cpsr&~C_T); c.r[15]=vm&~1u;} else c.r[rd]=vm; return; // MOV
        }
        return;
    }
    // LDR literal (PC-relative) — 01001
    if ((op & 0xF800) == 0x4800) {
        uint32_t rd=(op>>8)&7, imm=(op&0xFF)*4;
        c.r[rd] = rd32(((pc+4)&~3u) + imm);
        return;
    }
    // Load/store register offset — 0101
    if ((op & 0xF000) == 0x5000) {
        uint32_t rd=op&7, rn=(op>>3)&7, rm=(op>>6)&7, opc=(op>>9)&7;
        uint32_t addr=c.r[rn]+c.r[rm];
        switch (opc) {
            case 0: wr32(addr, c.r[rd]); break;                        // STR
            case 1: wr16(addr, (uint16_t)c.r[rd]); break;              // STRH
            case 2: wr8(addr, (uint8_t)c.r[rd]); break;                // STRB
            case 3: c.r[rd]=(int32_t)(int8_t)rd8(addr); break;         // LDRSB
            case 4: c.r[rd]=rd32(addr); break;                         // LDR
            case 5: c.r[rd]=rd16(addr); break;                         // LDRH
            case 6: c.r[rd]=rd8(addr); break;                          // LDRB
            case 7: c.r[rd]=(int32_t)(int16_t)rd16(addr); break;       // LDRSH
        }
        return;
    }
    // Load/store word/byte immediate offset — 011
    if ((op & 0xE000) == 0x6000) {
        uint32_t rd=op&7, rn=(op>>3)&7, imm=(op>>6)&0x1F;
        bool byte=(op>>12)&1, load=(op>>11)&1;
        uint32_t addr=c.r[rn]+(byte?imm:imm*4);
        if (load) c.r[rd]=byte?rd8(addr):rd32(addr);
        else      { if(byte) wr8(addr,(uint8_t)c.r[rd]); else wr32(addr,c.r[rd]); }
        return;
    }
    // Load/store halfword immediate — 1000
    if ((op & 0xF000) == 0x8000) {
        uint32_t rd=op&7, rn=(op>>3)&7, imm=((op>>6)&0x1F)*2;
        uint32_t addr=c.r[rn]+imm;
        if ((op>>11)&1) c.r[rd]=rd16(addr); else wr16(addr,(uint16_t)c.r[rd]);
        return;
    }
    // SP-relative load/store — 1001
    if ((op & 0xF000) == 0x9000) {
        uint32_t rd=(op>>8)&7, imm=(op&0xFF)*4, addr=c.r[13]+imm;
        if ((op>>11)&1) c.r[rd]=rd32(addr); else wr32(addr,c.r[rd]);
        return;
    }
    // ADR / ADD sp (load address) — 1010
    if ((op & 0xF000) == 0xA000) {
        uint32_t rd=(op>>8)&7, imm=(op&0xFF)*4;
        c.r[rd] = ((op>>11)&1) ? (c.r[13]+imm) : (((pc+4)&~3u)+imm);
        return;
    }
    // ADD/SUB sp immediate — 10110000
    if ((op & 0xFF00) == 0xB000) {
        uint32_t imm=(op&0x7F)*4;
        c.r[13] = (op&0x80) ? c.r[13]-imm : c.r[13]+imm;
        return;
    }
    // Sign/zero extend (SXTH/SXTB/UXTH/UXTB) — 1011 0010
    if ((op & 0xFF00) == 0xB200) {
        uint32_t rd=op&7, rm=(op>>3)&7, v=c.r[rm];
        switch ((op>>6)&3) {
            case 0: c.r[rd]=(int32_t)(int16_t)v; break;   // SXTH
            case 1: c.r[rd]=(int32_t)(int8_t)v; break;    // SXTB
            case 2: c.r[rd]=v&0xFFFF; break;              // UXTH
            case 3: c.r[rd]=v&0xFF; break;                // UXTB
        }
        return;
    }
    // LDMIA/STMIA — 1100
    if ((op & 0xF000) == 0xC000) {
        uint32_t rn=(op>>8)&7, list=op&0xFF, a=c.r[rn]; bool load=(op>>11)&1;
        for (int i=0;i<8;i++) if (list&(1<<i)) { if(load) c.r[i]=rd32(a); else wr32(a,c.r[i]); a+=4; }
        if (!(load && (list&(1<<rn)))) c.r[rn]=a;   // writeback unless loaded rn
        return;
    }

    // REV/REV16/REVSH — 1011 1010
    if ((op & 0xFF00) == 0xBA00) {
        uint32_t rd=op&7, rm=(op>>3)&7, v=c.r[rm];
        switch ((op>>6)&3) {
            case 0: c.r[rd]=__builtin_bswap32(v); break;                                 // REV
            case 1: c.r[rd]=((v&0xFF00FF00)>>8)|((v&0x00FF00FF)<<8); break;               // REV16
            case 3: c.r[rd]=(int32_t)(int16_t)__builtin_bswap16((uint16_t)v); break;      // REVSH
        }
        return;
    }
    // CBZ/CBNZ — 1011 x0x1
    if ((op & 0xF500) == 0xB100) {
        uint32_t rn=op&7; bool nz=(op>>11)&1;
        uint32_t imm=(((op>>9)&1)<<6)|(((op>>3)&0x1F)<<1);
        if ((c.r[rn]==0) != nz) c.r[15] = pc + 4 + imm;
        return;
    }

    compatLogFmt("arm32: UNIMPL Thumb op=0x%04x pc=0x%x", op, pc);
    c.halt = true; c.halt_pc = pc;
}

void cpuRun(CpuState& c) {
    g_abort = false;
    uint64_t guard = 0;
    const uint64_t t0 = armGetSystemTick();
    const uint64_t kMaxNs = 12ull * 1000 * 1000 * 1000;   // 12s wall-clock ceiling

    while (!c.halt) {
        // Trap sentinel PCs → native bridge.
        if ((c.r[15] & 0xFFF00000) == (A32_SENTINEL_BASE & 0xFFF00000)) {
            bridgeCall(c, c.r[15]);
            continue;
        }
        if (c.r[15] == A32_RETURN_TRAP || c.r[15] == 0) break;   // guest fn returned

        // PC must point at real guest code — never let a wild branch send the
        // fetch into host memory outside the region (fetch reads toHost(pc)).
        if (c.r[15] < 0x1000 || (uint64_t)c.r[15] + 4 > g_region) {
            compatLogFmt("arm32: bad PC=0x%x — halt (protects the system)", c.r[15]);
            c.halt = true; c.halt_pc = c.r[15]; break;
        }

        if (c.cpsr & C_T) stepThumb(c); else stepArm(c);

        // Periodic safety checks (cheap: once every 64k instructions).
        if ((++guard & 0xFFFF) == 0) {
            if (g_abort) { compatLog("arm32: aborted by UI — stop"); break; }
            if (armTicksToNs(armGetSystemTick() - t0) > kMaxNs) {
                compatLogFmt("arm32: %llus wall-clock watchdog — stop (pc=0x%x)",
                             (unsigned long long)(kMaxNs/1000000000ull), c.r[15]);
                break;
            }
        }
        if (guard > 400000000ull) { compatLog("arm32: 400M-insn watchdog — stop"); break; }
    }
    if (c.halt) compatLogFmt("arm32: HALTED at pc=0x%x", c.halt_pc);
}

}  // namespace a32
