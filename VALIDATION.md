# DUB-200 0.1.0 validation record

**Package:** `build/dub-0.1.0.fwsc`  
**FM-1 identity:** `FM-1_8000100`  
**Size:** 609,703 bytes  
**SHA-256:** `d3c728380f60f61fa10b182da040465a26af9e6fc00e38ff391147a475ec676c`

## Build

The exact release source was compiled on 2026-10-26 with the JieLi pi32v2 toolchain and AC79 SDK under QEMU x86_64 emulation. The release build passed all built-in target checks:

- `.ram_text`: **909 instructions**, no calls
- No soft-double routines; the target uses the single-precision FPU only
- App image: **229,212 bytes**
- RAM `.data + .bss`: **92,056 / 98,304 bytes**
- Pool: **266,320 / 344,064 bytes**
- Register accesses occur in `hal/` only; `src/` and `loader/` passed the static hardware-access check

## Complete host regression suite

`tests/run_tests.sh` completed successfully after building a fresh host simulator from the normal Barlow font. It passed:

- DSP maths, encoder, USB audio, TRS, storage and strict DSP simulation tests
- OTA entry and update-loader tests
- Simulated FM-1 installer tests
- Recipe-fingerprint generation
- Every ordinary UI/generator scenario
- Legacy DUB project migration scenarios (formats 8 through 17)
- New `scenario-dub200-samples`, which verifies the GLO + PRESETS Off/A/B/Alt gesture, format-18 project persistence, and safe migration from a DUB-1 format-17 project with all sample modes off

The final source changed no DUB-1 factory piece recipe. The only musical range change is Tempo’s upper bound, from 140 to **200 BPM**.

## Package-path validation

The final package is checked by the existing simulated updater/installer paths. The tests verify package logical-image extraction and identity, loader marker, updater handshake, loader staging, committed-record layout, no writes outside the OTA area, boot cleanup, normal install, force/error paths, and post-reboot identity reporting.

## Hardware boundary

This is a development firmware package, not a claim of physical-device certification. The physical FM-1 was not attached to this sandbox, so flash timing, USB MIDI sample transfer, audible mix balance, and 200 BPM performance must still be checked on the actual device. The code keeps those risks bounded: sample data is in independent slots; import stops playback; flash writes are in the foreground; the header is committed only after a device-side CRC; and interrupted imports leave that one slot silent rather than touching firmware, projects, OTA data, or panel calibration.
