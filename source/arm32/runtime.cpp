// ARM32 runtime orchestration: set up the guest, load the ELF32, run its static
// constructors, and call JNI_OnLoad — the first bring-up milestone. Graphics,
// input, and the full JNI surface come once the interpreter covers enough of the
// instruction set to reach them (log-driven).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <cstdio>
#include <string>

namespace a32 {

static constexpr uint32_t CPSR_T = 0x20;   // Thumb bit
static std::string s_asset_dir;
const char* g_asset_dir = "";

bool isElf32Arm(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[20] = {0};
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 20) return false;
    bool elf = (h[0]==0x7f && h[1]=='E' && h[2]=='L' && h[3]=='F');
    return elf && h[4] == 1 /*ELFCLASS32*/ && h[18] == 40 /*EM_ARM*/;
}

// Call a guest function (Thumb or ARM per its low bit) with up to 4 int args,
// running the interpreter until it returns to the trap. Returns r0.
static uint32_t callGuest(CpuState& c, uint32_t func, uint32_t a0=0, uint32_t a1=0,
                          uint32_t a2=0, uint32_t a3=0) {
    c.r[0]=a0; c.r[1]=a1; c.r[2]=a2; c.r[3]=a3;
    c.r[14] = A32_RETURN_TRAP;
    c.cpsr = (func & 1) ? (c.cpsr | CPSR_T) : (c.cpsr & ~CPSR_T);
    c.r[15] = func & ~1u;
    c.halt = false;
    cpuRun(c);
    return c.r[0];
}

int run(const char* main_so_host_path, const char* pkg) {
    compatLogFmt("arm32: bring-up for %s (%s)", pkg ? pkg : "?", main_so_host_path);
    if (!memInit()) return -1;

    s_asset_dir = std::string("sdmc:/Viridite/games/") + (pkg ? pkg : "") + "/assets/";
    g_asset_dir = s_asset_dir.c_str();

    LoadedElf32 elf = elf32Load(main_so_host_path);
    if (!elf.ok) { compatLog("arm32: ELF32 load failed"); return -2; }

    CpuState cpu = {};
    cpu.r[13] = A32_STACK_TOP;          // SP
    cpu.cpsr  = 0x10;                    // user mode

    // DT_INIT
    if (elf.entry_init) {
        compatLogFmt("arm32: DT_INIT @0x%x", elf.entry_init);
        callGuest(cpu, elf.entry_init);
        if (cpu.halt) return -3;
    }
    // DT_INIT_ARRAY (static constructors)
    for (uint32_t i = 0; i < elf.init_count; i++) {
        uint32_t fn = *(uint32_t*)toHost(elf.init_array + i*4);
        if (!fn || fn == 0xffffffff) continue;
        compatLogFmt("arm32: ctor[%u/%u] @0x%x", i+1, elf.init_count, fn);
        callGuest(cpu, fn);
        if (cpu.halt) { compatLogFmt("arm32: ctor halted, stopping at %u", i); return -4; }
    }
    compatLog("arm32: constructors done");

    // JNI_OnLoad(JavaVM*, void*) — pass 0/0 for now; the JNI object model needs
    // wiring through the bridge before this does anything useful.
    if (elf.jni_onload) {
        compatLogFmt("arm32: calling JNI_OnLoad @0x%x", elf.jni_onload);
        uint32_t r = callGuest(cpu, elf.jni_onload, 0, 0);
        compatLogFmt("arm32: JNI_OnLoad returned 0x%x (halt=%d)", r, cpu.halt);
    } else {
        compatLog("arm32: no JNI_OnLoad export");
    }

    compatLog("arm32: bring-up milestone reached (interpreter foundation)");
    return 0;
}

}  // namespace a32
