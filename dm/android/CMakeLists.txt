# Android build of the host (arm64-v8a): the shared host sources (dm/host, the C parts of dm/gfx) with the Darwin stand-ins of compat/, the
# Android front end and graphics backend. Two products from one set of objects:
#   libdmhost.so  the app's native library (NativeActivity, apk/)
#   dmhost        a command-line host for adb shell (headless: no window, silent audio unless DM_AUDIO=1), like the Mac's dmhost
# Configure with the NDK's toolchain file and the game profile, as the Mac host is built per profile:
#   cmake -G Ninja -S dm/android -B <build dir> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
#         -DANDROID_PLATFORM=android-29 -DGAME=<base game title ID> [-DDM_GAMES_DIR=<profiles folder outside dm/games>]
cmake_minimum_required(VERSION 3.22)
project(dmhost_android C ASM)

if(NOT GAME)
  message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (dm/games/<id>)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/${GAME} ${GEN}/asm)

# The profile header (dm/games/game.py header), regenerated at every configure and build; game.py writes it only when it changes
set(GAME_H ${GEN}/${GAME}/game_profile.h)
set(GAME_ENV)
if(DM_GAMES_DIR)
  set(GAME_ENV ${CMAKE_COMMAND} -E env DM_GAMES_DIR=${DM_GAMES_DIR})
endif()
add_custom_target(game_profile ALL
  COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H}
  BYPRODUCTS ${GAME_H} COMMENT "game profile ${GAME}")
execute_process(COMMAND ${GAME_ENV} ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GAME_H} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "game.py header ${GAME} failed")
endif()

