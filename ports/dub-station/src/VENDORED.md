# Vendored source

## src/airwindows/ — Galactic reverb

`galactic.h` is the reverb DSP from Airwindows' Galactic (MIT, © Chris Johnson / Airwindows), from
https://github.com/airwindows/airwindows, `plugins/LinuxVST/src/Galactic/` (`Galactic.h` + `GalacticProc.cpp`).
Stripped of the AudioEffectX/VST framework into a standalone stereo struct and simplified for a fixed 44.1 kHz
host (overallscale = 1, so cycleEnd = 1 and the original's >48 kHz lastRef interpolation is a no-op and dropped);
the algorithm is otherwise transcribed verbatim. The MIT notice is kept in the file header.

## src/chompi/ — CHOMPI TAPE 2.0 firmware DSP

From https://github.com/CHOMPI-Club/CHOMPI, commit `a73d732613da684e4de844619b690776f0f50ccf`,
folder `firmware/chompi-tape/code/src/`. MIT (`src/chompi/LICENSE`, `src/chompi/THIRD_PARTY.md`).

Files used by Dub Chamber: `fx_engine.h`, `reverb.h`, `InterpolatedDelayLine.h`, `Warble.h`, `DJFilter.h`,
`BasicMMF.h`. (`DJFilter.h`/`BasicMMF.h` drive the sweepable filter in the delay feedback.)

Local changes (each marked `TAPE MAGIC` in the file — the first port they were vendored for):
- `reverb.h`: `Init()` zeroes `lp_decay_1_`, `lp_decay_2_`, `amount_`, `input_gain_`, `reverb_time_`
  (the firmware leaves them as zeroed globals; here the object lives on the heap).
- `Warble.h`: the RNG state is a member instead of a function-static (one per instance); `Init()` zeroes the
  members the firmware leaves uninitialised; `SetDelay((size_t)10)` (the `10u` overload is ambiguous on 64-bit hosts).

Not taken: `DSPEngine.h` (tied to the sampler). Its reverb/delay/warble wiring is reproduced and re-voiced for a
dub-techno delay+reverb in `src/engine.cpp` (independent delay feedback with a filter and lo-fi degrade, huge
reverb, ducking and a global dry/wet).

## src/daisysp/ — DaisySP

From the same commit, `firmware/chompi-tape/code/libs/DaisySP/Source/Utility/`: `dsp.h`, `delayline.h`. MIT
(`src/daisysp/LICENSE`).

Local change:
- `dsp.h`: `fmax()`/`fmin()` use the `vmaxnm`/`vminnm` instructions only on processors that have them (the MPC's
  Cortex-A17 does not).

`s162f()`/`f2s16()` in `src/engine.cpp` are from libDaisy's `daisy_core.h` (MIT, © Electrosmith).
