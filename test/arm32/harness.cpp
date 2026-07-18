// Host test harness for the ARM32 interpreter. Compiles source/arm32/cpu.cpp
#include <initializer_list>
// against mock switch.h/loader.h + stub memory/bridge, so instruction semantics
// can be validated on a dev machine without Switch hardware.
//
//   g++ -std=c++17 -I test/arm32/mock -I include \
//       test/arm32/harness.cpp source/arm32/cpu.cpp -o /tmp/a32test && /tmp/a32test
#include "arm32/arm32_internal.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

// ── Mocks the interpreter links against ──
void compatLog(const char* m) { printf("  [log] %s\n", m); }
void compatLogFmt(const char* f, ...) { va_list a; va_start(a, f); printf("  [log] "); vprintf(f, a); printf("\n"); va_end(a); }

namespace a32 {
uint8_t* g_base = nullptr;
size_t   g_region = 0;
uint8_t* toHost(uint32_t g) { return g_base + g; }
uint32_t toGuest(const void* h) { return (uint32_t)((const uint8_t*)h - g_base); }
bool guestValid(uint32_t a, uint32_t l) { return g_base && (uint64_t)a + l <= g_region; }
uint32_t bridgeRegister(const char*) { return A32_SENTINEL_BASE; }
// Test bridge: r0 = 0xB0000000 | (import idx) so tests can see a call happened.
void bridgeCall(CpuState& c, uint32_t sentinel) {
    c.r[0] = 0xB0000000u | ((sentinel - A32_SENTINEL_BASE) / 4);
    c.r[15] = c.r[14];
    c.cpsr = (c.r[14] & 1) ? (c.cpsr | 0x20) : (c.cpsr & ~0x20u);
    c.r[15] &= ~1u;
}
}  // namespace a32
using namespace a32;

static int g_pass = 0, g_fail = 0;

// Run a program (words) placed at 0x1000 with given initial regs; return CpuState.
static CpuState runProg(const uint32_t* words, int n, bool thumb, uint32_t r0=0, uint32_t r1=0, uint32_t r2=0) {
    memset(g_base + 0x1000, 0, 0x1000);
    memcpy(g_base + 0x1000, words, n * 4);
    CpuState c = {};
    c.r[0]=r0; c.r[1]=r1; c.r[2]=r2;
    c.r[13] = 0x00200000;               // SP
    c.r[14] = A32_RETURN_TRAP;
    c.r[15] = 0x1000;
    c.cpsr  = 0x10 | (thumb ? 0x20 : 0);
    cpuRun(c);
    return c;
}
static void check(const char* name, uint32_t got, uint32_t want) {
    if (got == want) { g_pass++; /*printf("  ok  %s\n", name);*/ }
    else { g_fail++; printf("  FAIL %s: got 0x%x want 0x%x\n", name, got, want); }
}