# Assembly: the Mach-O sources converted to ELF (tools/asm_elf.py)
set(ASM_ELF)
foreach(s ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  add_custom_command(OUTPUT ${GEN}/asm/${n}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py ${s} ${GEN}/asm/${n}
    DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py)
  list(APPEND ASM_ELF ${GEN}/asm/${n})
endforeach()

# Host sources: every dm/host/*.c, as the Mac Makefile's wildcard, less the ones Android replaces
file(GLOB HOST_C ${DM}/host/*.c)
list(REMOVE_ITEM HOST_C ${DM}/host/hle_movie_avc.c)   # VideoToolbox: media_android.c (MediaCodec)
# Graphics: the NVN interface layer and texture code (the Metal replay layer, the Vulkan and GL layers are Objective-C on Metal: backend_*.c stand in)
file(GLOB GFX_C ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
file(GLOB_RECURSE VPX_C ${DM}/third_party/libvpx/*.c)

# The NVN replay: gfx_vk.c (Vulkan) by default; -DDM_GFX=null builds gfx_null.c (no GPU: submissions completed on the CPU, nothing drawn)
set(DM_GFX vk CACHE STRING "NVN replay backend: vk or null")
set(ANDROID_C
  ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/haptics_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c
  ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c)

# char is signed, as on Apple arm64 (the host and its third-party code were written there); AArch64 Linux / Android makes plain char unsigned. bcdec read BC4 / BC5 SNORM
# endpoints through (char): negative ones came out as 128..255 and every signed normal map decoded to noise (sparkling ground, bright seams in clouds)
add_compile_options(-fsigned-char)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w -fvisibility=hidden)

# The replay records every draw on the game's render thread (descriptor writes, pipeline and view lookups, vertex and uniform copies): -O2 there, after the target's -O1
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/gfx_vk.c PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(${DM}/gfx/nvn/hle_nvn.c PROPERTIES COMPILE_OPTIONS "-fno-omit-frame-pointer")   # frame records: DM_NVN_SIZE_TRACE walks the game's call chain from them
add_library(dmcore OBJECT ${HOST_C} ${GFX_C} ${ANDROID_C} ${ASM_ELF})
add_dependencies(dmcore game_profile)
target_include_directories(dmcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include
  ${GEN}/${GAME} ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis)
# x18 holds the guest thread's slot block (dm.h dm_slot_get): the compiler must never use it (Android reserves it by default; stated anyway)
target_compile_options(dmcore PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-format -Wno-deprecated-declarations -Wno-gnu-folding-constant -Wno-multichar -Wno-four-char-constants -Wno-unknown-pragmas -Wno-missing-braces>
  -ffixed-x18 -fvisibility=hidden -fPIC -march=armv8.2-a+lse)
target_compile_definitions(dmcore PRIVATE DM_ANDROID=1)

set(DM_LIBS vpx log android aaudio mediandk vulkan m dl)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(dmhost_app SHARED $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_android.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c ${GLUE}/android_native_app_glue.c)
target_include_directories(dmhost_app PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES> ${GLUE})
target_compile_options(dmhost_app PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_app ${DM_LIBS} jnigraphics)
target_link_options(dmhost_app PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384 -u ANativeActivity_onCreate)
set_target_properties(dmhost_app PROPERTIES OUTPUT_NAME dmhost)

add_executable(dmhost_cli $<TARGET_OBJECTS:dmcore> ${CMAKE_CURRENT_SOURCE_DIR}/front_cli.c ${CMAKE_CURRENT_SOURCE_DIR}/nvn_demo.c)
set_target_properties(dmhost_cli PROPERTIES OUTPUT_NAME dmhost)
target_include_directories(dmhost_cli PRIVATE $<TARGET_PROPERTY:dmcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(dmhost_cli PRIVATE -include dm_android.h -ffixed-x18 -O1 -g -Wall)
target_link_libraries(dmhost_cli ${DM_LIBS})
target_link_options(dmhost_cli PRIVATE -Wl,-z,max-page-size=16384 -Wl,--error-limit=0)

# Host unit tests for adb shell (-DDM_TESTS=ON): the sources and flags of dm/host/tests/Makefile, with the Android stand-ins (plat_android.c)
option(DM_TESTS "build dm/host/tests for the device" OFF)
if(DM_TESTS)
  set(T ${DM}/host/tests)
  set(TEST_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/plat_android.c)
  function(dm_test name)
    cmake_parse_arguments(A "" "PROFILE" "SOURCES;DEFS" ${ARGN})
    add_executable(${name} ${A_SOURCES} ${TEST_COMMON})
    add_dependencies(${name} game_profile)
    if(A_PROFILE)
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${A_PROFILE})
    else()
      target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${DM}/host ${GEN}/${GAME} ${DM}/games/${GAME})
    endif()
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:-include dm_android.h -Wno-unused-function -Wno-format -Wno-deprecated-declarations> -O1 -g -ffixed-x18)
    target_compile_definitions(${name} PRIVATE ${A_DEFS})
    target_link_libraries(${name} log)
  endfunction()
  dm_test(t_thread_leak SOURCES ${T}/t_thread_leak.c)
  dm_test(t_condvar SOURCES ${T}/t_condvar.c ${T}/t_condvar_stub.c ${DM}/host/hle_thread.c DEFS DM_TEST_HOOKS)
  dm_test(t_fs_commit_list SOURCES ${T}/t_fs_commit_list.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c)
  dm_test(t_svc_memory SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile)
  dm_test(t_svc_memory_unlimited SOURCES ${T}/t_svc_memory.c PROFILE ${T}/memory_profile DEFS DM_GAME_MEM_MAP_LIMIT=0)
  # t_svc_memory_16k is not built: it simulates 16 KiB pages over the kernel's, which works where mmap aligns to 16 KiB (Apple silicon); a 4 KiB kernel gives
  # the guest address space only 4 KiB alignment. A 16 KiB-page Android kernel runs t_svc_memory itself at its real page size
  dm_test(t_nro SOURCES ${T}/t_nro.c)
  dm_test(t_audio SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_audio.c ${CMAKE_CURRENT_SOURCE_DIR}/coreaudio_aaudio.c)
  target_link_libraries(t_audio aaudio mediandk m)
  dm_test(t_media SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_media.c ${CMAKE_CURRENT_SOURCE_DIR}/media_android.c)
  target_link_libraries(t_media mediandk android m)
  dm_test(t_hid SOURCES ${T}/t_hid.c ${DM}/host/hle_haptics.c)
  dm_test(t_preempt SOURCES ${T}/t_preempt.c ${T}/t_condvar_stub.c)
  dm_test(t_fs SOURCES ${T}/t_fs.c ${T}/t_fs_stub.c ${DM}/host/hle_bigstack.c)
  # The NVN interface layer's tests (dm/gfx/tests/t_nvn.c includes hle_nvn.c) against the GPU-less replay of gfx_null.c
  set(GFX_NO_IF ${GFX_C})
  list(REMOVE_ITEM GFX_NO_IF ${DM}/gfx/nvn/hle_nvn.c)
  dm_test(t_nvn SOURCES ${DM}/gfx/tests/t_nvn.c ${DM}/host/crash.c ${DM}/host/hle_bigstack.c ${GFX_NO_IF} ${CMAKE_CURRENT_SOURCE_DIR}/gfx_${DM_GFX}.c
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_android_stubs.c ${CMAKE_CURRENT_SOURCE_DIR}/apple_stubs.c ${GEN}/asm/nvn_entry.S ${GEN}/asm/nvn_thunks.S)
  target_include_directories(t_nvn PRIVATE ${DM}/gfx/nvn/include ${DM}/gfx/nvn ${DM}/gfx/tex ${DM}/third_party/vulkan-headers/include)
  target_compile_options(t_nvn PRIVATE -march=armv8.2-a+lse)
  target_link_libraries(t_nvn vulkan)
  target_link_options(t_nvn PRIVATE -Wl,--error-limit=0)
endif()
