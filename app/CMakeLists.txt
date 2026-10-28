# Ketsui: Kizuna Jigoku Tachi Extra (Xbox 360 disc, CAVE / 5pb., title 465607D1): static recompilation host application.
#
# The game code is not in this repository: run the code generator first (tools/codegen.ps1), which writes generated/
# from your own default.xex, together with the CMake glue (generated/rexglue.cmake, made by
# `rexglue codegen`).
#
# Stage 1, Windows PC (development): the SDK's emulated Xenos GPU, as the reference.
#   cmake --preset win-pc && cmake --build --preset win-pc        (from app/, inside tools\win\env.ps1)
# Stage 2: Renut's Direct3D-level renderer (futari_renderer = "renut", the default): deko3d on the Switch, plume's
# Vulkan / D3D12 on the PC (futari-nx's, through banjo-nx; cmake/ketsui_renut_gpu.cmake).
#   Switch: tools/build_switch.ps1 (devkitA64, the PC tree's shader cache first).

cmake_minimum_required(VERSION 3.25)
project(ketsui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()

# futari-nx's (peggle2-nx's, 2026-10-13): -mearly-ra=none for everything compiled for the console (devkitA64 GCC 16.1's
# AArch64 early register allocation dropped table lookups of the NEON vperm helper). Before the SDK is added.
option(KETSUI_SWITCH_EARLY_RA "Leave GCC's AArch64 early register allocation pass on (Switch; can miscompile)" OFF)
if(REXGLUE_PLATFORM_SWITCH AND NOT KETSUI_SWITCH_EARLY_RA)
    add_compile_options(-mearly-ra=none)
    add_link_options(-mearly-ra=none)
endif()

# No Vulkan driver in the Switch link (the user, 2026-10-21: deko3d only): the SDK's switch_vulkan_stub.c answers the
# Vulkan entry points the code references.
set(REXGLUE_SWITCH_LINK_NVK OFF CACHE BOOL "" FORCE)

include(generated/rexglue.cmake)

# The build re-runs codegen whenever one of its inputs changes; the post-codegen steps (tools/ketsui_post_codegen.py:
# never-ending guest loops made explicit, then futari-nx's performance steps, tools/post_codegen.py) must follow EVERY
# codegen or they are lost (futari-nx's lesson of 2026-10-06), so they are appended to the codegen command itself.
# KETSUI_PERF_CODEGEN_POST=OFF keeps the correctness step only: the generated code as rexglue writes it (the A side
# of the A/B pair).
option(KETSUI_PERF_CODEGEN_POST "Run futari-nx's performance steps (tools/post_codegen.py) after every codegen" ON)
if(TARGET ketsui_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_ketsui_post_args "")
    if(NOT KETSUI_PERF_CODEGEN_POST)
        set(_ketsui_post_args --correctness-only)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/ketsui_post_codegen.py"
                ${_ketsui_post_args})
endif()

