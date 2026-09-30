# Vendored DSP sources

The DSP for this port is vendored from the Mac VST3 plugin **"Mutable Vibe"**
(private repo `nachtaktiv303/rings-plugin`). Pulled in once as committed files so the
port builds fully self-contained (Docker/CI see no host paths). Do not fetch at build time.

## Upstream
- Main repo: `https://github.com/nachtaktiv303/rings-plugin.git` @ `f1db322a0bec41e78b768a31d9089d206a226059`
- eurorack submodule (Mutable Instruments, MIT) @ `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4`
  (includes the local per-voice velocity patch `68cc5f4` and the rings polyphony-8 patch).
- Vendored on 2026-09-26 from the local checkout at `~/mutable-vibe/`.

## What is here (`src/dsp/`)
- Wrapper cores (from repo root): `rings_c.cpp`, `plaits_c.cpp`, `filter_c.c`, `reverb_c.c`,
  `delay_tape_c.c`, `sat_c.c`, `chorus_c.c`. **`elements_c.cpp` is intentionally NOT vendored**
  (Elements is excluded from this port).
- `eurorack/rings/` and `eurorack/plaits/` — full subtrees (small).
- `eurorack/stmlib/` — copied **without `third_party/`** (43 MB of STM32 HAL not used with `-DTEST=1`;
  nothing in the rings/plaits/stmlib compile path includes it — verified 2026-09-26).

## Local changes vs upstream
- `eurorack/plaits/user_data.h`: added `#include <cstdio>` inside the `#ifdef TEST` block. The mock
  `FLASH_ErasePage`/`FLASH_ProgramWord` there call `printf` but never include it; host clang/gcc pull
  `<cstdio>` in transitively so the offline test builds, but `arm32v7/gcc:12` does not → device build failed
  with "'printf' was not declared". Upstream bug; fix applied to the vendored copy (2026-09-26).
- Otherwise no source content changes. File layout changed: files that lived at the Mac repo root
  and under the `eurorack/` submodule are now under `src/dsp/` and `src/dsp/eurorack/`.
- `vst.json` `sources`/`cflags` rewritten from absolute `/Users/macmini/mutable-vibe/...` paths to
  `ports/mutable-vibe-mpc/src/dsp/...` relative paths. Old absolute version kept at `../vst.json.inplace-bak`.

## Not compiled (present in the tree but unused)
- `eurorack/{rings,plaits}/bootloader/`, `plaits.cc`, `user_data_receiver.*`, makefiles — these are the
  only files referencing the un-vendored `stm_audio_bootloader/`. They are not in `vst.json` `sources`
  and nothing we compile includes them, so their dangling includes never matter.

## Re-vendoring from a newer upstream
Re-copy the same file set from `~/mutable-vibe/` (root wrappers + `eurorack/{rings,plaits}` +
`eurorack/stmlib` minus `third_party`), bump the commits above. `tools/test_port.sh
ports/mutable-vibe-mpc/vst.json` must print PASSED afterwards.
