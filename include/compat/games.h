#pragma once
#include <cstdint>
#include <cstddef>

// ─── Per-game compatibility profiles ────────────────────────────────────────
// Game-specific fixups live here, isolated from the shared translation layer
// (elf_loader/jni_env/shim_table) and from the Unity runtime, so one title's
// quirks can never collide with another's. Each supported title has its own
// source/compat/games/game_<title>.cpp; this header is the only surface the
// shared code calls into.

// Apply any in-place instruction patches a game needs to a freshly-staged,
// still-writable .so image. Called once per loaded library, before the image is
// made executable. `soname` is the library basename (e.g. "libgame.so");
// `pkg` is the owning package id (e.g. "com.fingersoft.hillclimb") or "" if
// unknown. A no-op for anything without a registered profile.
void gameApplyQuirks(const char* pkg, const char* soname,
                     uint8_t* stage_base, uint64_t min_vaddr, size_t alloc_size);

// The store variation a Cocos2d-x game should believe it is (getMarketVariation;
// 1 = Google Play). Returns the game's value, or -1 if it has no opinion (the
// caller then uses its own default).
int gameMarketVariation(const char* pkg);
