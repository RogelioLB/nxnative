# dm/android — the host on Android

The Switch game's own arm64 code runs natively on an Android phone, as it does on the iPhone: the same rewriter, host and NVN interface layer, built
for Android (bionic, arm64-v8a) with stand-ins for the Darwin interfaces they call. The Vulkan NVN replay draws (stage 2: shaders translated to
SPIR-V when the game is packed) and runs compute and transform feedback; tessellation is missing, as on the Mac and iOS (see Status). Super Mario Bros. Wonder plays; Bowser's Fury and Danganronpa V3 (from a provisional profile) run with
a correct picture (the [README](../../README.md#status) has the list).

## Status

| Part | State |
|---|---|
| Rewriter | `DM_TARGET=android dm/tools/patch_nso.py`: guest thread slots through x18 (below); the outputs carry `# TARGET android` and the loader refuses a module rewritten for the other target |
| Host | All of `dm/host` builds unchanged in logic: Darwin interfaces come from `compat/` (force-included `dm_android.h` and headers named as Apple's) and `plat_android.c` |
| Audio and movies | AudioToolbox's output AudioUnit on AAudio (`coreaudio_aaudio.c`). Movies' H.264 (`hle_movie_avc.h`) and AAC (AudioConverter) on MediaCodec (`media_android.c`): H.264 pictures through an AImageReader (YUV 4:2:0 with plane strides), out as NV12 or RGBA |
| Graphics | `gfx_vk.c`, the NVN replay on Vulkan, stage 1: textures (one image per serial; packaged and CPU-written textures deswizzled and uploaded, re-uploaded after writes through the dirty-page tracker; BC decoded on the CPU with `dm/third_party/bcdec` where the GPU lacks BC), buffer/texture copies, CPU texel reads, render-target and texture clears (full ones), presents to the window. Draws (`gfx_vk_draw.inc`, stage 2): the shaders translated when the game is packed (below), one descriptor set per stage, pipelines cached by their state, render passes on the bound targets, vertex / index / uniform / storage data copied from guest memory per draw, NVN's driver constant banks made from the bound state, textures and samplers resolved through the pools the game registers. Compute dispatches (direct and indirect; c0 + 0x200 the group counts, + 0x210 the uniform buffer table). Indirect and multi-draw-indirect draws (the arguments read on the CPU once earlier GPU writes have landed). Images (storage images in the format the shader declares, the textures made mutable for it; buffer images as storage texel buffers), buffer textures (uniform texel buffers over copies), multisample textures, with stand-ins of the right kind for unbound slots (a draw whose binding cannot be valid is skipped, never bound wrong). Textures shared by storage as `nvn_mtl.m` shares them (pool, offset and description: a render target made anew on another object each frame, or read through another object, is the same image); texture views in their own format (the images of uncompressed colour formats are mutable); whole texture arrays; vertex streams stepping per N instances (`VK_EXT_vertex_attribute_divisor`); vertex attributes matched to the shader's input types (res.json `vin`; integer-only inputs translated again as uint, as `dmsh_build.py` does): a format of another kind, or one the GPU takes as no vertex format (all scaled ones on the Adreno 619: `tests/t_vkfmt.c`), becomes on the CPU the 32-bit registers Maxwell's fetch would give, in a binding of its own; CPU-written linear textures (LINEAR, LINEAR_RENDER_TARGET: a game's glyph cache) uploaded from guest memory and again after CPU writes. Clears limited by the scissor (`vkCmdClearAttachments`) and with a channel or stencil mask (a draw: `shaders/clear.*`). Submissions are asynchronous: chunks of commands with their own staging, arena, descriptor pools and write-backs, completed in order by a thread that writes back to guest memory only the bytes the GPU changed, then signals events and the NVN fence. Not done: tessellation (the Mac / iOS backend has none either: `SetPatchSize` is not even recorded, and the Adreno binds only 4 descriptor sets) Transform feedback without `VK_EXT_transform_feedback` (the Adreno 619 has none): a vertex shader with a feedback layout gets a `.tf.spv` variant that stores its captured outputs into storage buffers, drawn as points with rasterization off and copied back to the game's buffers (the limits of `nvn_mtl.m` tf_draw: not indexed, instanced, fans or quads); rasterizer discard draws nothing else. `-DDM_GFX=null` builds `gfx_null.c` instead (no GPU) |
| Shaders | `tools/spv_build.py`: the package `dm/gfx/tools/shader_split.py` makes (as for iOS) translated by the repository's `dmsh_xlate` to Vulkan SPIR-V, then `tools/spv_android.py`: a descriptor set per stage, fragment shader interlock removed, a `.z.spv` variant of each vertex shader for NVN's -1..1 depth. The app reads them from `game/spv` (`DM_SPV_DIR`). Vertex shaders whose inputs are only used as integers are translated again with those inputs uint (`int_inputs` on the SPIR-V). Outputs carry the Android chain version (`android` in res.json); older ones are translated again. Two-part vertex shaders (VertexA + VertexB) are merged first, as `dmsh_build.py` does; compute shaders without facts take their thread group from the control section |
| Front end | `front_android.c`: NativeActivity, Vulkan window with a status screen, a physical controller as player 1, lifecycle, log to logcat (tag `nxnative`) and `files/log.txt`. The Java side (`java/`, in the APK's `classes.dex`): the virtual gamepad of dm/ios's `DmGamepad.m` (2D / 3D / Tap layouts, kept per app; hidden while a controller is connected; touches no control takes go to the game's touch screen; shaking the phone shakes the controller), drawn in a PopupWindow over the activity, and the software keyboard's text box (an AlertDialog). Game vibration (`haptics_android.c`): player 1's motors on the controller's vibrator, else the phone's, as a game vibration (`USAGE_GAME`); `DM_RUMBLE` / `DM_RUMBLE_GAIN` as on Apple. The driver's pipeline cache is kept in the app's files (`DM_PIPELINE_CACHE`), written when the app pauses |
| Packing | `scripts/build_android_from_nsp.py`, and the desktop app `scripts/nxnative_android.py` over it. A game without a profile is surveyed first and packed from a provisional profile (`--no-push`), then built and installed like any other |

Tested on a POCO X5 5G (Snapdragon 695, Adreno 619, Android 14, 4 KiB pages): the self test and the sandbox checks pass in the app, and the host
unit tests below pass over adb. Games run on it: Super Mario Bros. Wonder, Super Mario 3D World + Bowser's Fury, Danganronpa V3 and The Binding of Isaac: Afterbirth+ with Repentance
(its shaders compiled at run time and captured on the phone: [Run-time GLSLC outputs captured on the phone](../../docs/reference/shaders.md#run-time-glslc-outputs-captured-on-the-phone-android)).

## Games on the phone: the launcher

One app carries every game packed on the computer. Its launcher (`java/org/nxnative/android/Launcher.java`) lists the games in `Android/data/org.nxnative.android/files/games/<title ID>/`
(icon, name and version from their `icon.jpg` and `control.nacp`); a tap opens the game's settings screen, and Play starts it. It has the desktop app's look: an ink
ground, one cream action (Play), the game's own colours washed behind its screen; the app's icon is the adaptive icon in `apk/res` (the mark: two halves of a controller,
cream on ink, with a monochrome layer), which `tools/build_apk.py` links unless `--icon` names another.

- **A host per game.** The host is built for one profile, so the APK holds one per game, `lib/arm64-v8a/libdmhost_<title ID>.so`. The NativeActivity's library is
  `libnxloader.so` (`loader.c`): it reads the intent extra `game` (the title ID) and forwards `ANativeActivity_onCreate` to that game's host. A game folder whose host the APK
  lacks is listed as needing an app update.
- **A process per game.** The game runs in `:game` (`apk/AndroidManifest.xml`): a host is loaded once per process, so starting another game ends the running one, after
  asking, and starts a fresh process.
- **Per-game paths** (`front_android.c`): the game's files in `files/games/<id>/`, its settings in `files/games/<id>/launcher.txt` (and `env.txt`, for every game, in `files/`);
  in internal storage its save in `saves/<id>` (`DM_SAVE`; a device save beside it as `saves/<id>_device`) and the driver's pipeline cache in `cache/<id>/pipeline.cache`.
- **Saves of the one-game installs.** The app before the launcher kept one game in `files/game/` and its save in internal `files/save`. The packing script moves that game
  folder to `files/games/<id>/` when it packs the same game (the same `control.nacp`), or with `--migrate --title <id>`, and leaves a `legacy_save` marker there. At its next start
  the launcher backs every internal save up to `files/backups/<time>/` and copies `files/save` (and `save_device`) into `saves/<id>`, through a temporary folder; `files/save`
  itself is left as it was. **Back up saves** in the game list makes the same backup on demand (xattrs, the guest names of case collisions, go to its `xattrs.tsv`: external
  storage does not keep them); a backup pushed to `files/backups/restore/` is copied back into internal storage at the next start, after a backup of what is there.
- **Updates keep the saves.** Every APK is signed with `~/.android/nxnative-debug.jks` and installed with `adb install -r`; the scripts never uninstall.

## Settings screen

A game's settings screen (the iOS launcher's counterpart) shows its icon and name and the settings the Android host reads: console mode (`DM_OPMODE`), game language (`DM_LANG`, the game's own languages), game timing
(`DM_AUDIO_PACE`), game rumble and its strength (`DM_RUMBLE`, `DM_RUMBLE_GAIN`). Defaults come from the game's `defaults.txt` (the packing script's `--language`). The frame-rate counter
(`DM_SHOW_FPS`, drawn by `Gamepad.java` from the frames presented) and the render scale (`DM_RENDER_SCALE`, offered when `main.fixups.tsv` marks a render-scale stub: Mario 1.2.1 has
one, `dm/games/010015100B514000/patches.py`) are there too. Play writes the game's `launcher.txt` and starts the NativeActivity, which reads it after `env.txt` without overriding it; a key
`env.txt` sets is shown greyed out. The choices are kept per game. Not yet on Android: touch feedback.

Write watches (`DM_PROBE_WATCH`, `dm/host/probe.c`) work here too: `heap+<offset>` names a guest-heap address (the guest address space moves between runs on Android), the page is made
writable for each emulated store (no second mapping), and STXR / STLXR are carried out as plain stores that succeed. One run with that last change showed a black picture: use the
watch for finding a writer, not for playing.

## Thread slots through x18

On Apple the rewritten code finds its four thread slots (the guest's x18, tpidr_el0 and tpidrro_el0, and the host gate) at `tpidrro_el0 + 8 * key`,
the pthread TSD. Linux arm64 gives user code no such register, but it preserves x18, which Apple's kernel does not. So on Android each guest thread
gets a block (`plat_android.c` `dm_slots_thread_init`): a page whose last 32 bytes are the slots, then 16 KiB of shadow call stack, and x18 points at
the start of the shadow call stack. The rewriter emits `mov xN, x18` where it emitted `mrs xN, tpidrro_el0` (`dm/tools/nso_asm.py`), and the key page
holds each slot's offset from x18 (-32, -24, -16, -8). The guest never changes x18: every guest use of x18 is rewritten to slot K18. System code built
with ShadowCallStack pushes return addresses at [x18] and finds a stack there. The host is built with x18 reserved, and gives x18 back to bionic's
value before a thread returns into bionic or exits (`dm_slots_x18_restore`).

## Building

With the Android SDK (NDK, CMake 3.22, build-tools), a JDK (keytool) and Python with lz4, capstone and fonttools:

```sh
cmake -G Ninja -S dm/android -B <build> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-29 -DGAME=<base game title ID> [-DDM_TESTS=ON]
ninja -C <build> dmhost_app nxloader dmhost_cli   # libdmhost_<id>.so (the game's host in the app), libnxloader.so and dmhost (a command-line host for adb shell)
python dm/android/tests/selftest_nso.py <ndk clang> <ndk llvm-objcopy> <dir>/main.nso
DM_TARGET=android DM_GAME=<id> python dm/tools/patch_nso.py <dir>/main.nso <dir>/patched main
python dm/android/tools/build_apk.py --sdk <sdk> --loader <build>/libnxloader.so --so <build>/libdmhost_<id>.so [--so <another game's host> ...]       --selftest <dir>/patched --keystore ~/.android/nxnative-debug.jks -o nxnative.apk
```

On Windows, Git Bash rewrites `/data/...` arguments: set `MSYS_NO_PATHCONV=1` for adb only (it breaks native tools' paths).

`scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys>` does it all for a game you own: extraction, the Android rewrite, the host for the
profile (kept in the computer's host library, `%LOCALAPPDATA%\nxnative\android\hosts\<id>\` on Windows, `~/.local/share/nxnative/android` elsewhere), the APK with every host in
the library, then installs it and copies the modules, the RomFS, the DLC, the button-icon font and a `game.json` to `Android/data/org.nxnative.android/files/games/<id>/`.
`--push-only` installs and copies a game built before without building; `--apk-only` packs and installs the app alone; `--migrate --title <id>` moves a one-game install;
`--check` and `--info <NSP folder>` print the requirements and a game's details as JSON; `--progress json` prints the build's progress as JSON events.
adbd may not create folders there, so the folder tree is made first by a mkdir script run in the shell. The shell makes them `drwxrws---` for itself, which an Android 15 phone (a Galaxy S21 Ultra) kept the app out of (it found no game and ran the self test, logging `[RED] the game in … cannot be read`), so the script opens the copied tree to everyone at the end (`chmod 0777` folders, `0666` files; `Android/data/<package>` is reachable only by the app and the shell). Every build is signed with
`~/.android/nxnative-debug.jks`: an app signed with another key cannot be updated, and uninstalling it deletes its saves and game files.

## Desktop app

`scripts/nxnative_android.py` (Python with PySide6: `pip install PySide6`) runs the packing script and adb for you:

- **Library**: the NSP folders you add, with each game's box art, name, version and status on the phone picked at the top; *Build and install*, *Build only*, *Install
  on phone* (no build) and *Play*. A game without a profile gets *Experimental build* (`--no-push`: the survey, a provisional profile in `<NSP folder>/<id>_build/profile`,
  found through `DM_GAMES_DIR`); once built it is marked *Experimental* and installs like the others.
- **Phones**: the adb devices over USB and Wi-Fi (*Pair over Wi-Fi*, *Connect*, *Discover*, *Disconnect*); for the phone picked at the top, the app version and the games
  on it, *Update the app*, *Move the old one-game install*, *Back up saves to this computer* (the app's `files/backups/`) and *Remove game from phone* (its save stays).
- **Builds**: the running build's stages, progress bars and messages, from `--progress json` (or a console build's journal); *Stop* ends it and its tools.
- **Performance**: `adb logcat -s nxnative` with tiles for the figures the host prints (frame time, GPU busy, replay CPU and draws, game speed, the wait for the GPU) and a frame-time
  chart; *Profile a frame* asks for a frame's GPU times per pass (`files/prof_now`) and groups them; *Thermal* shows the phone's cooling state.
- **Settings**: the keys, and `--check` with how to get what is missing. It opens first when something is.

## Checks

- The app without a game also runs `nvn_demo.c`: the NVN calls a game makes to clear its window texture and present it, 600 frames of changing colour on screen,
  and with `--demo` given to `build_apk.py` (the output of `tests/synth_shaders.py` translated: `dmsh_xlate` on its `jobs.json`, then `tools/spv_android.py`, plus its
  `*.code` / `*.ctl`) a triangle drawn by synthetic Maxwell shaders over it. `dmhost` runs it after the self test with `DM_NVN_DEMO=<frames>` and `DM_SPV_DIR`, and logs
  pixels read back: on the Adreno 619 the centre is `ffccbbba` (red and green interpolated, blue and alpha from the uniform buffer) and the orientation is NVN's lower-left origin.
  The demo's last frame also draws through DrawArraysIndirect, clears two corners through the scissor (one red only), and dispatches a compute shader that writes
  a storage buffer (checked after, with a word the CPU wrote meanwhile beside it, which must stay); with `vstf.*` it draws the triangle once more with
  rasterization off and transform feedback on, and checks the captured outputs.
  The last frame reads the triangle's colour as RGBA16_UI2F from a stream of its own. The texture probe after the frames also clears a target through one texture object and reads it through another over the same storage.
  `DM_NVN_DEMO_MODE=llf` culls front faces (the triangle goes), `ul` sets what most games set (upper-left origin, depth 0..1) with back faces culled (the triangle
  stays, its rows flipped), `ulf` culls front faces there (it goes): all four pass on the Adreno 619.
- The app without a game: the self test (`tests/selftest_nso.py`: x18 slots, TLS, TLR, gates, an import, a second guest thread) and the sandbox checks
  (a write fault reaching the host's hook past ART, the save transaction's tree copy with xattrs and `renameat2(RENAME_EXCHANGE)`, an unmapped read).
- In the app without a game, with switches in `env.txt`: `DM_PAD_TEST=1` sends made-up touches to the virtual gamepad (not to the screen) and checks every
  key, the D-pad, both sticks, two fingers and the game's touch screen (`gamepad check: 44 checks, 0 failed`); `DM_TEXT_TEST=2` opens the text box after the demo and
  types and confirms "Zelda" itself (`DM_TEXT_TEST=1` waits for a person); `DM_RUMBLE_TEST=1` vibrates half a second (`dumpsys vibrator_manager` shows it).
- `tests/t_audio`: the AAudio output at real time (a 440 Hz tone for two seconds: the frames rendered, the underruns), Stop and Start again.
- `tests/t_media <h264> <aac>`: the movie decoders on clips ffmpeg makes (`ffmpeg -f lavfi -i color=c=0x2060c0:size=320x240:rate=30 -t 1 -c:v libx264 -profile:v high -bf 2
  -pix_fmt yuv420p -x264-params aud=1 -f h264 blue.h264`; `ffmpeg -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 1 -ac 2 -c:a aac -f adts tone.aac`): every picture
  in NV12 and RGBA with its colour, the tone's length and pitch.
- `-DDM_TESTS=ON`: `t_thread_leak`, `t_condvar`, `t_fs`, `t_fs_commit_list`, `t_svc_memory` (with and without the map limit), `t_nro`, `t_hid`,
  `t_preempt` (both modes) pass on the device. `t_nvn` passes 154 of 154 on `gfx_vk.c` (130 on `gfx_null.c`; two of its checks time the GPU right after a submit and can miss on a
  fast run); before asynchronous submissions three of them failed: they expect the GPU to be
  still busy right after a submit.

## What the GPU backend needs from this device

The front end logs the Vulkan device's capabilities at start. The Adreno 619's driver (Vulkan 1.1.128) has no BC texture compression (every packaged
Switch texture is BC: they need decoding), no descriptor indexing (NVN's texture and sampler pools are bindless), no transform feedback, no custom
border colors and no extended dynamic state; it has geometry and tessellation shaders, buffer device addresses and push descriptors.
