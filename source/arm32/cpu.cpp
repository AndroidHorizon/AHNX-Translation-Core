// ARMv7-A + Thumb interpreter. This is an intentionally partial first cut: the
// common instruction classes are implemented with correct NZCV flags, and
// anything else logs its encoding and halts, so hardware runs drive which
// instructions to add next (the same log-driven method the shim table used).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <switch.h>

namespace a32 {

// Safety: the interpreter runs on the loader thread. A runaway loop (interpreter
// bug or wild PC) must never wedge the console — bound it by both instruction
// count and wall-clock time, and let the UI thread abort it.
static volatile bool g_abort = false;
void requestAbort() { g_abort = true; }

// CPSR bit helpers.
enum { C_N=1u<<31, C_Z=1u<<30, C_C=1u<<29, C_V=1u<<28, C_T=1u<<5 };
static inline bool nf(CpuState& c){return c.cpsr&C_N;} static inline bool zf(CpuState& c){return c.cpsr&C_Z;}
static inline bool cf(CpuState& c){return c.cpsr&C_C;} static inline bool vf(CpuState& c){return c.cpsr&C_V;}
static inline void setNZ(CpuState& c, uint32_t r){ c.cpsr = (c.cpsr&~(C_N|C_Z)) | (r&0x80000000?C_N:0) | (r==0?C_Z:0); }
static inline void setC(CpuState& c, bool v){ c.cpsr = v ? c.cpsr|C_C : c.cpsr&~C_C; }
static inline void setV(CpuState& c, bool v){ c.cpsr = v ? c.cpsr|C_V : c.cpsr&~C_V; }

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

// ── ARM (32-bit) step ───────────────────────────────────────────────────────
static void stepArm(CpuState& c) {
    uint32_t pc = c.r[15];
    uint32_t insn = rd32(pc);
    c.r[15] = pc + 4;                       // default advance; PC reads as +8 in ARM
    uint32_t cond = insn >> 28;
    if (cond != 0xF && !condPass(c, cond)) return;

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
    // Data processing (bits 27-26 == 00), immediate or register shift (no reg-shift-by-reg yet)
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
        } else { compatLogFmt("arm32: UNIMPL dp reg-shift-reg pc=0x%x insn=0x%08x", pc, insn); c.halt = true; c.halt_pc = pc; return; }
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
    // Single data transfer LDR/STR (bits 27-26 == 01), immediate offset only
    if ((insn & 0x0C000000) == 0x04000000 && (insn & 0x02000000) == 0) {
        bool P=(insn>>24)&1, U=(insn>>23)&1, B=(insn>>22)&1, W=(insn>>21)&1, L=(insn>>20)&1;
        uint32_t rn=(insn>>16)&0xF, rd=(insn>>12)&0xF, imm=insn&0xFFF;
        uint32_t base = (rn==15) ? (pc+8) : c.r[rn];
        uint32_t addr = P ? (U?base+imm:base-imm) : base;
        if (!guestValid(addr, B?1:4)) { compatLogFmt("arm32: bad LDR/STR addr=0x%x pc=0x%x", addr, pc); c.halt=true; c.halt_pc=pc; return; }
        if (L) c.r[rd] = B ? *toHost(addr) : rd32(addr);
        else   { if (B) *toHost(addr)=(uint8_t)c.r[rd]; else wr32(addr, c.r[rd]); }
        if (!P) addr = U?base+imm:base-imm;
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

// ── Thumb step (16-bit + BL/BLX 32-bit prefix) ──────────────────────────────
static void stepThumb(CpuState& c) {
    uint32_t pc = c.r[15];
    uint16_t op = *(uint16_t*)toHost(pc);
    c.r[15] = pc + 2;

    // BL / BLX 32-bit pair (F000..FFFF prefix)
    if ((op & 0xF800) == 0xF000) {
        uint16_t op2 = *(uint16_t*)toHost(pc + 2);
        c.r[15] = pc + 4;
        int32_t off = ((int32_t)(op & 0x7FF) << 21) >> 9;   // high part sign-extended
        off |= (op2 & 0x7FF) << 1;
        uint32_t target = pc + 4 + off;
        c.r[14] = (pc + 4) | 1;                             // LR (Thumb)
        if ((op2 & 0xF800) == 0xE800) { c.cpsr &= ~C_T; target &= ~3u; }  // BLX → ARM
        c.r[15] = target & ~1u;
        return;
    }
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
    // Conditional branch B<cc>
    if ((op & 0xF000) == 0xD000) {
        uint32_t cond = (op>>8)&0xF;
        if (cond == 0xE || cond == 0xF) { /* undef/SVC */ }
        else if (condPass(c, cond)) { int32_t off=((int32_t)(op&0xFF)<<24)>>23; c.r[15]=pc+4+off; }
        return;
    }
    // MOV/CMP/ADD/SUB immediate (001xx)
    if ((op & 0xE000) == 0x2000) {
        uint32_t sub=(op>>11)&3, rd=(op>>8)&7, imm=op&0xFF;
        switch (sub) {
            case 0: c.r[rd]=imm; setNZ(c,imm); break;                       // MOV
            case 1: addWithCarry(c, c.r[rd], ~imm, 1, true); break;         // CMP
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
            case 0x8: setNZ(c,a&b); return;                            // TST (no wb)
            case 0x9: r=addWithCarry(c,0,~b,1,true); break;            // NEG (RSB #0)
            case 0xA: addWithCarry(c,a,~b,1,true); return;             // CMP (no wb)
            case 0xB: addWithCarry(c,a,b,0,true); return;              // CMN (no wb)
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
            case 1: addWithCarry(c, c.r[rd], ~vm, 1, true); return;    // CMP
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
