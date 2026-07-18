#pragma once
#include "arm32/arm32.h"
#include <cstdint>
#include <cstddef>

// Internal contract shared across the arm32 module (not part of the public API).
namespace a32 {

// ── Guest address-space layout (all offsets into the region) ────────────────
static constexpr uint32_t A32_LOAD_BASE   = 0x01000000;  // ELF load bias (16 MiB)
static constexpr uint32_t A32_HEAP_START  = 0x08000000;  // 128 MiB
static constexpr uint32_t A32_HEAP_END    = 0x1E000000;  // 480 MiB
static constexpr uint32_t A32_STACK_TOP   = 0x1FF00000;  // SP start (grows down)
static constexpr uint32_t A32_STACK_SIZE  = 0x00800000;  // 8 MiB
// Imports resolve to sentinel PCs the interpreter traps instead of executing.
// Deliberately outside the region so a stray data access never lands here.
static constexpr uint32_t A32_SENTINEL_BASE = 0xE0000000;
// LR value used when calling a guest function from the host — the interpreter
// stops cleanly when PC returns here.
static constexpr uint32_t A32_RETURN_TRAP = 0xFFFFFFF0;

// ── Memory (arm32_mem.cpp) ──────────────────────────────────────────────────
extern uint8_t* g_base;
extern size_t   g_region;
// Host path of the game's extracted assets/ dir, for the file-I/O bridge.
extern const char* g_asset_dir;
bool     memInit();
bool     guestValid(uint32_t addr, uint32_t len);
uint32_t guestAlloc(uint32_t size);
void     guestFree(uint32_t payload);
uint32_t guestRealloc(uint32_t payload, uint32_t size);

// ── ELF32 loader (elf32.cpp) ────────────────────────────────────────────────
struct LoadedElf32 {
    uint32_t load_bias   = 0;   // guest base the image was placed at
    uint32_t entry_init  = 0;   // DT_INIT (guest addr) or 0
    uint32_t init_array  = 0;   // DT_INIT_ARRAY (guest addr) or 0
    uint32_t init_count  = 0;   // number of init_array entries
    uint32_t jni_onload  = 0;   // JNI_OnLoad export (guest addr) or 0
    uint32_t min_vaddr   = 0;
    uint32_t max_vaddr   = 0;
    bool     ok          = false;
};
LoadedElf32 elf32Load(const char* host_path);
// Resolve an exported symbol by name to a guest address (0 if absent).
uint32_t elf32Sym(const char* name);

// ── Import bridge (bridge.cpp) ──────────────────────────────────────────────
// A sentinel PC was hit — run the native shim it stands for. Reads args from
// `cpu`, may allocate/translate, writes the result to r0/r1, returns to LR.
void bridgeCall(CpuState& cpu, uint32_t sentinel);
// Register the name a sentinel maps to (called during import resolution).
uint32_t bridgeRegister(const char* name);

// ── Interpreter (cpu.cpp) ───────────────────────────────────────────────────
// Run until halt (unimplemented insn, sentinel with no LR, or explicit stop).
void cpuRun(CpuState& cpu);

}  // namespace a32
