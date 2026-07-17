// ─── Hill Climb Racing (com.fingersoft.hillclimb) — Cocos2d-x ───────────────
// All HCR-specific fixups live here, keyed by package + soname, so they can
// never fire for another title (e.g. the Unity path). Everything is
// signature-gated against this exact libgame.so (v1.67.0,
// sha256 542c25e5…c07b150a) so a mismatched build is left untouched.
#include "compat/games.h"
#include "compat/loader.h"
#include <cstring>

namespace {

constexpr char   kPkg[]   = "com.fingersoft.hillclimb";
constexpr char   kSoName[] = "libgame.so";
constexpr uint32_t kNop   = 0xd503201f;

bool inRange(uint64_t vaddr, uint64_t min_vaddr, size_t alloc_size) {
    return vaddr >= min_vaddr && vaddr + 4 <= min_vaddr + alloc_size;
}

// Quirk 1 — Shop/IAP crash.
// The Google-Play shop builder populates its product vector from a store
// backend that doesn't exist here, leaving it empty, then unconditionally reads
// items[size-1] — a load from -8, a hard crash the moment the Shop opens. The
// populate call and its result are otherwise discarded, so NOPing it skips the
// doomed populate: the Shop opens empty (nothing is purchasable here anyway)
// instead of taking the app down. Call site 0x22bbc8:
//   mov x0,x19 / bl 0x232240 / ldr x0,[x19,#0x360]
void patchShopPopulate(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x22bbc8;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0xaa1303e0u ||   // mov x0, x19
        at[ 0] != 0x9400199eu ||   // bl 0x232240 (shop-populate)
        at[ 1] != 0xf941b260u) {   // ldr x0, [x19, #0x360]
        return;
    }
    at[0] = kNop;
    compatLog("quirk[HCR]: NOP'd Shop product-list populate at +0x22bbc8 "
              "(empty-vector crash; shop opens empty, no billing backend)");
}

// Quirk 2 — racing crash on the vehicle skin lookup.
// SkinProvider::find("jeep") (bl 0x387350) returns null when the skin map is
// empty, and the caller reads the returned std::string's size byte with no null
// check (ldrb w8,[x0] at 0x31354c) — a null deref the instant a race starts.
// After the length check the string is never touched again (the rest uses the
// vehicle object in x19), and the len!=6 path just sets "no skin matched", so
// turning the deref into `cbz x0, 0x31358c` makes a missing skin fall through
// gracefully (worst case: no custom skin) instead of crashing. Signature:
//   add x0,sp,#0x10 / bl 0x387350 / ldrb w8,[x0] / ldr x9,[x0,#8]
void patchSkinLookupGuard(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x31354c;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0x9401cf82u ||   // bl 0x387350 (SkinProvider::find)
        at[ 0] != 0x39400008u ||   // ldrb w8, [x0]   ← the crashing load
        at[ 1] != 0xf9400409u) {   // ldr  x9, [x0, #8]
        return;
    }
    at[0] = 0xb4000200u;           // cbz x0, #0x31358c  (skip to "no skin" path)
    compatLog("quirk[HCR]: guarded null SkinProvider::find at +0x31354c "
              "(cbz x0 → +0x31358c; racing no longer crashes on a missing skin)");
}

}  // namespace

// ─── Registry entry points ──────────────────────────────────────────────────
// (The dispatcher lives here while HCR is the only title with quirks; add a
//  game_<title>.cpp and route to it here when another game needs fixups.)

void gameApplyQuirks(const char* pkg, const char* soname,
                     uint8_t* stage_base, uint64_t min_vaddr, size_t alloc_size) {
    const bool isHcr = (pkg && strcmp(pkg, kPkg) == 0) ||
                       (soname && strcmp(soname, kSoName) == 0);
    if (isHcr && soname && strcmp(soname, kSoName) == 0) {
        patchShopPopulate(stage_base, min_vaddr, alloc_size);
        patchSkinLookupGuard(stage_base, min_vaddr, alloc_size);
    }
}

int gameMarketVariation(const char* pkg) {
    if (pkg && strcmp(pkg, kPkg) == 0)
        return 1;   // claim Google Play so the shop builder takes a sane branch
    return -1;       // no opinion — caller keeps its own default
}