int main() {
    g_region = 16 * 1024 * 1024;
    g_base = (uint8_t*)calloc(1, g_region);

    // ── ARM: MOV imm, ADD imm, BX lr ──
    { uint32_t p[] = {0xe3a00005 /*mov r0,#5*/, 0xe2800003 /*add r0,r0,#3*/, 0xe12fff1e /*bx lr*/};
      CpuState c = runProg(p, 3, false); check("arm mov+add", c.r[0], 8); }

    // ── ARM: SUB, flags via CMP + conditional MOV ──
    { uint32_t p[] = {0xe3a0000a /*mov r0,#10*/, 0xe2400004 /*sub r0,r0,#4*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm sub", c.r[0], 6); }

    // ── ARM: ORR / AND / EOR ──
    { uint32_t p[] = {0xe3a000f0 /*mov r0,#0xf0*/, 0xe380000f /*orr r0,r0,#0xf*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm orr", c.r[0], 0xff); }
    { uint32_t p[] = {0xe3a000ff /*mov r0,#0xff*/, 0xe20000f0 /*and r0,r0,#0xf0*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm and", c.r[0], 0xf0); }

    // ── ARM: LSL shift (mov r0, r0, lsl #4) ──
    { uint32_t p[] = {0xe3a00003 /*mov r0,#3*/, 0xe1a00200 /*lsl r0,r0,#4*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm lsl", c.r[0], 48); }

    // ── ARM: MOVW/MOVT ──
    { uint32_t p[] = {0xe3010234 /*movw r0,#0x1234*/, 0xe3400678 /*movt r0,#0x678*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm movw/movt", c.r[0], 0x06781234); }

    // ── ARM: STR then LDR (round-trip through memory) ──
    { uint32_t p[] = {0xe3a0002a /*mov r0,#42*/, 0xe58d0010 /*str r0,[sp,#16]*/,
                      0xe59d1010 /*ldr r1,[sp,#16]*/, 0xe12fff1e};
      CpuState c = runProg(p, 4, false); check("arm str/ldr", c.r[1], 42); }

    // ── ARM: PUSH/POP via STMDB/LDMIA ──
    { uint32_t p[] = {0xe3a00007 /*mov r0,#7*/, 0xe92d0001 /*push {r0}*/,
                      0xe3a00000 /*mov r0,#0*/, 0xe8bd0002 /*pop {r1}*/, 0xe12fff1e};
      CpuState c = runProg(p, 5, false); check("arm push/pop", c.r[1], 7); }

    // ── ARM: BL to a sentinel import (bridge) ── mov r0,#1; b to a sentinel? use blx? skip.

    // ── ARM: MUL (6*7=42) ──
    { uint32_t p[] = {0xe3a00006 /*mov r0,#6*/, 0xe3a01007 /*mov r1,#7*/,
                      0xe0020190 /*mul r2,r0,r1*/, 0xe1a00002 /*mov r0,r2*/, 0xe12fff1e};
      CpuState c = runProg(p, 5, false); check("arm mul", c.r[0], 42); }
    // ── ARM: UMULL (0x10000 * 0x10000 = 0x1_0000_0000) ──
    { uint32_t p[] = {0xe3a00801 /*mov r0,#0x10000*/, 0xe1a01000 /*mov r1,r0*/,
                      0xe0832190 /*umull r2,r3,r0,r1*/, 0xe1a00003 /*mov r0,r3*/, 0xe12fff1e};
      CpuState c = runProg(p, 5, false); check("arm umull hi", c.r[0], 1); }
    // ── ARM: reg-shift-reg (mov r0, r1, lsl r2) 3<<4=48 ──
    { uint32_t p[] = {0xe3a01003 /*mov r1,#3*/, 0xe3a02004 /*mov r2,#4*/,
                      0xe1a00211 /*lsl r0,r1,r2*/, 0xe12fff1e};
      CpuState c = runProg(p, 4, false); check("arm lsl reg", c.r[0], 48); }
    // ── ARM: LDRH/STRH round-trip ──
    { uint32_t p[] = {0xe3a0000f /*mov r0,#15*/, 0xe3a01a01 /*mov r1,#0x1000*/,
                      0xe1c100b0 /*strh r0,[r1]*/, 0xe1d120b0 /*ldrh r2,[r1]*/,
                      0xe1a00002 /*mov r0,r2*/, 0xe12fff1e};
      CpuState c = runProg(p, 6, false); check("arm strh/ldrh", c.r[0], 15); }
    // ── ARM: LDRB register offset ──
    { uint32_t p[] = {0xe3a000ab /*mov r0,#0xab*/, 0xe3a01a01 /*mov r1,#0x1000*/,
                      0xe5c10000 /*strb r0,[r1]*/, 0xe5d12000 /*ldrb r2,[r1]*/,
                      0xe1a00002 /*mov r0,r2*/, 0xe12fff1e};
      CpuState c = runProg(p, 6, false); check("arm strb/ldrb", c.r[0], 0xab); }

    // ── Thumb: MOV imm, ADD imm, BX lr ──
    { uint16_t t[] = {0x2005 /*movs r0,#5*/, 0x3003 /*adds r0,#3*/, 0x4770 /*bx lr*/};
      uint32_t buf[2]; memcpy(buf, t, sizeof(t));
      CpuState c = runProg(buf, 2, true); check("thumb mov+add", c.r[0], 8); }

    // ── Thumb: SUB imm ──
    { uint16_t t[] = {0x200a /*movs r0,#10*/, 0x3804 /*subs r0,#4*/, 0x4770};
      uint32_t buf[2]; memcpy(buf, t, sizeof(t));
      CpuState c = runProg(buf, 2, true); check("thumb sub", c.r[0], 6); }

    // Helper to run a thumb program given as uint16 array.
    auto runT = [](std::initializer_list<uint16_t> il, uint32_t r0=0, uint32_t r1=0, uint32_t r2=0) {
        uint32_t buf[32]; int i=0; for (uint16_t w : il) ((uint16_t*)buf)[i++]=w;
        // pad to even for the trailing bx lr word boundary
        return runProg(buf, (i+1)/2, true, r0, r1, r2);
    };
    // shift imm: lsl r0, r1, #4  (r1=3 → 48)
    { CpuState c = runT({0x0108 /*lsl r0,r1,#4*/, 0x4770}, 0, 3); check("thumb lsl imm", c.r[0], 48); }
    // add/sub register: adds r0, r1, r2 (2+3=5)
    { CpuState c = runT({0x1888 /*adds r0,r1,r2*/, 0x4770}, 0, 2, 3); check("thumb add reg", c.r[0], 5); }
    // add 3-bit imm: adds r0, r1, #5
    { CpuState c = runT({0x1d48 /*adds r0,r1,#5*/, 0x4770}, 0, 10); check("thumb add imm3", c.r[0], 15); }
    // ALU: orr r0, r1 (0xf0 | 0x0f)
    { CpuState c = runT({0x2000 /*movs r0,#0*/, 0x30f0 /*adds r0,#0xf0*/, 0x4308 /*orrs r0,r1*/, 0x4770}, 0, 0x0f);
      check("thumb orr", c.r[0], 0xff); }
    // ALU: mul r0, r1 (6*7=42) — muls r0, r1, r0
    { CpuState c = runT({0x2006 /*movs r0,#6*/, 0x4348 /*muls r0,r1,r0*/, 0x4770}, 0, 7); check("thumb mul", c.r[0], 42); }
    // ALU: mvn r0, r1
    { CpuState c = runT({0x43c8 /*mvns r0,r1*/, 0x4770}, 0, 0); check("thumb mvn", c.r[0], 0xffffffff); }
    // hi-reg mov: mov r0, r8 (set r8 first is hard; test mov r0,r1 via hi form 0x4608)
    { CpuState c = runT({0x4608 /*mov r0,r1*/, 0x4770}, 0, 0x1234); check("thumb hi mov", c.r[0], 0x1234); }
    // sp-relative store/load: str r0,[sp,#8]; ldr r1,[sp,#8]
    { CpuState c = runT({0x2063 /*movs r0,#0x63*/, 0x9002 /*str r0,[sp,#8]*/, 0x9902 /*ldr r1,[sp,#8]*/, 0x4770});
      check("thumb sp str/ldr", c.r[1], 0x63); }
    // load/store byte imm: strb + ldrb
    { CpuState c = runT({0x20aa /*movs r0,#0xaa*/, 0x7008 /*strb r0,[r1,#0]*/, 0x7808 /*ldrb r0,[r1,#0]*/, 0x4770}, 0, 0x1000);
      check("thumb strb/ldrb", c.r[0], 0xaa); }
    // add sp imm then read: add sp,#16; sub sp,#16; (sp unchanged) — just ensure no halt
    { CpuState c = runT({0xb004 /*add sp,#16*/, 0xb084 /*sub sp,#16*/, 0x4770}); check("thumb add/sub sp halt", c.halt?1:0, 0); }
    // ADR: adr r0, . (pc-relative) — just ensure it runs
    { CpuState c = runT({0xa000 /*add r0,pc,#0*/, 0x4770}); check("thumb adr halt", c.halt?1:0, 0); }
    // sxtb: r1=0x80 → r0 = 0xffffff80
    { CpuState c = runT({0x2080 /*movs r0,#0x80*/, 0xb241 /*sxtb r1,r0*/, 0x4770}); check("thumb sxtb", c.r[1], 0xffffff80u); }

    // ── Thumb-2 32-bit (encodings verified via capstone) ──
    // movw r0,#0x1234 ; movt r0,#0x5e78 → 0x5e781234
    { CpuState c = runT({0xf241,0x2034 /*movw r0,#0x1234*/, 0xf6c5,0x6078 /*movt r0,#0x5e78*/, 0x4770});
      check("t2 movw/movt", c.r[0], 0x5e781234u); }
    // add.w r0,r1,#5 (r1=10)
    { CpuState c = runT({0xf101,0x0005, 0x4770}, 0, 10); check("t2 add.w", c.r[0], 15); }
    // sub.w r0,r1,#5 (r1=10)
    { CpuState c = runT({0xf1a1,0x0005, 0x4770}, 0, 10); check("t2 sub.w", c.r[0], 5); }
    // addw r0,r1,#0x64 (r1=1)
    { CpuState c = runT({0xf201,0x0064, 0x4770}, 0, 1); check("t2 addw", c.r[0], 101); }
    // str.w r0,[r1] ; ldr.w r2,[r1] (r0=0xdead, r1=0x1000)
    { CpuState c = runT({0xf8c1,0x0000 /*str.w r0,[r1]*/, 0xf8d1,0x2000 /*ldr.w r2,[r1]*/, 0x4602 /*mov r2->? */, 0x4770}, 0xdead, 0x1000);
      check("t2 str/ldr.w", c.r[2], 0xdead); }
    // ubfx r0,r1,#16,#8 (r1=0x00AB0000 → 0xAB)
    { CpuState c = runT({0xf3c1,0x4007, 0x4770}, 0, 0x00AB0000u); check("t2 ubfx", c.r[0], 0xAB); }
    // orr.w r0,r1,r2 (r1=0xf0, r2=0x0f)
    { CpuState c = runT({0xea41,0x0002, 0x4770}, 0, 0xf0, 0x0f); check("t2 orr.w", c.r[0], 0xff); }
    // mul r0,r1,r2 (r1=6, r2=7)
    { CpuState c = runT({0xfb01,0xf002, 0x4770}, 0, 6, 7); check("t2 mul", c.r[0], 42); }
    // udiv r0,r1,r2 (r1=100, r2=7 → 14)
    { CpuState c = runT({0xfbb1,0xf0f2, 0x4770}, 0, 100, 7); check("t2 udiv", c.r[0], 14); }

    // ── VFP (float): vmov s0,r0; vmov s1,r1; OP s2,s0,s1; vmov r0,s2 ──
    const uint32_t F3=0x40400000, F2=0x40000000;
    { CpuState c = runT({0xee00,0x0a10 /*vmov s0,r0*/, 0xee00,0x1a90 /*vmov s1,r1*/,
                         0xee30,0x1a20 /*vadd.f32 s2,s0,s1*/, 0xee11,0x0a10 /*vmov r0,s2*/, 0x4770}, F3, F2);
      check("vfp vadd (3+2=5)", c.r[0], 0x40a00000u); }
    { CpuState c = runT({0xee00,0x0a10, 0xee00,0x1a90, 0xee30,0x1a60 /*vsub*/, 0xee11,0x0a10, 0x4770}, F3, F2);
      check("vfp vsub (3-2=1)", c.r[0], 0x3f800000u); }
    { CpuState c = runT({0xee00,0x0a10, 0xee00,0x1a90, 0xee20,0x1a20 /*vmul*/, 0xee11,0x0a10, 0x4770}, F3, F2);
      check("vfp vmul (3*2=6)", c.r[0], 0x40c00000u); }
    // vcvt.f32.s32 s0,s0 then back: int 7 → float 7.0
    { CpuState c = runT({0xee00,0x0a10 /*vmov s0,r0*/, 0xeeb8,0x0ac0 /*vcvt.f32.s32 s0,s0*/,
                         0xee10,0x0a10 /*vmov r0,s0*/, 0x4770}, 7);
      check("vfp vcvt int->float", c.r[0], 0x40e00000u /*7.0f*/); }
    // vldr/vstr round trip: store f2u(5.0) via r0→s0→mem→s1→r1
    { CpuState c = runT({0xee00,0x0a10 /*vmov s0,r0*/, 0xed81,0x0a00 /*vstr s0,[r1]*/,
                         0xed91,0x1a00 /*vldr s2,[r1]? */, 0x4770}, 0x40a00000u, 0x1000);
      // read back what's in memory at 0x1000
      check("vfp vstr mem", *(uint32_t*)(g_base+0x1000), 0x40a00000u); }

    // ── IT block: itt eq ; moveq r0,#1 ; moveq r0,#2 (cond true when r0==r1) ──
    // set flags with cmp r0,r1 (equal), then IT EQ executing two movs
    { CpuState c = runT({0x4288 /*cmp r0,r1*/, 0xbf04 /*itt eq*/, 0x2001 /*movs r0,#1*/, 0x2002 /*mov r0,#2*/, 0x4770}, 5, 5);
      check("it-block eq taken", c.r[0], 2); }
    // IT with false condition: cmp r0,r1 (not equal) → itt eq skips both
    { CpuState c = runT({0x4288 /*cmp r0,r1*/, 0xbf04 /*itt eq*/, 0x2001, 0x2002, 0x4770}, 5, 9);
      check("it-block eq skipped", c.r[0], 5); }
    // CBZ r0,#0x1006: at 0x1000 → skips movs(0x1002) + nop(0x1004) to bx lr(0x1006)
    { CpuState c = runT({0xb108 /*cbz r0*/, 0x2007 /*movs r0,#7*/, 0xbf00 /*nop*/, 0x4770 /*bx lr*/}, 0);
      check("cbz taken", c.r[0], 0); }
    // REV (thumb): r1=0x11223344 → r0=0x44332211
    { CpuState c = runT({0x2000, 0xba08 /*rev r0,r1*/, 0x4770}, 0, 0x11223344u); check("thumb rev", c.r[0], 0x44332211u); }
    // ARM CLZ: r1=0x00010000 → 15
    { uint32_t p[] = {0xe3a01801 /*mov r1,#0x10000*/, 0xe16f0f11 /*clz r0,r1*/, 0xe12fff1e};
      CpuState c = runProg(p, 3, false); check("arm clz", c.r[0], 15); }

    printf("\narm32 harness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