set(KETSUI_SOURCES
    src/main.cpp
    # What the generated code needs from the application (rex_spin_forever for tools/spin_forever.py).
    src/ketsui_fixes.cpp
    # Video recordings of the game image for testing (cvar ketsui_video_s; tools/make_video.sh makes the MP4).
    src/ketsui_capture.cpp
    # Scripted pad for unattended test runs (cvar ketsui_input_script; tools/win/boot_test.ps1 -Pad).
    src/ketsui_input_script.cpp
    # PC sampling profiler (cvar ketsui_pc_profile_s). Nothing on the Switch.
    src/ketsui_pc_profiler.cpp
    # Development: guest memory words logged periodically (cvar ketsui_diag_watch).
    src/ketsui_diag.cpp
    # Windows: fatal host exceptions with module, offset and stack (logs/crash.txt for unhandled ones).
    src/ketsui_crash_log.cpp
    # The PC self-checks of the post-codegen steps ("[verify] codegen checks" every 10 s; the cores compile it too).
    src/ketsui_codegen_checks.cpp
    # Native replacements of the console profile's hottest guest functions (src/cpu/ketsui_native.h: a key each,
    # a code-hash check, verified against the recompiled function with ketsui_cpu_verify).
    src/cpu/ketsui_native.cpp
    # Ketsui's idle task thread sleeps instead of spinning (ketsui_worker_idle_sleep).
    src/cpu/ketsui_worker_idle.cpp
    # The optimisation round of 2026-10-27 (bejeweled3-nx's method, docs/ketsui-notes.md): the localized copies of
    # src/cpu/hot/localized.txt and the bulk list's in 8 shards (tools/localize_native.py), the hand-written natives
    # (src/cpu/hot/h_*.h) and their registry (keys ketsui_cpu_native_*, the CPU master, verify mode).
    src/cpu/ketsui_hot_natives.cpp
    src/cpu/ketsui_hot_guest.cpp
    src/cpu/ketsui_hot_shard_0.cpp
    src/cpu/ketsui_hot_shard_1.cpp
    src/cpu/ketsui_hot_shard_2.cpp
    src/cpu/ketsui_hot_shard_3.cpp
    src/cpu/ketsui_hot_shard_4.cpp
    src/cpu/ketsui_hot_shard_5.cpp
    src/cpu/ketsui_hot_shard_6.cpp
    src/cpu/ketsui_hot_shard_7.cpp
    # Input lag: the frame's controller read as late as the frame allows (futari_late_input).
    src/ketsui_late_input.cpp
    # PGO profile collection (the Switch build with KETSUI_PGO=generate; empty otherwise).
    src/ketsui_pgo.cpp
    # On/off keys for every enhancement + the CPU / non-CPU masters (ketsui_cpu_enhancements, ketsui_noncpu_...).
    src/ketsui_enhancements.cpp
)

if(NOT REXGLUE_PLATFORM_SWITCH)
    # The Switch profiler counters the renderer reports to (no-ops on the PC).
    list(APPEND KETSUI_SOURCES src/me/me_pc_stubs.cpp)
endif()

if(WIN32)
    add_executable(ketsui WIN32 ${KETSUI_SOURCES})
else()
    add_executable(ketsui ${KETSUI_SOURCES})
endif()

# Link the SDK. Stage 1 draws with ReXGlue's Xenos GPU emulation (the PC reference).
rexglue_setup_target(ketsui GPU_PLUGINS xenos)

# Ryujinx starts NROs without argv[0]; the SDK then takes the NRO's usual place on the SD (banjo-nx's fix).
if(REXGLUE_PLATFORM_SWITCH AND TARGET rexcore)
    target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/ketsui-nx/ketsui.nro")
endif()

