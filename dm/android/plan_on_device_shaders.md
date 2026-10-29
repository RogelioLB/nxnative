# Plan: translating shaders on the phone

Status: a plan, nothing of it is built yet. Today every shader the Vulkan replay draws with is translated on the computer, before the game is packed: the shader
split finds the game's NVN shaders in its files, `dmsh_xlate` translates each to SPIR-V, `tools/spv_android.py` adjusts them for Android drivers, and the packing
script copies the results to `games/<title ID>/spv`. A shader that was not in the files has no translation, and its draws are skipped
([no translated shader](../../docs/reference/shaders.md#run-time-glslc-outputs-captured-on-the-phone-android)).

That leaves two gaps. Games that compile their shaders at run time (The Binding of Isaac, Fire Emblem: Three Houses, Catherine) need a capture loop: play, pull
the captures, translate them on the computer, restart the game; the first time a scene appears it draws nothing. And a game whose shader containers the split does
not read yet has no shaders at all until a reader is written. Translating on the phone, at the moment the game uses a shader, closes both.

## What the translation is made of

| Part | Size | Language |
|---|---|---|
| `dm/gfx/tools/dmsh_xlate/Program.cs`: reads a control section and code area, drives the translator, writes SPIR-V and a reflection (`.res.json`) | 253 lines | C# |
| `dm/third_party/ryujinx` (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`): the Maxwell shader decoder and SPIR-V emitter | about 42,000 lines | C# |
| `dm/android/tools/spv_android.py`: one descriptor set per stage, interlock removed, the `.z.spv` depth variant, the Adreno vector rewrite, the transform-feedback variant | 333 lines | Python |
| `dm/android/tools/spv_build.py`: shader facts, the second pass for integer vertex inputs, the transform-feedback layout | 168 lines | Python |

Porting 42,000 lines of C# to C is not realistic. The translator is managed code with no platform calls, which is what .NET NativeAOT compiles to a native
library: the plan is to build `dmsh_xlate` as `libdmsh.so` for `linux-bionic-arm64` and call it from the host.

## Phases

1. **Spike: the translator as a native Android library (1 to 2 days).** Add a library target to `dmsh_xlate` (`<PublishAot>`, `<NativeLib>Shared</NativeLib>`,
   runtime `linux-bionic-arm64`) with one export, `[UnmanagedCallersOnly(EntryPoint = "dmsh_translate")]`:
   `int dmsh_translate(int stage, const uint8_t *ctl, size_t ctl_n, const uint8_t *code, size_t code_n, const char *facts_json, uint8_t **spv, size_t *spv_n, char **res_json)`.
   NativeAOT compiles only on the operating system it targets, so this builds on Linux (WSL on Windows, or a CI job) with the NDK's clang as the linker. Check: the
   library loads in the app, translates Super Mario Bros. Wonder's packaged shaders on the phone, and its SPIR-V is byte-identical to the computer's for the same
   chain version (`DMSH_CHAIN`). Measure the library's size and the time per shader on the Adreno 619 phone. Trimming warnings from Ryujinx's reflection use are
   fixed or rooted here.
2. **The Android post-processing in C# (3 to 5 days).** Move `spv_android.py` and the parts of `spv_build.py` that work per shader (the integer-input second pass,
   the transform-feedback layout, shader facts) into the same library, so one call returns what `games/<id>/spv` holds today: `<key>.spv`, `<key>.z.spv`,
   `<key>.res.json`. The Python tools stay for the computer and call the same code (the library also builds as the desktop `dmsh_xlate`), so there is one
   implementation. Test: every shader of the four games run on Android so far comes out identical from both.
3. **The host translates what is missing (3 to 5 days).** In `gfx_vk_draw.inc`, a shader key with no file in `DM_SPV_DIR` goes to a translation thread instead of
   being skipped: the shader's control section and code come from guest memory (the program the game bound: the same bytes the split takes from files) or from
   the GLSLC output `hle_glslc.c` already sees. The draw is skipped for those few frames, as now; when the translation lands the pipeline is made and later draws
   use it. Results are kept in the app's internal storage, `cache/<title ID>/spv-<DMSH_CHAIN>/`, so a shader is translated once per phone, and a chain change starts a
   new folder. `files/spv_now` style switches stay for diagnosis, and `DM_SPV_ONDEVICE=0` turns it off.
4. **Packing and the desktop app (1 to 2 days).** `build_apk.py` adds `libdmsh.so` once (shared by every game). The offline translation stays the first source: it
   costs nothing on the phone and lets the pipeline prebuild run at start. The capture loop becomes optional; the packing script can still pull the phone's cache to
   prebuild those shaders next time.
5. **Beyond the split (later).** With translation from guest memory working, a game whose containers the split cannot read still draws: every shader is
   translated the first time it is bound. That makes the shader split an optimisation (prebuilt pipelines) rather than a requirement for a new game on Android.

## Risks

- **NativeAOT on Android.** `linux-bionic-arm64` is a NativeAOT target but less travelled than `linux-arm64`; the spike settles whether it loads in an app process
  (it must not rely on `/proc` paths or signals the host's crash handler owns: `crash.c` installs SIGSEGV / SIGBUS handlers, and the .NET runtime installs its own
  for null references; their order has to be checked).
- **Size.** A NativeAOT library with the translator is expected in the tens of MB; the APK already carries one host per game.
- **Hitches.** A translation on the phone takes milliseconds to tens of milliseconds; on its own thread the game does not stall, but the first appearance of a shader
  is a few frames without that draw. The persistent cache limits that to once per phone.
- **Inputs only the computer has.** Some translations take per-game inputs from the profile (`shader_facts.tsv`, `shader_pairs.tsv`, the vertex padding pairs).
  The packing script copies those tables beside the shaders; shaders they do not cover are translated with the defaults, as `spv_build.py` does for shaders
  "without facts".
- **Determinism.** The phone's translation must match the computer's for the same chain version, or the two sources would disagree; phase 1 and 2 test that.

## Order and size

Phase 1 decides the rest: if the library does not load or its output differs, the plan stops there with nothing changed in the app. Phases 1 to 4 are about two to
three weeks of work; phase 5 follows when a game needs it.
