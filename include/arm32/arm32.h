#pragma once
#include <cstdint>
#include <cstddef>

// ─── ARM32 (AArch32) emulation layer ────────────────────────────────────────
// The Switch runs Viridite as a 64-bit process and can't execute 32-bit ARM
// code in-process, so armeabi-v7a games need their CPU emulated. This module is
// a software ARMv7-A + Thumb-2 interpreter (a JIT can come later) plus an ELF32
// loader and a calling-convention bridge that routes the game's libc/JNI calls
// back into the native arm64 shim table.
//
// Memory model: one contiguous host region backs the entire 32-bit guest
// address space via a fixed base+offset map (host = g_a32_base + guest_addr), so
// a 32-bit guest pointer is just an offset — no soft page table. The ELF, heap,
// and stack all live inside it. See A32_REGION_SIZE.

namespace a32 {

// The whole 32-bit guest lives in this window (guest addr 0 → host base).
static constexpr uint64_t A32_REGION_SIZE = 512ull * 1024 * 1024;  // 512 MiB

struct CpuState {
    uint32_t r[16];        // r0..r15 (r13=SP, r14=LR, r15=PC)
    uint32_t cpsr;         // NZCV + T(humb) bit (bit 5)
    uint64_t vfp[32];      // s0..s31 packed as d0..d15 halves (VFP/NEON) — d view
    uint8_t  itstate;      // Thumb IT block: firstcond<<4 | mask (0 = not in block)
    bool     halt;         // interpreter requested stop
    uint32_t halt_pc;      // where it stopped
};

// Is `path` a 32-bit ARM (EM_ARM, ELFCLASS32) shared object?
bool isElf32Arm(const char* path);

// Load and run an armeabi-v7a game whose main .so is `main_so_host_path`
// (already extracted to the SD card). Blocking; drives the interpreter.
// Returns 0 on clean exit, negative on setup failure. Logs via compatLog*.
int run(const char* main_so_host_path, const char* pkg);

// Ask a running interpreter to stop as soon as possible (UI thread → loader
// thread). Safe to call any time; the interpreter also self-limits by time.
void requestAbort();

// Guest<->host address translation (valid only after the region is mapped).
uint8_t* toHost(uint32_t guest);
uint32_t toGuest(const void* host);

}  // namespace a32
