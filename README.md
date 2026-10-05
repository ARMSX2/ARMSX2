# ARMSX2 — Native ARM64 JIT Fork of PCSX2
[![All Platforms](https://img.shields.io/github/actions/workflow/status/ARMSX2/ARMSX2/build-all.yml?branch=master&label=All%20Platforms)](https://github.com/ARMSX2/ARMSX2/actions/workflows/build-all.yml)

ARMSX2 is a free and open-source PlayStation 2 (PS2) emulator based on PCSX2. Its purpose is to emulate the PS2's hardware, using a combination of MIPS CPU [Interpreters](<https://en.wikipedia.org/wiki/Interpreter_(computing)>), [Recompilers](https://en.wikipedia.org/wiki/Dynamic_recompilation) and a [Virtual Machine](https://en.wikipedia.org/wiki/Virtual_machine) which manages hardware states and PS2 system memory. This allows you to play PS2 games on your phone, PC, or gaming handheld, with many additional features and benefits.

## Thank You

The ARMSX2 team is eternally indebted to the [PCSX2 project](https://pcsx2.net) it is based on. We are so fortunate to build on their 20 years of hardcore development.

## About This Fork

<img width="1920" height="1080" alt="ARMSX2" src="https://github.com/user-attachments/assets/c4170730-9fc0-4e75-9c66-bb605397a6b2" />

▶️ Watch: [PS2 Emulation on Android Has LEVELED UP](https://www.youtube.com/watch?v=noFBhUGmSYU) (Retro Game Corps)

The upstream PCSX2 project ships an ARM64 *interpreter* build for ARM, but its high-performance **JIT recompilers** (EE, IOP, VU0, VU1, and vtlb fast memory) are x86-64 only. 

**This fork exists to close that gap.** The goal is to preserve the correctness features of 20 years of PCSX2 development, while generating the fastest native ARM performance possible.

**Current status:**
- ✅ EE, VU (COP2, mVU, VU1), IOP jits fully complete with extensive unit test framework
- ✅ Enhanced interpreter with no known accuracy divergence tested against real ps2 hardware
- ✅ hardware-accurate floating point emulation in jit
- ✅ hardware-accurate software renderer
- ✅ Custom Adreno and Mali drivers with corresponding renderer backend

### Why LLMs / AI Were Used

Our development team has seasoned developers with previous compiler experience, credits on shipped PS2 games, and contributions to many emulators for other platforms.
However, we still use AI for development. Our goal is to produce the best emulator we can *by any means*. We are sensitive to the political issues surrounding AI, and we do not want our usage to be considered as a blanket endorsement of the technology.
Still, we're just too passionate about making a good emulator to not use every tool at our disposal, even if it makes us look worse in the eyes of some. This isn't about us, it's about the emulator.

## System Requirements

ARMSX2 targets ARM64 across desktop (macOS, Windows, Linux) and mobile (Android, iOS/iPadOS), all from the single shared core. See [System Requirements](https://armsx2.net/docs/system-requirements) for details, and the [ARMSX2 docs](https://armsx2.net/docs/) for setup guides.

Please note that a BIOS dump from a legitimately-owned PS2 console is required to use the emulator. See [Dumping your BIOS](https://armsx2.net/docs/dumping-bios) and [Importing your BIOS](https://armsx2.net/docs/importing-bios).

## Building

Check out our [github actions](https://github.com/ARMSX2/ARMSX2/actions/workflows/build-all.yml) for the latest build recipe
