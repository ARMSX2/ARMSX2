# Linux arm64 PGO profile

`armsx2-linux-arm64.profdata` is a merged Clang 17 instrumentation profile.
The Linux SDL workflow (`.github/workflows/linux_build_sdl.yml`) passes it as
`-DARMSX2_PGO_PROFILE=...`. CMake then compiles every target with
`-fprofile-use`, the same way the Android release uses `platforms/android/pgo/armsx2.profdata`.

The profile describes which emulator code paths are hot: the recompilers, VU,
GS hardware renderer, SPU2, and so on. It is not tied to a particular game. Code
that changed after training still builds correctly; those functions just get no
profile guidance (`-Wno-profile-instr-out-of-date` silences the warning). Retrain
every so often so the profile keeps up with the code.

## Retraining

The compiler has to match the CI compiler (clang-17), because a `.profdata`
file is only valid for the LLVM major version that wrote it. Use the same
CMake options as the workflow:

```sh
# 1. Instrumented build
cmake -B build-gen -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17 \
  -DENABLE_QT_UI=OFF -DENABLE_SDL_FRONTEND=ON \
  -DDISABLE_ADVANCE_SIMD=TRUE -DOVERRIDE_HOST_PAGE_SIZE=4096 \
  -DUSE_PGO_GENERATE=ON
ninja -C build-gen pcsx2-sdl

# 2. Play. Each process writes one .profraw file. Load a few saves in different
#    games (3D-heavy scenes, FMV, menus) and spend about a minute in each.
#    Close the emulator normally (or with SIGTERM) so the profile gets written.
LLVM_PROFILE_FILE=$PWD/prof/%p.profraw ./build-gen/bin/armsx2-sdl game.chd

# 3. Merge, then copy the result here
llvm-profdata-17 merge -o platforms/linux/pgo/armsx2-linux-arm64.profdata prof/*.profraw
```

To build with a local profile, add `-DARMSX2_PGO_PROFILE=/path/to/file.profdata`
(with LTO enabled, as in CI) to a normal configure.

The current profile was recorded on an RK3588 (Cortex-A76/A55, Mali-G610,
Vulkan) from six one-minute runs: Shadow of the Colossus (three scenes),
God of War II, Burnout Revenge and Need for Speed Underground 2.
