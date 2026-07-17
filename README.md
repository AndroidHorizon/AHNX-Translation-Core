# VNX Translation Core

The game-loading engine half of **[Viridite](https://github.com/Viridite/Viridite)** — an Android NDK compatibility layer that runs a game's real Android `.so` binary natively on Nintendo Switch, instead of emulating anything. If you're looking for the app you actually launch on your Switch, that's the [Viridite launcher repo](https://github.com/Viridite/Viridite); this repo is the engine it hands off to.

## What lives here

- **`VNX-Translation-Core-x64`** — the real engine: custom ARM64 ELF loader (JIT dual-mapping, relocation, symbol resolution), a JNI/JavaVM emulation layer, real pthreads, real audio (SDL2_mixer), real accelerometer/gyroscope/battery via libnx. Built from `source/` + `source/compat/` at the repo root.
- **`VNX-Translation-Core-x32`** (`core32/`) — a placeholder. 32-bit (`armeabi-v7a`) Android binaries aren't supported yet — running AArch32 code on Switch is possible in principle (real prior art exists for it), but the one precedent project we found for this depends on a 32-bit libnx build that isn't publicly available anywhere. This binary just explains that; it doesn't do anything else yet.

## Why this is a separate binary from the launcher

The launcher and this Core can't be one process: launching a specific game means chain-loading from the launcher into whichever Core matches that game's architecture (`envSetNextLoad`, the same mechanism homebrew forwarders use). A Switch process runs in one execution state for its whole lifetime, so a future real 32-bit engine has to be its own binary anyway — splitting the 64-bit engine out now means adding that later is "point the launcher at a new NRO," not "rewrite everything."

This binary always expects a package name (passed as `argv[1]` — `argv[0]` has to stay this binary's own real path, since libnx's `romfsInit()` depends on it to find and read this NRO's embedded assets). Launching it directly without a valid package argument just shows a message pointing back at the launcher — it isn't meant to be run standalone.

## Building

Requires [devkitPro](https://devkitpro.org/) with `devkitA64` and `libnx` installed.

```sh
export DEVKITPRO=/opt/devkitpro
make            # builds VNX-Translation-Core-x64.nro at the repo root
make -C core32  # builds the x32 placeholder
```

### Dependencies (via pacman/devkitPro)

```
switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-sdl2_mixer
switch-libpng switch-libjpeg-turbo switch-minizip switch-libvorbisidec
switch-libopus switch-opusfile switch-flac switch-mpg123 switch-libmodplug
switch-mesa switch-glad switch-curl switch-mbedtls
```

## Status

Only one game has ever actually been run against this engine — **Hill Climb Racing 1.67.0** (Fingersoft) — fully playable (touch, real audio, real threads, persistent saves, ~locked 60fps), with one known deterministic crash on the Shop/IAP screen (root-caused via `.eh_frame` unwind-table analysis: a generic `std::vector`-append helper handed an invalid reference by the game's own code when its IAP product list is empty — not yet patched, see the launcher repo's compatibility list for the full writeup).

See the [launcher repo's README](https://github.com/Viridite/Viridite) for the full project history, changelog, roadmap, and game compatibility list.

## License

Licensed under the Viridite Free & Source-Available License v1.0 — see [LICENSE](LICENSE). Free to use, copy, modify, and share; selling this code or derivatives isn't permitted, forks must credit Aaronateataco and stay under this same license and publicly available.

## About

Built by [Aaron](https://aaronworld.uk) with [Claude](https://anthropic.com) — an experiment in AI-assisted Nintendo Switch homebrew development. Aaron tests on real hardware and reports what's broken; Claude writes the fixes.