target_include_directories(ketsui PRIVATE src)
# The capture includes xxhash.h directly; when the SDK is built from source it is private to its libraries.
if(TARGET xxhash)
    target_include_directories(ketsui PRIVATE $<TARGET_PROPERTY:xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Settings template next to the executable, only when there is none yet (the copy there is the user's to edit).
if(NOT REXGLUE_PLATFORM_SWITCH)
    add_custom_command(TARGET ketsui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/ketsui.toml -DDST=$<TARGET_FILE_DIR:ketsui>/ketsui.toml
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        VERBATIM)
endif()

# The SDK compiles itself with these (directory scope), but they do not reach the generated game code, which is built
# in this directory (futari-nx's rule, every game-core target included): GCC's default -ffp-contract=fast fuses
# separate PPC fmul/fadd into FMA and results drift from the Xbox 360; strict aliasing is unsafe for recompiled code.
# -fno-math-errno: nothing reads errno (one fsqrt instead of a check and a possible libm call).
set(KETSUI_GAME_TARGETS ketsui ketsui_recomp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target ${KETSUI_GAME_TARGETS})
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing -fno-math-errno)
        endif()
    endforeach()
endif()

# Not built with the generated code's precompiled header.
set_source_files_properties(src/ketsui_codegen_checks.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# futari-nx's post-codegen steps (tools/post_codegen.py, run by tools/ketsui_post_codegen.py; the user, 2026-10-22):
# one switch back per step, as futari-nx has them (FUTARI_GEN_*). OFF compiles the statements as they were before the
# step, on every platform, so a PC build can take one step out to find which of them a difference comes from. The two
# _POISON options are PC tests, never for a build that ships: every write the step left out writes 0xDEADBEEF... (a
# signalling NaN for a float register) instead, so a reader the analysis missed breaks visibly.
option(KETSUI_GEN_RO_CONSTANTS "Loads from the image's read-only data as constants" ON)
option(KETSUI_GEN_FLOAT_LOADS "Single-precision loads straight into a float register" ON)
option(KETSUI_GEN_LEAF_LOCALS "ctx registers of leaf functions with loops as locals" ON)
option(KETSUI_GEN_LOOP_LOCALS "ctx registers of call-free loops as locals for the length of the loop" ON)
option(KETSUI_GEN_COLD_TRAPS "The trap helper out of line and cold" ON)
option(KETSUI_GEN_PROMOTED_CALLS "Guarded direct calls at the promoted indirect call sites" ON)
option(KETSUI_GEN_NARROW_LOOP_BARRIER "Loop barriers clobber the guest window only (OFF: all memory)" ON)
option(KETSUI_GEN_DCBT_PREFETCH "The guest's dcbt / dcbtst hints as host prefetches" ON)
option(KETSUI_GEN_VECTOR_LOCALS_BY_VALUE "Vector registers of the loop and leaf locals as values, not references" ON)
option(KETSUI_GEN_VPERM1 "A vperm of one vector as a one-register lookup" ON)
option(KETSUI_GEN_FP_SIGN_OPS "fneg / fabs / fnabs as float operations" ON)
option(KETSUI_GEN_LEAF_DEAD "Leaf write-backs no caller can read left out" ON)
option(KETSUI_GEN_LEAF_DEAD_POISON "PC test: the left-out leaf write-backs write poison" OFF)
option(KETSUI_GEN_SINGLE_FLOATS "Single-precision arithmetic in float where the inputs are proven singles" ON)
option(KETSUI_GEN_INLINE_MEMCALLS "memcpy / memset of a constant size up to 64 bytes inline" ON)
option(KETSUI_GEN_VEC_UNALIGNED "stvlx / stvrx as a few stores instead of byte loops" ON)
option(KETSUI_GEN_RO_CFG "Read-only constants found over the control-flow graph" ON)
option(KETSUI_GEN_DEAD_DEFS "Guest register writes nothing reads left out" ON)
option(KETSUI_GEN_DEAD_DEFS_POISON "PC test: the left-out register writes write poison" OFF)
option(KETSUI_GEN_FLUSH_HOIST "Loop-header flush-mode checks in front of the loop" ON)
option(KETSUI_GEN_LR_WRITE_SAME "Proven link-register write-backs compile to nothing (the PC build checks each)" ON)
option(KETSUI_GEN_R1_LOCAL "Guest r1 as a local mirror (the PC build checks the assumed calls)" ON)
option(KETSUI_GEN_VSEL_NEON "VMX vsel as one NEON bit select" ON)
option(KETSUI_GEN_INLINE_LEAVES "Inline copies of tiny leaf functions in the files that call them" ON)
set(_ketsui_gen_definitions "")
if(NOT KETSUI_GEN_RO_CONSTANTS)
    list(APPEND _ketsui_gen_definitions REX_RO_CONSTANTS_OFF)
endif()
if(NOT KETSUI_GEN_FLOAT_LOADS)
    list(APPEND _ketsui_gen_definitions REX_FLOAT_LOADS_INTEGER)
endif()
if(NOT KETSUI_GEN_LEAF_LOCALS)
    list(APPEND _ketsui_gen_definitions REX_LEAF_LOCALS_OFF)
endif()
if(NOT KETSUI_GEN_LOOP_LOCALS)
    list(APPEND _ketsui_gen_definitions REX_LOOP_LOCALS_OFF)
endif()
if(NOT KETSUI_GEN_COLD_TRAPS)
    list(APPEND _ketsui_gen_definitions "REX_TRAP_ATTR=")
endif()
if(NOT KETSUI_GEN_PROMOTED_CALLS)
    list(APPEND _ketsui_gen_definitions REX_PROMOTED_CALLS_OFF)
endif()
if(NOT KETSUI_GEN_NARROW_LOOP_BARRIER)
    list(APPEND _ketsui_gen_definitions REX_LOOP_BARRIER_FULL)
endif()
if(NOT KETSUI_GEN_DCBT_PREFETCH)
    list(APPEND _ketsui_gen_definitions "REX_PREFETCH(x,write)=")
endif()
if(KETSUI_GEN_VECTOR_LOCALS_BY_VALUE)
    list(APPEND _ketsui_gen_definitions REX_VECTOR_LOCALS_BY_VALUE)
endif()
if(NOT KETSUI_GEN_VPERM1)
    list(APPEND _ketsui_gen_definitions REX_VPERM1_OFF)
endif()
if(NOT KETSUI_GEN_FP_SIGN_OPS)
    list(APPEND _ketsui_gen_definitions REX_FP_SIGN_INTEGER)
endif()
if(NOT KETSUI_GEN_LEAF_DEAD)
    list(APPEND _ketsui_gen_definitions REX_LEAF_DEAD_KEEP)
elseif(KETSUI_GEN_LEAF_DEAD_POISON)
    list(APPEND _ketsui_gen_definitions REX_LEAF_DEAD_POISON)
endif()
if(NOT KETSUI_GEN_SINGLE_FLOATS)
    list(APPEND _ketsui_gen_definitions REX_SINGLE_FLOATS_OFF)
endif()
if(NOT KETSUI_GEN_INLINE_MEMCALLS)
    list(APPEND _ketsui_gen_definitions REX_INLINE_MEMCALLS_OFF)
endif()
if(NOT KETSUI_GEN_VEC_UNALIGNED)
    list(APPEND _ketsui_gen_definitions REX_VEC_UNALIGNED_LOOPS)
endif()
if(NOT KETSUI_GEN_RO_CFG)
    list(APPEND _ketsui_gen_definitions REX_RO_CFG_OFF)
endif()
if(NOT KETSUI_GEN_DEAD_DEFS)
    list(APPEND _ketsui_gen_definitions REX_DEAD_DEFS_KEEP)
elseif(KETSUI_GEN_DEAD_DEFS_POISON)
    list(APPEND _ketsui_gen_definitions REX_DEAD_DEFS_POISON)
endif()
if(NOT KETSUI_GEN_FLUSH_HOIST)
    list(APPEND _ketsui_gen_definitions REX_FLUSH_HOIST_OFF)
endif()
if(NOT KETSUI_GEN_LR_WRITE_SAME)
    list(APPEND _ketsui_gen_definitions REX_LR_WRITE_SAME_OFF)
elseif(NOT REXGLUE_PLATFORM_SWITCH)
    # The PC build compares every proven write-back with the value ctx.lr holds ("[verify] codegen checks").
    list(APPEND _ketsui_gen_definitions REX_LR_WRITE_SAME_CHECK)
endif()
if(NOT KETSUI_GEN_R1_LOCAL)
    list(APPEND _ketsui_gen_definitions REX_R1_LOCAL_OFF)
elseif(NOT REXGLUE_PLATFORM_SWITCH)
    # The PC build compares the mirror with ctx.r1 after every kept or assumed call ("[verify] codegen checks").
    list(APPEND _ketsui_gen_definitions REX_R1_CHECK)
endif()
if(NOT KETSUI_GEN_VSEL_NEON)
    list(APPEND _ketsui_gen_definitions REX_VSEL_OFF)
endif()
if(NOT KETSUI_GEN_INLINE_LEAVES)
    list(APPEND _ketsui_gen_definitions REX_INLINE_LEAVES_OFF)
endif()
# futari-nx's CPU round 1 (sasrt-nx's, through sa2-nx), the Switch's:
#   - the flush-mode checks the control flow fixes compile to nothing (tools/flush_mode_flow.py; the PC build keeps
#     and counts them); OFF defines REX_FLUSH_FLOW_OFF;
#   - guest compare-and-swap without the legacy builtin's trailing `dmb ish` (tools/guest_cas.py); OFF defines
#     REX_GUEST_CAS_LEGACY (the run-time key ketsui_cpu_guest_cas_no_barrier puts the barrier back without a build);
#   - guest vector loads and stores reversed with REV64 + EXT instead of a table lookup (tools/vec_byteswap.py); OFF
#     defines REX_BSWAP128_TABLE;
#   - constant word shuffles as one permutation (tools/pch_neon_shuffles.py); OFF defines REX_SIMDE_SHUFFLES;
#   - vperm and the float-to-integer conversions as their NEON instruction (rex/ppc/intrinsics.h); OFF defines
#     REX_VMX_HELPERS_X86_FORM.
if(REXGLUE_PLATFORM_SWITCH)
    option(KETSUI_SWITCH_FLUSH_FLOW "Flush-mode checks proven by control flow compile to nothing (Switch)" ON)
    option(KETSUI_SWITCH_GUEST_CAS "Guest compare-and-swap without the trailing dmb (Switch)" ON)
    option(KETSUI_SWITCH_VEC_BYTESWAP "Guest vector byte reversal as REV64 + EXT (Switch)" ON)
    option(KETSUI_SWITCH_NEON_SHUFFLES "Constant word shuffles as one permutation (Switch)" ON)
    option(KETSUI_SWITCH_NEON_VMX_HELPERS "vperm and float-to-integer conversions as NEON instructions (Switch)" ON)
    if(NOT KETSUI_SWITCH_FLUSH_FLOW)
        list(APPEND _ketsui_gen_definitions REX_FLUSH_FLOW_OFF)
    endif()
    if(NOT KETSUI_SWITCH_GUEST_CAS)
        list(APPEND _ketsui_gen_definitions REX_GUEST_CAS_LEGACY)
    endif()
    if(NOT KETSUI_SWITCH_VEC_BYTESWAP)
        list(APPEND _ketsui_gen_definitions REX_BSWAP128_TABLE)
    endif()
    if(NOT KETSUI_SWITCH_NEON_SHUFFLES)
        list(APPEND _ketsui_gen_definitions REX_SIMDE_SHUFFLES)
    endif()
    if(NOT KETSUI_SWITCH_NEON_VMX_HELPERS)
        list(APPEND _ketsui_gen_definitions REX_VMX_HELPERS_X86_FORM)
    endif()
endif()
foreach(target ${KETSUI_GAME_TARGETS})
    if(TARGET ${target} AND _ketsui_gen_definitions)
        target_compile_definitions(${target} PRIVATE ${_ketsui_gen_definitions})
    endif()
endforeach()
message(STATUS "ketsui: post-codegen steps ${KETSUI_PERF_CODEGEN_POST}; game code definitions: "
               "${_ketsui_gen_definitions}")

# Renut's Direct3D-level renderer (futari_renderer = "renut"; cmake/ketsui_renut_gpu.cmake): Ketsui's Direct3D
# natively through plume, no PM4 ring and no EDRAM model. Built in by default; off, the build has the emulated GPU only
# (PC).
option(FUTARI_RENUT_RENDERER "Renut's Direct3D-level renderer (futari_renderer = renut)" ON)
if(FUTARI_RENUT_RENDERER)
    include(cmake/ketsui_renut_gpu.cmake)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # Link map: which object (game core) each function of a profile comes from (tools/switch_hot_functions.py).
    target_link_options(ketsui PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/ketsui.map")
    # The Switch build's fixed settings, compiled in (src/ketsui_app.h BuiltInConfig; tools/make_ketsui_configs.py
    # writes them). The SD card's ketsui.toml holds the player's options and overrides these key by key.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_switch_defaults.toml" KETSUI_BUILTIN_CONFIG)
    # And the player's ketsui.toml, written next to the NRO when it is missing (src/ketsui_app.h DefaultConfigFile).
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_switch.toml" KETSUI_PLAYER_CONFIG)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ketsui_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/ketsui_builtin_config.h" @ONLY)
    target_include_directories(ketsui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(ketsui PRIVATE KETSUI_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_switch_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_switch.toml")
    # futari-nx's Switch compiler optimisations (sa2-nx's set, from nfsmw-nx / UnleashedRecomp-NX):
    #   - LTO: the recompiled default.xex code (ketsui_recomp) and the app (renderer) are compiled to GIMPLE and
    #     optimised as one program at the link (-flto=N -flto-partition=balanced: N parallel LTRANS jobs, kept low for
    #     the build machine's memory). The SDK libraries and the four game cores (partially linked with ld -r +
    #     objcopy, which needs machine code) stay out.
    #   - -fipa-pta: interprocedural points-to analysis at the LTO link.
    #   - The link repeats -ffp-contract=off and -fno-strict-aliasing (the LTRANS stage must not fuse what the compile
    #     stage kept apart), and xxHash is built with XXH_FORCE_MEMORY_ACCESS=0 (nfsmw-nx's garbage cache keys).
    # OFF by default (the user, 2026-10-21): test builds go without it (the LTO link alone takes most of an hour); the
    # final build turns it on (tools/win/build_switch.ps1 -Lto on).
    option(KETSUI_SWITCH_LTO "Link-time optimisation of the default.xex code and the app (Switch)" OFF)
    set(KETSUI_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    option(KETSUI_SWITCH_IPA_PTA "-fipa-pta at the Switch LTO link" ON)
    target_compile_definitions(ketsui PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(KETSUI_SWITCH_LTO)
        set(_ketsui_lto_compile -flto -fno-fat-lto-objects)
        set(_ketsui_lto_link -flto=${KETSUI_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                              -fno-strict-aliasing)
        if(KETSUI_SWITCH_IPA_PTA)
            list(APPEND _ketsui_lto_compile -fipa-pta)
            list(APPEND _ketsui_lto_link -fipa-pta)
        endif()
        foreach(target ketsui_recomp ketsui)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_ketsui_lto_compile})
            endif()
        endforeach()
        target_link_options(ketsui PRIVATE ${_ketsui_lto_link})
        message(STATUS "ketsui: Switch LTO on (${KETSUI_SWITCH_LTO_JOBS} LTRANS jobs, -fipa-pta ${KETSUI_SWITCH_IPA_PTA})")
    endif()
    # No x29 frame chain in the game code (futari-nx's CPU round 1): one more callee-saved register for the recompiled
    # code. The SDK and the app keep theirs (the console profiler walks x29, bounds-checked).
    option(KETSUI_SWITCH_OMIT_FRAME_POINTER "Game code without the x29 frame chain (Switch)" ON)
    if(KETSUI_SWITCH_OMIT_FRAME_POINTER)
        foreach(target ${KETSUI_GAME_TARGETS})
            if(TARGET ${target} AND NOT target STREQUAL "ketsui")
                target_compile_options(${target} PRIVATE -fomit-frame-pointer)
            endif()
        endforeach()
    endif()
    # Line tables for the app's own code (addr2line on the archived ELF); DWARF 4 (sa2-nx: DWARF 5 mapped some LTO
    # functions' lines to the wrong file). The generated game code stays without.
    option(KETSUI_SWITCH_LINE_INFO "Line tables (-g1) for the app's own code in the Switch ELF" ON)
    if(KETSUI_SWITCH_LINE_INFO)
        target_compile_options(ketsui PRIVATE -g1 -gdwarf-4)
        target_link_options(ketsui PRIVATE -g1 -gdwarf-4)
    endif()
    # Profile-guided optimisation (futari-nx's, sa2-nx's design; the user, 2026-10-22), for the generated default.xex
    # code and the app (the four game cores, partially linked with ld -r + objcopy, stay out as in futari-nx):
    #   KETSUI_PGO=generate  an instrumented NRO that writes its branch counts to pgo/ next to it on the SD card
    #                         (src/ketsui_pgo.cpp: registered in a section, not libgcov's exit-time files, which do not
    #                         fit the Switch). Value profiling is off: its shared lists are not safe without atomics.
    #   KETSUI_PGO=use       the counts merged on the PC (tools/win/pgo_merge.ps1). Code the sessions did not run is
    #                         compiled as without a profile (-fprofile-partial-training), and functions changed since
    #                         the instrumented build keep no profile (-Wno-coverage-mismatch).
    # The profile is pgo/ at the root of the repository (KETSUI_PGO_DIR): one .gcda per object, named by the object's
    # path in the build folder with # for each separator (-fprofile-prefix-path: the instrumented build's folder,
    # out/switch_pgo, and the release's, out/switch, give the same names).
    set(KETSUI_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE KETSUI_PGO PROPERTY STRINGS "" generate use)
    set(KETSUI_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    get_filename_component(_ketsui_pgo_dir "${KETSUI_PGO_DIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_ketsui_pgo_dir}" _ketsui_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _ketsui_pgo_prefix)
    if(KETSUI_PGO STREQUAL "generate")
        set(_ketsui_pgo "-fprofile-generate=${_ketsui_pgo_dir}" "-fprofile-prefix-path=${_ketsui_pgo_prefix}"
                         -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(ketsui PRIVATE KETSUI_PGO_GENERATE=1)
    elseif(KETSUI_PGO STREQUAL "use")
        if(NOT EXISTS "${KETSUI_PGO_DIR}")
            message(FATAL_ERROR "KETSUI_PGO=use: no profile in ${KETSUI_PGO_DIR}")
        endif()
        # -fprofile-correction: counts taken without atomic updates (-fprofile-update=single) while several game
        # threads ran the same code lose a few increments; GCC smooths them instead of stopping.
        set(_ketsui_pgo "-fprofile-use=${_ketsui_pgo_dir}" "-fprofile-prefix-path=${_ketsui_pgo_prefix}"
                         -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                         -Wno-coverage-mismatch)
    elseif(KETSUI_PGO)
        message(FATAL_ERROR "KETSUI_PGO must be empty, generate or use (got ${KETSUI_PGO})")
    endif()
    if(KETSUI_PGO)
        # Renut's renderer too (an object library of the app: the render thread's code).
        foreach(target ketsui_recomp ketsui ketsui_renut_gpu)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_ketsui_pgo})
            endif()
        endforeach()
        target_link_options(ketsui PRIVATE ${_ketsui_pgo})
        message(STATUS "ketsui: PGO ${KETSUI_PGO}")
    endif()
    # The homebrew menu's entry: name, author, version and icon (app/ketsui_icon.jpg, 256x256 JPEG).
    set(_ketsui_nro_icon "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_icon.jpg")
        set(_ketsui_nro_icon ICON "${CMAKE_CURRENT_SOURCE_DIR}/ketsui_icon.jpg")
    endif()
    rexglue_switch_add_nro(ketsui
        NAME "Ketsui: Kizuna Jigoku Tachi"
        AUTHOR "ChanseyIsTheBest"
        VERSION "1.0.0"
        ${_ketsui_nro_icon})
endif()
