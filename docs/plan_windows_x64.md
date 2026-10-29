# Plan: the host on Windows on x64

Status: a plan, nothing of it is built yet. A separate build from [the host on Windows on Arm](plan_windows_arm.md): its own host executable compiled
for x64 (clang or MSVC, `x86_64-pc-windows`), its own directory (`dm/windows_x64/`), CMake project and dependencies (dynarmic, no arm64 toolchain), and its
own output from the packer. It does not run or translate the Arm build. Source that does not depend on the processor (the Win32 window, the Vulkan replay,
WASAPI audio, Media Foundation movies, GameInput controllers) is written once and compiled into both, as `gfx_vk.c` is shared today by Android and nothing
else; neither build waits for the other.

## Why it is a different kind of port

Everywhere else this repository runs the game's own arm64 code on an arm64 processor: the rewriter changes only what the operating system forbids, and the
game calls the host directly (its imports bound to host functions, `svc` turned into a branch to `dm_gate`, NVN calls into `nvn_thunks.S`), both sides using
the same calling convention. An x64 processor cannot execute arm64 instructions, and Windows on x64 does not emulate arm64 (only the reverse). The game's code
must be translated to x64 while it runs, by a recompiler.

The recompiler is [dynarmic](https://github.com/lioncash/dynarmic) (0BSD licence): an A64 front end and an x64 back end, with the exclusive monitor and
the page-table memory model the Switch emulators built on it used. It is a library: the host gives it guest memory, receives callbacks for `svc`, unknown
instructions and exceptions, and runs one translation context per guest thread.

## When the game's code is translated

The host is native x64. The game's code is arm64 machine code and is translated either way; the choice is when:

- **While the game runs (JIT, dynarmic).** Each block is translated the first time it runs and kept in a cache. Proven by the Switch emulators; costs some
  time the first time each part of the game runs. This is the first implementation.
- **When the game is packed (static recompilation, later).** The packer translates the game's modules to x64 ahead of time and links them into the
  executable: no translation while playing. The rewriter already finds every function and resolves jump tables (`patch_nso.py`), which is what an
  ahead-of-time pass needs. What it cannot see in advance (code reached through pointers it could not resolve, NROs loaded at run time, code the game
  generates) still goes to the JIT, so the JIT stays as the fallback. Built on top of the JIT version, once that runs games, if its speed is not enough.

## What changes

1. **The game's code runs in dynarmic.** Each guest thread (`hle_thread.c`) owns a dynarmic `A64::Jit` whose registers are the thread's; the host's thread
   still owns the host stack and the scheduling. Guest memory is the host's own mapping at the same addresses (reserved as the Windows on Arm plan
   describes), given to dynarmic as a flat page table with fastmem, so loads and stores are single x64 instructions.
2. **The rewriter does less.** No operating system forbids `x18` or `tpidrro_el0` inside the recompiler: dynarmic presents them as guest registers. The cuts
   that change a game's behaviour (`patches.py`, `sdk_patches.py`: frame steps, observation gates) still apply, as guest code. `DM_TARGET=x64` keeps those
   and drops the register rewrites.
3. **Every call between the game and the host goes through a bridge.** Today an import bound to a host function is a plain arm64 call. On x64 an import
   resolves to an address that ends the dynarmic block and enters the host: a table of host functions, each with a generated adapter that reads the
   arguments from the guest's `x0`–`x7`, `v0`–`v7` and stack (AAPCS64) and calls the x64 function, then writes the result back. `gen_protos.py` already
   generates `nvn_thunks.S` from the NVN prototypes: it generates these adapters instead, and the same for the service and SDK functions the host
   implements. Calls the other way (thread entries, module initialisers, the game's callbacks such as allocators and NVN's debug callback) run the guest
   function in the calling thread's `A64::Jit` until it returns to a sentinel address.
4. **Structures shared with the game.** Both sides are 64-bit and little-endian, so the host's views of guest structures (`NVNshaderData`, `prog_t`, the
   service buffers) keep their layout. Host code compiled for x64 must not assume arm64 instructions: the inline assembly (`hle_thread.c`'s stack switch,
   `loader.c`'s `tpidrro_el0` check, `dm_slots_*`) gets x64 or recompiler equivalents.

## Phases

0. **Spike: dynarmic alone (3 to 5 days).** dynarmic built with clang for `x86_64-w64-mingw32` beside the host; one guest function from a game module (a pure
   computation) runs in it and returns the arm64 result; one `svc` comes back to a host callback.
1. **The bridge (2 to 3 weeks).** The generated adapters for NVN and the host's service and SDK functions, the sentinel return for host-to-guest calls,
   threads each with a `Jit`. Check: a game reaches its frame loop with `gfx_null.c`, its log matching the Android one.
2. **Vulkan and the rest (1 week).** The shared Windows front end and Vulkan replay on x64 GPUs (NVIDIA, AMD, Intel): the shaders are SPIR-V already, but the Adreno
   rewrites in `spv_android.py` are not wanted there, so the packer chooses the shader variant per GPU vendor.
3. **Speed (open).** Frame time against the Android build of the same game; dynarmic's block cache warmed at start, its optimisation passes, and the
   host functions the game calls most bridged without adapters.

Phases 0 to 2 are about seven to nine weeks with the Windows code (window, Vulkan surface, audio, movies, input) written here, five to seven if the Arm
build has already written it. Static recompilation is a later project of its own (months), only if the JIT build is too slow. Phase 0 decides the rest.

## Risks

- **It is no longer native.** Recompiled code is slower than the game's own instructions; desktop x64 processors have the margin for the Switch's four
  1 GHz cores, low-power laptops may not.
- **The bridge is the bulk of the work.** Every host function the game can call needs an adapter generated from its prototype; varargs functions
  (`printf`-style logging in the SDK) and structures passed by value need cases written by hand.
- **Host code that reads the game's registers** (hang reports, the sampling probe, `threads_now`, crash reports) reads them from the thread's `Jit`
  instead of a suspended thread's context.

## Testing

Unlike the Arm build, the x64 build runs on the PC that builds it: no SSH. The command-line host and the unit tests run directly; a game starts from the
desktop app or the command line, with the same diagnostics files (`log.txt`, `shot_now`, `dump_now`, `threads_now`).
