#pragma once
#include <cstdint>
typedef uint32_t u32; typedef uint64_t u64;
static inline u64 armGetSystemTick() { return 0; }
static inline u64 armTicksToNs(u64 t) { return t; }
