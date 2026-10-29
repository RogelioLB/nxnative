# Plan: the host on Windows on Arm

Status: a plan, nothing of it is built yet. Windows 11 on Snapdragon (X Elite / X Plus laptops and tablets, the 8cx generation before them) is arm64, so a
Switch game's code can run there natively, as it does on the Mac and on Android phones. The APK cannot be used: Microsoft retired the Windows Subsystem for
Android in March 2025. This plan ports the Android host (`dm/android`) to a Windows executable, reusing its Vulkan replay and its shaders.

## What carries over

| Part | On Windows |
|---|---|
| Rewriter (`dm/tools/patch_nso.py`) | Already removes the game's own `x18` uses (Apple reserves `x18`, and so does Windows: it holds the thread's TEB). A third target, `DM_TARGET=windows`, differs only in where rewritten code finds the four thread slots (below) |
| Host (`dm/host`) | Builds as on Android: Darwin interfaces from a compat layer. Android's `compat/` headers and `plat_android.c` (412 lines) are the model; most of it (CoreFoundation dictionary, SHA-256, dispatch semaphores) is plain C and is shared |
| NVN replay (`gfx_vk.c`, `gfx_vk_draw.inc`) | Vulkan on Qualcomm's Windows driver; only the surface changes (`VK_KHR_win32_surface` instead of `VK_KHR_android_surface`) |
| Shaders | The SPIR-V `spv_build.py` makes for Android, unchanged at first. Its Adreno rewrites (`spv_android.py`) target the phone driver: whether Qualcomm's Windows driver needs them is measured, not assumed |
| Packing | `scripts/build_android_from_nsp.py` already runs on Windows; the Windows target writes a folder (executable, host, `games/<id>/`) instead of an APK |

## What is new

1. **Toolchain.** [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang, `aarch64-w64-mingw32`), cross-compiling from the x64 PC: GNU assembler syntax, so
   the `.S` sources convert as for Android (`tools/asm_elf.py` becomes an ELF/COFF choice), and winpthreads for the host's 69 files that use pthreads. Built
   with x18 reserved (`-ffixed-x18`, the Windows ABI default for this target).
2. **Thread slots.** Android points `x18` at a per-thread block; Windows cannot (the kernel owns `x18`). The TEB's own TLS array is the Apple form instead: a
   slot is at `x18 + 0x1480 + 8 × index` for a `TlsAlloc` index under 64, as Apple's is at `tpidrro_el0 + 8 × key`. `dm_slot_keypage_value` gives the rewriter
   that value. The offset is the 64-bit TEB's `TlsSlots`; phase 0 checks it on the device.
3. **Memory.** The host maps the game's address space at fixed addresses and aliases memory (`vm_remap`: the loader's code pages, the NVN physical pools in
   `hle_nvn.c`; on Android `mremap`). On Windows aliasing needs memory made from a section (`CreateFileMapping` backed by the paging file) mapped with
   `MapViewOfFile3` into placeholders (`VirtualAlloc2` with `MEM_RESERVE_PLACEHOLDER`). Views fall on 64 KiB allocation granularity while Horizon maps at
   4 KiB: placeholders split finer, and phase 0 measures how far. This is the largest risk.
4. **Exceptions and thread control.** `crash.c`'s signal handlers become a vectored exception handler. Thread suspension (hang reports, the sampling probe,
   the pause in `hle_thread.c`, which Android does with real-time signals and a futex) becomes `SuspendThread` / `GetThreadContext` / `ResumeThread`, which
   Windows has natively.
5. **Front end, audio, movies, input.** A Win32 window with the Vulkan swapchain and the status screen (`front_windows.c` from `front_android.c`); audio
   output on WASAPI (from `coreaudio_aaudio.c`); H.264 and AAC on Media Foundation (from `media_android.c`); controllers through GameInput or XInput and the
   keyboard; rumble through the same.

## Phases

0. **Spike: the risky parts alone (2 to 4 days).** A small test executable, no game: TLS slots at `x18 + 0x1480 + 8 × index` read from hand-written
   assembly on several threads; fixed-address reservations and 4 KiB aliases with placeholders; a vectored handler catching a fault in a code page and
   resuming; a thread suspended and its registers read. The host's existing unit tests for memory and threads (`dm/host/tests/t_svc_memory.c`,
   `t_thread_leak.c`, `t_condvar.c`, `t_preempt.c`) are the next check. If aliasing at 4 KiB does not work, the plan stops here with nothing changed.
1. **Host without graphics (1 week).** `dm/windows/`: CMake for llvm-mingw, compat layer, `plat_windows.c`, `DM_TARGET=windows` in the rewriter, the
   `gfx_null.c` replay (no GPU). Check: a game boots to its frame loop in the command-line host, its log matching the Android one.
2. **Vulkan replay (3 to 5 days).** `gfx_vk.c` with a Win32 surface. Check: Super Mario Bros. Wonder draws, then the other games run on Android so far;
   which of `spv_android.py`'s Adreno rewrites the Windows driver needs.
3. **Window, input, audio, movies (1 week).** The front end and its diagnostics (`log.txt`, `shot_now`, `dump_now`, `threads_now` in the game's folder).
4. **Packing and the desktop app (2 to 3 days).** A Windows target in the packing script and in `nxnative_android.py`: build, copy to the device, launch.

Phases 0 to 4 are about four to six weeks. Phase 0 decides the rest.

A build for Windows on x64 PCs is a separate executable with its own host and dependencies, sharing only the Windows code that does not depend on the
processor: [plan_windows_x64.md](plan_windows_x64.md).

## Testing on the device

The PC that builds is x64 and cannot run the arm64 host, so every run is on the Windows on Arm device, driven from the PC over SSH as Android is over adb:

- **Once, on the device** (administrator PowerShell): `Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0`, then
  `Set-Service sshd -StartupType Automatic; Start-Service sshd`. The PC's public key goes in `C:\ProgramData\ssh\administrators_authorized_keys` (an
  administrator account) or `%USERPROFILE%\.ssh\authorized_keys`, so commands run without a password prompt. Both machines on the same network.
- **Copy and run headless:** `scp` the build to `C:\nxnative\`, then `ssh <user>@<address> C:\nxnative\dmhost_cli.exe ...` for the unit tests and the
  graphics-less host (phases 0 and 1); the output comes back over SSH.
- **Run with a window:** a program started over SSH runs in a session with no desktop, so the game window would not appear. A scheduled task created once
  with `schtasks /create /tn nxnative /tr C:\nxnative\run.cmd /sc once /st 00:00 /it` runs in the signed-in user's desktop; `ssh ... schtasks /run /tn nxnative`
  starts it. The user must be signed in on the device.
- **Diagnostics:** the same file switches as on Android, in the game's folder (`shot_now`, `dump_now`, `threads_now`), and `log.txt` pulled with `scp`.
- **Device sleep:** set the device not to sleep while plugged in during testing, or the SSH connection drops.
