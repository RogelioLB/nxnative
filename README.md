# nxnative-android

**Nintendo Switch games running natively on Android phones.** This repository is a fork of [seedds/nxnative](https://github.com/seedds/nxnative), which runs
Switch games natively on the iPhone, the iPad and the Mac; this fork adds **Android**: the same host built for Android, the games' graphics replayed on Vulkan,
an Android app with a game launcher, and a desktop app that packs your games and puts them on your phone.

It is not an emulator. The game's own ARM64 code runs directly on the phone's CPU, rewritten only where the host needs it; a host answers the Switch's system
calls and SDK services, and the game's NVN graphics commands are replayed on the phone's GPU through Vulkan. Nothing from any game is in this repository: you
bring **your own** game files and the keys of **your own** Switch. The project is not affiliated with Nintendo or any game publisher.

Everything below the line further down is the original project's README (the iPhone and Mac side), kept as it is apart from a pointer to Android.

## How it fits together

```
 Your computer (Windows, macOS or Linux)                         Your Android phone
 ───────────────────────────────────────                         ─────────────────────────────────────
 NSP / XCI files + prod.keys                                     nxnative app (org.nxnative.android)
          │                                                        ├─ Launcher: the games on the phone,
          ▼                                                        │    each game's settings, Play
 Desktop app: scripts/nxnative_android.py                          │
 (or the command line: scripts/build_android_from_nsp.py)          └─ Game screen, in its own process:
          │  1. extract the game (nx_extract)                           loads that game's host
          │  2. rewrite its code for Android (patch_nso.py)             (libdmhost_<title ID>.so), which runs
          │  3. split its shaders, translate them to SPIR-V             the game's code and replays its
          │  4. build the game's host with the Android NDK              graphics on Vulkan
          │  5. pack one APK carrying every game built so far
          └─ adb: install the APK, copy the game files ──────▶   Android/data/org.nxnative.android/files/games/<title ID>/
```

One app holds every game: each game you build adds its host to a library on your computer, and every APK is packed from the whole library, so building a second
game keeps the first one in the app. Game files (code, shaders, RomFS, icon) live in the app's folder on the phone; saves stay in the app's internal storage.

## Status

Tested on a **POCO X5 5G** (Snapdragon 695, Adreno 619 GPU, Android 14). Other phones and GPUs have not been tried.

| Game | State on Android |
|---|---|
| Super Mario Bros. Wonder 1.2.1 | Playable: about 30 fps in levels and on the world map at render scale 0.8 |
| Super Mario 3D World + Bowser's Fury 1.2.2 | Bowser's Fury runs with correct colours; one launch stopped at a black loading screen (open, see the game's note) |
| Danganronpa V3: Killing Harmony Anniversary Edition 1.0.2 | Boots, plays its opening movie and draws its picture, from a provisional (guessed) profile; not played through |
| The Binding of Isaac: Afterbirth+ 1.7.9b with Repentance | Runs from a provisional profile, with its Repentance DLC; it compiles its shaders at run time, captured on the phone (58 so far, below) |

Every other game in the [games table](dm/games/README.md#games) has a profile and can be packed the same way, but has not been run on Android. A game without a
profile can still be packed as an *experimental build* (below): most likely it does not boot, but it is the starting point for porting it.

## What you need

- **Your game and keys**, dumped from your own Switch: the game as NSP / XCI files (the base game and, if you have it, its update; for example with nxdumptool) in one
  folder, and your `prod.keys` (Lockpick_RCM).
- **On the computer:** Python 3.12 or newer with `lz4`, `capstone`, `fonttools` and `backports.zstd` (not needed on Python 3.14), plus `PySide6` for the desktop app:
  `pip install lz4 capstone fonttools backports.zstd PySide6`. The .NET SDK 10, the Android SDK with an NDK, CMake 3.22 and build-tools (set `ANDROID_HOME` or pass
  `--sdk`), `adb`, and a JDK (for `keytool` and `javac`). The desktop app's **Settings** page checks all of this and says how to get what is missing.
- **On the phone:** USB debugging (or wireless debugging) turned on, so that `adb devices` lists the phone, and room for the game: its RomFS is copied as one image
  (Danganronpa V3's is 6.7 GB).

## The desktop app

`python scripts/nxnative_android.py` opens a window that drives the packing script and adb for you:

- **Library:** the games you added (**+ Build game** picks a folder with a game's NSP files), each with its box art, version and status on the phone picked at the top.
  The big button does what comes next: **Build and install**, **Install on phone** (a game built before, without building again) or **Play**. **Manage** has the rest:
  build only, update the app on the phone, remove the game. A game with no profile gets **Experimental build**: the script surveys the game, writes a guessed
  provisional profile into the game's folder and packs it; once built it is marked *Experimental* and can be installed like any other.
- **Builds:** the running build's stages, progress and messages (the bar at the bottom of every page follows it); a build started from a console shows here too.
- **Phones:** adb devices over USB and Wi-Fi (pair, connect, discover), the app version and the games on each phone, saves backed up to the computer.
- **Performance:** the game's log from `adb logcat`, with the frame time, GPU and CPU figures the host prints, and a frame's GPU time per pass.
- **Settings:** your `prod.keys`, and the requirements above with how to install what is missing.

A rebuild only does what changed: the extraction, the rewritten code and the translated shaders are reused when their inputs are the same, and the RomFS image is not
copied to the phone again when the phone already holds the same one.

## The Android app

- **Launcher.** Opening **nxnative** shows the games on the phone with their icon, name and version (a game whose host the installed app lacks is marked as needing an
  app update). Picking a game opens its **settings screen**, grouped into Game and Controls: console mode (handheld is lighter), game language, game timing, render
  scale where the game supports it, the frame-rate counter, rumble and its strength. **Play** starts the game. Lines like `DM_RENDER_SCALE=0.8` in
  `Android/data/org.nxnative.android/files/env.txt` win over the screen (they show greyed out).
- **In the game.** Touch controls are drawn over the picture in three layouts (2D, 3D and Tap, switched with the round button at the top); touches no control takes go
  to the game's touch screen. A physical controller works as player 1 and hides the touch controls; rumble goes to the controller or the phone, and shaking the phone
  shakes the controller. The **≡** button at the top (left of −) goes back to the launcher while the game keeps running behind it: **Play** on the same game resumes
  it, and picking another game asks before closing the running one.
- **Saves** stay in the app's internal storage (`saves/<title ID>`), so updating the app keeps them; the desktop app backs them up to the computer. The log is
  `Android/data/org.nxnative.android/files/log.txt` (also `adb logcat -s nxnative`).

## Packing from the command line

```
python scripts/build_android_from_nsp.py "<folder with your NSP / XCI files>" -k "<your prod.keys>"    build, install, copy the game
    -p v0                pack the base game instead of the newest version in the folder
    -l es-419            the default game language (a code or a name)
    --serial <device>    the adb device, when more than one is connected
    --no-push            build the APK only (a game without a profile is first built this way)
    --push-only          install the app and copy a game built before, without building
python scripts/build_android_from_nsp.py --apk-only    pack the app with every game built on this computer and install it
python scripts/build_android_from_nsp.py --check       what the build needs on this computer, as JSON
```

The first build of a game takes a while (the shader translation and, on a hard disk, the RomFS image); later builds reuse what did not change.

## Seeing what a game draws

When a game shows a black or wrong picture, these files in `Android/data/org.nxnative.android/files/` ask the running game for diagnostics (each is checked every
30 frames and removed when taken; `adb shell touch <path>` creates one):

- `shot_now`: the next presented frame as a PNG in `dump/` (`DM_NVN_SHOT=<frame>[,<frame>...]:<path>` in `env.txt` does the same for given frames).
- `dump_now`: every render target of one frame, after each pass, as `.raw` and `.png` in `dump/`, with each draw's shaders and bound textures in the log.
- `threads_now`: every game thread's state, what it waits on and its backtrace, in the log (for a game that stops moving on).

## Games that compile their shaders while they run

Some games carry their shaders as GLSL source and compile them with Nintendo's GLSL compiler while they run (The Binding of Isaac does). Their files hold no shader to
translate, so the first build has none (`[RED] the game is packed without shaders`): the game runs but draws nothing. The shaders are captured on the phone:

1. Add `DM_GLSLC_CAPTURE=/sdcard/Android/data/org.nxnative.android/files/glslc_capture/<title ID>` to `Android/data/org.nxnative.android/files/env.txt`.
2. Play the game for a while: each shader is saved the first time the game compiles it (`glslc: saved a run-time compile output` in the log).
3. Copy them to the computer: `adb pull /sdcard/Android/data/org.nxnative.android/files/glslc_capture/<title ID>/. "<NSP folder>/<title ID>_build/extract/<version>/glslc_capture"`.
4. Build and install again: the shader split reads the captures and translates the new ones.

A scene reached later (or a DLC) can compile more shaders: repeat until the log shows no `no translated shader`. DLC in the NSP folder is extracted and copied to the
phone with the game.

## What the Android port changed

The iOS / Mac host runs on Android with stand-ins for the Darwin interfaces it calls (`dm/android/compat`); the guest's thread slots go through the x18 register (the
rewriter emits `mov xN, x18` for `DM_TARGET=android`). NVN is replayed on Vulkan (`dm/android/gfx_vk.c`, `gfx_vk_draw.inc`): textures (BC decoded on the CPU where the
GPU lacks it), copies, clears, draws, compute, indirect draws, transform feedback, queries and timestamps, submitted in chunks so the GPU works while the CPU records;
shaders are translated with the project's translator and adjusted for Android drivers (`dm/android/tools/spv_android.py`). Audio goes through AAudio, movies through
MediaCodec. Details of the port: [dm/android/README.md](dm/android/README.md).

Problems found on the way, each with its cause and fix in the reference ([rendering](docs/reference/rendering.md#android-vulkan-replay), [memory](docs/reference/memory.md),
[NVN objects](docs/reference/nvn-objects.md)):

- **Wrong pictures:** the Adreno driver misreading dynamic vector components in uniform buffers (vertical stripes); render targets attached in their sRGB view format
  (washed-out colours); `char` unsigned on Android (noise in clouds); separate textures read as "no texture" because the texture and sampler banks are packed into one
  field (Danganronpa V3's black screen); a second NVN device resetting the window origin (an upside-down picture); a 3D colour-grading LUT drawn a slice at a time into
  one slice (Bowser's Fury's blue scene).
- **Crashes:** the thread stack reported with bionic's guard page, which Unity's garbage collector scanned (Danganronpa V3 at boot); `pthread_exit` run on a game
  thread's small stack (Bowser's Fury after a few seconds).
- **Speed:** syncs limited to what a draw reads, passes kept open, early and mid-pass submissions, constant buffers copied only as far as the shader reads, pipeline keys
  built from the state fields, a capped pipeline prebuild; *Game time* holds the audio to the game's frames for levels timed to the music.

---

# nxnative

Play your own Nintendo Switch games natively on iPhone and iPad. Bring your game (its NSP files) and your `prod.keys`; one script turns them into an IPA that you sideload.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

> [!TIP]
> **Prefer an app to the command line?** [NX2Apple](https://github.com/LNPANDY/NX2Apple) is a community Mac app that builds your games for the iPhone, iPad or Mac, keeps the Mac games in a library and starts them with their settings. It is a separate project that follows this one; this page describes nxnative's own command-line way. Building a front end of your own: [Front ends](docs/frontends.md).

## Acknowledgements

This project is built on other people's work:

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' Maxwell shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **yuzu**: Maxwell instruction encodings for the shader reference tool, and a cross-check for kernel behaviour.
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`. **hactool** did the first extractions.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language. **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **deko3d** (zlib): its texture layout size algorithm is ported in `dm/gfx/nvn/nvn_tex.c`.
- **libvpx** (VP9 movies), **stb_vorbis** (movie sound), the **Vulkan headers** and **NVIDIA FLIP** ship in `dm/third_party` with their licenses ([list](dm/third_party/README.md)).
- **Capstone**, **lz4**, **fontTools** and **XcodeGen** are used by the build.

## Games

Games tested on Mac or iPhone. See the [per-game notes](dm/games/README.md#games) for tested platforms, progress and known issues:

Needs iOS / iPadOS 26 or later on an iPhone 15 Pro or newer (A17 Pro chip or later) or an iPad with an M1 chip or later: the games' textures are BC-compressed, and older iPhones can't read that format. Other games aren't supported yet. For them the script writes a survey and still tries its best to pack an IPA, guessing the game's settings; such an IPA may not start.

Under each game: the IPA's size, and "60 fps with 1.2.0" where the game runs at 60 fps (30 on the Switch) with that update.

<!-- gallery: written by dm/tools/docs_gallery.py from docs/images/gallery.tsv; change the table, then run it -->
<table>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-kart-8-deluxe.jpg" width="256" alt="Mario Kart 8 Deluxe"><br>Mario Kart 8 Deluxe<br><sub>~11.9 GB · no Mii heads</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-vs-donkey-kong.jpg" width="256" alt="Mario vs. Donkey Kong"><br>Mario vs. Donkey Kong<br><sub>~1.6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/paper-mario-the-thousand-year-door.jpg" width="256" alt="Paper Mario: The Thousand-Year Door"><br>Paper Mario: The Thousand-Year Door<br><sub>~5 GB · 60 fps with 1.0.1</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-3d-world.jpg" width="256" alt="Super Mario 3D World + Bowser's Fury"><br>Super Mario 3D World + Bowser's Fury<br><sub>~3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-bros-wonder.jpg" width="256" alt="Super Mario Bros. Wonder"><br>Super Mario Bros. Wonder<br><sub>~4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-galaxy.jpg" width="256" alt="Super Mario Galaxy"><br>Super Mario Galaxy<br><sub>~3 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-galaxy-2.jpg" width="256" alt="Super Mario Galaxy 2"><br>Super Mario Galaxy 2<br><sub>~3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-odyssey.jpg" width="256" alt="Super Mario Odyssey"><br>Super Mario Odyssey<br><sub>~6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-breath-of-the-wild.jpg" width="256" alt="The Legend of Zelda: Breath of the Wild"><br>The Legend of Zelda: Breath of the Wild<br><sub>~16 GB · 60 fps with 1.9.0</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-links-awakening.jpg" width="256" alt="The Legend of Zelda: Link's Awakening"><br>The Legend of Zelda: Link's Awakening<br><sub>~6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-tears-of-the-kingdom.jpg" width="256" alt="The Legend of Zelda: Tears of the Kingdom"><br>The Legend of Zelda: Tears of the Kingdom<br><sub>~17 GB · 60 fps with 1.4.3</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/13-sentinels.jpg" width="256" alt="13 Sentinels: Aegis Rim"><br>13 Sentinels: Aegis Rim<br><sub>~5 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/absolum.jpg" width="256" alt="Absolum"><br>Absolum<br><sub>~2 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ace-attorney-investigations.jpg" width="256" alt="Ace Attorney Investigations Collection"><br>Ace Attorney Investigations Collection<br><sub>~7 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/advance-wars-1-2-re-boot-camp.jpg" width="256" alt="Advance Wars 1+2: Re-Boot Camp"><br>Advance Wars 1+2: Re-Boot Camp<br><sub>~7.1 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/ai-the-somnium-files.jpg" width="256" alt="AI: THE SOMNIUM FILES"><br>AI: THE SOMNIUM FILES<br><sub>~8.3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ai-the-somnium-files-nirvana-initiative.jpg" width="256" alt="AI: THE SOMNIUM FILES - nirvanA Initiative"><br>AI: THE SOMNIUM FILES - nirvanA Initiative<br><sub>~11.6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/animal-crossing-new-horizons.jpg" width="256" alt="Animal Crossing: New Horizons"><br>Animal Crossing: New Horizons<br><sub>~10.5 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/animal-well.png" width="256" alt="ANIMAL WELL"><br>ANIMAL WELL<br><sub>~54 MB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/astral-chain.png" width="256" alt="ASTRAL CHAIN"><br><a href="dm/games/01007300020FA000/README.md">ASTRAL CHAIN</a><br><sub>~9.7 GB · 60 fps target with 1.0.1</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/balatro.jpg" width="256" alt="Balatro"><br>Balatro<br><sub>~82 MB · played by touch</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/ball-x-pit.jpg" width="256" alt="BALL x PIT"><br>BALL x PIT<br><sub>~1.1 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta.jpg" width="256" alt="Bayonetta"><br>Bayonetta<br><sub>~8.8 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta-2.jpg" width="256" alt="Bayonetta 2"><br>Bayonetta 2<br><sub>~12.5 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta-3.jpg" width="256" alt="Bayonetta 3"><br>Bayonetta 3<br><sub>~14.2 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/big-brain-academy-brain-vs-brain.jpg" width="256" alt="Big Brain Academy: Brain vs. Brain"><br>Big Brain Academy: Brain vs. Brain<br><sub>~0.8 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/brain-memory-training-academy.jpg" width="256" alt="Brain Memory Training Academy"><br>Brain Memory Training Academy<br><sub>~0.1 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/captain-toad-treasure-tracker.jpg" width="256" alt="Captain Toad: Treasure Tracker"><br>Captain Toad: Treasure Tracker<br><sub>~2 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/catherine-full-body.jpg" width="256" alt="Catherine: Full Body"><br>Catherine: Full Body<br><sub>~14 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/celeste.jpg" width="256" alt="Celeste"><br>Celeste<br><sub>~0.9 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/cuphead.jpg" width="256" alt="Cuphead"><br>Cuphead<br><sub>~5 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dark-souls-remastered-creation.jpg" width="256" alt="DARK SOULS: REMASTERED"><br>DARK SOULS: REMASTERED<br><sub>~4 GB · 60 fps with 1.0.3</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/digimon-story-cyber-sleuth.jpg" width="256" alt="Digimon Story Cyber Sleuth: Complete Edition"><br>Digimon Story Cyber Sleuth: Complete Edition<br><sub>~5 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/donkey-kong-country-tropical-freeze.jpg" width="256" alt="Donkey Kong Country: Tropical Freeze"><br>Donkey Kong Country: Tropical Freeze<br><sub>~7.3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dragon-quest-vii-reimagined.jpg" width="256" alt="DRAGON QUEST VII Reimagined"><br>DRAGON QUEST VII Reimagined<br><sub>~5.9 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dragon-quest-xi-s.jpg" width="256" alt="DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition"><br>DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition<br><sub>~15 GB · 60 fps with 1.0.4</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-resonance.jpg" width="256" alt="FINAL FANTASY RESONANCE"><br>FINAL FANTASY RESONANCE<br><sub>~10 GB · 60 fps with 1.0.1</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-tactics.jpg" width="256" alt="FINAL FANTASY TACTICS - The Ivalice Chronicles"><br>FINAL FANTASY TACTICS - The Ivalice Chronicles<br><sub>~11 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/fire-emblem-engage.jpg" width="256" alt="Fire Emblem Engage"><br>Fire Emblem Engage<br><sub>~15 GB · 60 fps with 2.0.0</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/fire-emblem-three-houses.jpg" width="256" alt="Fire Emblem: Three Houses"><br>Fire Emblem: Three Houses<br><sub>~13 GB · 60 fps with 1.2.0</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ghost-trick-phantom-detective.jpg" width="256" alt="Ghost Trick: Phantom Detective"><br>Ghost Trick: Phantom Detective<br><sub>~3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/the-great-ace-attorney-chronicles.jpg" width="256" alt="The Great Ace Attorney Chronicles"><br>The Great Ace Attorney Chronicles<br><sub>~4 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/hades.jpg" width="256" alt="Hades"><br>Hades<br><sub>~4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/hades-ii.jpg" width="256" alt="Hades II"><br>Hades II<br><sub>~4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/hollow-knight-silksong.jpg" width="256" alt="Hollow Knight: Silksong"><br>Hollow Knight: Silksong<br><sub>~6.4 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/hyrule-warriors-age-of-calamity.jpg" width="256" alt="Hyrule Warriors: Age of Calamity"><br>Hyrule Warriors: Age of Calamity<br><sub>~15 GB · 60 fps with 1.3.0</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/kirby-and-the-forgotten-land.jpg" width="256" alt="Kirby and the Forgotten Land"><br>Kirby and the Forgotten Land<br><sub>~6.4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/luigis-mansion-3.jpg" width="256" alt="Luigi's Mansion 3"><br>Luigi's Mansion 3<br><sub>~7.5 GB · 60 fps with 1.4.0</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/metroid-dread.jpg" width="256" alt="Metroid Dread"><br>Metroid Dread<br><sub>~5 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/monster-hunter-rise.jpg" width="256" alt="Monster Hunter Rise"><br>Monster Hunter Rise<br><sub>~22 GB · 60 fps with 16.0.2</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/monster-train-2.jpg" width="256" alt="Monster Train 2"><br>Monster Train 2<br><sub>~1.6 GB · 60 fps with 1.1.1</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler.jpg" width="256" alt="OCTOPATH TRAVELER"><br>OCTOPATH TRAVELER<br><sub>~3.6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler-0.jpg" width="256" alt="Octopath Traveler 0"><br>Octopath Traveler 0<br><sub>~4 GB · 60 fps with 1.0.5</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler-ii.jpg" width="256" alt="Octopath Traveler II"><br>Octopath Traveler II<br><sub>~5.9 GB · 60 fps with 1.1.1</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/patricks-parabox.jpg" width="256" alt="Patrick's Parabox"><br>Patrick's Parabox<br><sub>~0.2 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/persona-5-royal.jpg" width="256" alt="Persona 5 Royal"><br>Persona 5 Royal<br><sub>~14.9 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pikmin-4.jpg" width="256" alt="Pikmin 4"><br>Pikmin 4<br><sub>~10.8 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-brilliant-diamond.jpg" width="256" alt="Pokémon Brilliant Diamond"><br>Pokémon Brilliant Diamond<br><sub>~4.6 GB · 60 fps with 1.3.0</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-legends-z-a.jpg" width="256" alt="Pokémon Legends: Z-A"><br>Pokémon Legends: Z-A<br><sub>~6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-scarlet.png" width="256" alt="Pokémon Scarlet"><br>Pokémon Scarlet<br><sub>~7.0 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-sword.jpg" width="256" alt="Pokémon Sword"><br>Pokémon Sword<br><sub>~12.4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-shield.jpg" width="256" alt="Pokémon Shield"><br>Pokémon Shield<br><sub>~12.4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/portal.jpg" width="256" alt="Portal"><br>Portal<br><sub>~2.6 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/raging-loop.jpg" width="256" alt="Raging Loop"><br>Raging Loop<br><sub>~0.9 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/rhythm-heaven-groove.jpg" width="256" alt="Rhythm Heaven Groove"><br>Rhythm Heaven Groove<br><sub>~3 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/romance-of-the-three-kingdoms-8-remake.jpg" width="256" alt="ROMANCE OF THE THREE KINGDOMS 8 REMAKE"><br>ROMANCE OF THE THREE KINGDOMS 8 REMAKE<br><sub>~16.8 GB · controller only</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/sea-of-stars.jpg" width="256" alt="Sea of Stars"><br>Sea of Stars<br><sub>~5.1 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/shin-megami-tensei-v-vengeance.jpg" width="256" alt="SHIN MEGAMI TENSEI V: Vengeance"><br>SHIN MEGAMI TENSEI V: Vengeance<br><sub>~14.6 GB · 60 fps with 1.0.3</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/stardew-valley.jpg" width="256" alt="Stardew Valley"><br>Stardew Valley<br><sub>~0.6 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/steins-gate-reboot.jpg" width="256" alt="STEINS;GATE RE:BOOT"><br>STEINS;GATE RE:BOOT<br><sub>~6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-smash-bros-ultimate.jpg" width="256" alt="Super Smash Bros. Ultimate"><br>Super Smash Bros. Ultimate<br><sub>~17.5 GB · offline</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/taiko-risshiden-v-dx.jpg" width="256" alt="Taiko Risshiden V DX"><br>Taiko Risshiden V DX<br><sub>~3.3 GB · Japanese · played by touch</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/unicorn-overlord.jpg" width="256" alt="Unicorn Overlord"><br>Unicorn Overlord<br><sub>~4 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/yoshis-crafted-world.jpg" width="256" alt="Yoshi's Crafted World"><br><a href="dm/games/01006000040C2000/README.md">Yoshi's Crafted World</a><br><sub>~6.1 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/nier-automata.png" width="256" alt="NieR:Automata The End of YoRHa Edition"><br>NieR:Automata The End of YoRHa Edition<br><sub>~10.9 GB · 60 fps with 1.0.3</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-echoes-of-wisdom.jpg" width="256" alt="The Legend of Zelda: Echoes of Wisdom"><br>The Legend of Zelda: Echoes of Wisdom<br><sub>~7.8 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/donkey-kong-country-returns-hd.png" width="256" alt="Donkey Kong Country Returns HD"><br>Donkey Kong Country Returns HD<br><sub>~11.6 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-rabbids-sparks-of-hope.jpg" width="256" alt="Mario + Rabbids Sparks of Hope"><br>Mario + Rabbids Sparks of Hope<br><sub>~9.8 GB</sub></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/xenoblade-chronicles-2.jpg" width="256" alt="Xenoblade Chronicles 2"><br>Xenoblade Chronicles 2<br><sub>~17.1 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/xenoblade-chronicles-3.jpg" width="256" alt="Xenoblade Chronicles 3"><br>Xenoblade Chronicles 3<br><sub>~19.2 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-crystal-chronicles.jpg" width="256" alt="FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition"><br>FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition</td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/return-of-the-obra-dinn.png" width="256" alt="Return of the Obra Dinn"><br><a href="dm/games/010032E00E6E2000/README.md">Return of the Obra Dinn</a><br><sub>~0.9 GB</sub></td>
<td align="center" valign="top" width="33%"><img src="docs/images/blasphemous-2.png" width="256" alt="Blasphemous 2"><br><a href="dm/games/010040801A4C8000/README.md">Blasphemous 2</a><br><sub>~1.5 GB</sub></td>
<td></td>
</tr>
</table>
<!-- end of gallery -->

## Pack an IPA

Prefer a graphical interface to the command line? **[NX2Apple](https://github.com/LNPANDY/NX2Apple)** is a community fork with a macOS app that runs this whole pipeline for you and packs the game files into a sharded RomFS archive for a slimmer IPA. It is a separate project that follows this one; the steps below are nxnative's own command-line way.

**1. Get nxnative and its tools** (once, on a Mac with macOS 26.6 or later, [Xcode](https://apps.apple.com/app/xcode/id497799835) 27.1 and [Homebrew](https://brew.sh)). The shaders are tested with Xcode 27.1's Metal compiler. The script stops on another version and explains how to install the tested toolchain. Corruption reported with another compiler in [#242](https://github.com/seedds/nxnative/issues/242) is still under investigation.

```
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang
brew install --cask dotnet-sdk
python3 -m pip install lz4 capstone fonttools "backports.zstd; python_version < '3.14'"
```

Use Python 3.12 or newer. Extraction writes one `RomFS.img` directly; the build reads it and bundles it as `romfs.img` in the IPA. The app reads the image in place, so neither the Mac build nor the iPhone expands it into a RomFS folder.

**2. Put one game's NSPs in a folder**: the base game (an NSP, or an XCI cartridge dump), plus its update and DLC if you have them:

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

NSZ and XCZ files (compressed NSPs and XCIs) work too: the script converts them once with [nsz](https://github.com/nicoboss/nsz) (`python3 -m pip install nsz`) and keeps the NSPs in `<title ID>_build/nsp/`, which takes their full size on disk.

Any version works, with or without an update: the script packs the newest version in the folder. DLC is optional too: the DLC in the folder is packed, and without it the game sees none, as on a Switch without it. Each game was tested on one version (an update named in the table above is the one its 60 fps needs). With another version the script builds the game with that version's own shaders and says that the version was not tested; fixes made for the tested version's code, such as the 60 fps ones, don't apply to it.

**3. Run the script:**

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

The IPA appears in the same folder (here `~/Games/MarioWonder/SMBWMobile.ipa`). The first build takes 30–60 minutes. A later one reuses every step whose inputs did not change, so after a pull that changes only the host it takes about a minute. The small `.idx` file beside the IPA (`.app.key` beside a Mac app) records what was packed; keep it for the next build. You need free disk space of about twice the NSPs' size plus 10 GB. Options such as `-o` (output path) and `--app-name` are listed by `-h`.

**4. Install** the IPA with your sideloading tool (AltStore, Sideloadly, LiveContainer, …). The app gets the game's name, icon and version (the update's version, for example 1.2.1 for Mario Wonder; the base game's when there is no update). Sign it with the **Increased Memory Limit** capability (`com.apple.developer.kernel.increased-memory-limit`) if your tool can: without it iOS gives the app about 3.4 GB and closes the bigger games (Hyrule Warriors: Age of Calamity and Hades II use more, Breath of the Wild and Cuphead come close); with it a recent iPhone gives about 6 GB.

To play on a Mac with Apple silicon instead, `--platform mac-app` packs a Mac app of the game that you open from Finder ([A Mac app](docs/development.md#a-mac-app)). It shows the game's settings in a window before the game starts ([The Mac settings window](docs/settings.md#the-mac-settings-window)).

With an Apple developer account you can instead build and install from Xcode: `--prepare-xcode` makes an Xcode project for the game rather than an IPA ([Build and run on a device with Xcode](docs/development.md#build-and-run-on-a-device-with-xcode)).

**Android.** To play on an Android phone instead, see [the top of this page](#nxnative-android): a desktop app packs the game and installs it on the phone.

## Play

**Settings.** A screen before the game starts offers what the game supports: frame rate, render scale, docked or handheld mode, game language, the frame-rate counter and rumble. Everything is on one scrollable screen, grouped into **Game**, **Controls** and **Performance**, with menus, sliders and a reset to the settings the game was tested with. Tap **Play** to start; the app remembers your choices. What each setting does: [Settings](docs/settings.md).

**Touch controls.** Tap **2D** / **3D** at the top to switch layouts; the app remembers your choice. Wonder, Engage, Dread, 13 Sentinels, Link's Awakening, Ace Attorney Investigations Collection, Unicorn Overlord, Rhythm Heaven Groove, Octopath Traveler 0, Pokémon Legends: Z-A, Cuphead, Absolum, Blasphemous 2, Brain Memory Training Academy, The Great Ace Attorney Chronicles, STEINS;GATE RE:BOOT, Big Brain Academy: Brain vs. Brain, Patrick's Parabox, Celeste, Raging Loop and Mario vs. Donkey Kong start in 2D, the other games in 3D (STEINS;GATE RE:BOOT's quick save is R3, in the 3D layout). In 3D, the right stick (the camera in most games) is not drawn: swipe on the right third of the screen, starting anywhere off the buttons. In Super Mario Galaxy and Super Mario Galaxy 2, other touches outside the controls move the pointer; in Captain Toad: Treasure Tracker they touch the game's screen (touch blocks, stun enemies), and in Big Brain Academy: Brain vs. Brain too (its menus and activities).
A connected game controller hides the touch controls and keeps every button.

**Rumble.** Game vibration uses the iPhone's haptic engine or a compatible connected controller. Controllers with separate left/right haptic outputs keep those effects separate. The launcher's **Game rumble** and **Rumble strength** settings control it independently of **Touch feedback**. Phone rumble approximates the Switch's effects; physical feel and controller compatibility still need device testing.

**Motion (gyro).** A controller that supplies gyro data through Apple's controller support (DualSense, DualShock 4) drives the game's motion controls, on the Mac and the iPhone: aiming and tilt puzzles in games that read the controller's motion, such as Breath of the Wild with its Motion Controls option on. A controller without one (Backbone One, Razer Kishi and other controllers the phone mounts in) uses the iPhone's own gyro, as a Switch in handheld. Motion follows the controller in docked mode, the default. `DM_PHONE_MOTION=0` in `Documents/env.txt` stops the phone's gyro, `DM_MOTION=0` all motion. If motion misbehaves, pack with `--no-motion` (`MOTION = False` in Spyder) for an app without it; `DM_MOTION=1` in `Documents/env.txt` turns it back on.

Switch Pro Controller gyro on the Mac still needs the HID support being developed in [PR #296](https://github.com/seedds/nxnative/pull/296).

**Shaking.** Shake the iPhone to shake the controller, in games that read its motion: Super Mario Odyssey's motion moves (such as the cap throw on a shake) need it, and Odyssey only takes them in its default docked mode. A light bump does not count; a sharp flick of the phone does.

- **Game language** is English by default. Pick another when packing with `--language` (`-l`), by code or by name: `-l ja`, `-l zh-Hans`, `-l French` (`LANGUAGE` in Spyder). It must be one the game has; the script prints the game's languages and stops on one it lacks. To change it without rebuilding, pick another on the settings screen.
- **On the Mac**, a game's app starts with the settings it was tested with. To choose others before it starts (frame rate, console mode, upscaling, …), start it from [NX2Apple](https://github.com/LNPANDY/NX2Apple)'s library, or put lines like `DM_FPS_TARGET=30` in `~/Library/Application Support/nxnative/<title ID>/env.txt`. Its log is `~/Library/Logs/nxnative/<title ID>.log`.
- **Saves and logs** are in the Files app, under the app's name: `save/` and `log.txt`. When something goes wrong, send `log.txt` (the previous run is `log.prev.txt`).
- **Short stutters** the first time a new scene appears are normal: the app compiles graphics for it once and remembers them for the next launch. What it remembered, and what the game's list names, is built before the game starts, behind "Preparing shaders" and a progress bar when that takes more than half a second.
- **Reinstalling:** some sideloading tools give a reinstalled app a new data folder. Copy `save/` and `pso_user.tsv` over from the old one to keep your progress.
- **Settings in a file:** lines like `DM_OPMODE=0` (handheld mode: lower resolution, higher frame rate) in `Documents/env.txt` win over the settings screen, which then shows them greyed out. `DM_FPS_TARGET=30` keeps Breath of the Wild, Tears of the Kingdom, Octopath Traveler 0 and Octopath Traveler II at their original 30 fps, which saves battery; `DM_PRESENT_INTERVAL=game` does the same for Monster Train 2. `DM_SHOW_FPS=1` shows the frame rate at the top left (`--show-fps` when packing turns it on from the start). `DM_LAUNCHER=0` skips the settings screen.

## Known issues

- No amiibo and no online features.
- Fire Emblem Engage: after switching away from the app and back, some sounds may stay silent until you restart it.
- Fire Emblem: Three Houses: small square patches on the ground where decals land.
- Super Mario Galaxy and Super Mario Galaxy 2: effects drawn as points or lines come out one pixel wide.

## For developers

How it works, the script's options, running games on the Mac and adding a game: [docs/development.md](docs/development.md). Per-game notes: [dm/games](dm/games/README.md#games). In-depth bring-up and debugging notes: [docs/agents](docs/agents/README.md). What a graphical front end can build on: [Front ends](docs/frontends.md). Reporting a problem or sending a change: [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license; see [dm/third_party](dm/third_party/README.md).
