# Building ketsui-nx

How to build the Windows PC build (used for development and testing), the Nintendo Switch NRO and its shaders from
your own copy of the game. To just play it, use the installer page instead (README). The tree is guwange-nx's, itself
futari-nx's, so [futari-notes.md](futari-notes.md) explains most of the tools named here and
[ketsui-notes.md](ketsui-notes.md) what is Ketsui's own.

## What you need

- **Your own copy of Ketsui Kizuna Jigoku Tachi Extra for Xbox 360** (the Japanese disc, title `465607D1`): a disc
  image (`.iso`) of it.
- **Windows 10/11** with:
  - VS 2022 Build Tools (the MSVC libraries and the Windows SDK; CMake and Ninja come with it);
  - LLVM clang 20 (a portable copy is fine);
  - Python 3 and Git.

  `tools/win/env.ps1` finds them.
- **For the Switch:** [devkitPro](https://devkitpro.org) with devkitA64, libnx and deko3d, and the Vulkan headers of
  nfsmw-nx's Mesa build (`KETSUI_NVK_SDK`; plume is built with them, the NRO links no Vulkan driver).

Run every build command from PowerShell, through `tools\win\run.ps1` or the scripts below (they load
`tools\win\env.ps1`). Running `cmake` from a Bash shell picks up MSYS2's tools: a configure started there fails and
wipes the build folder's cache.

## 1. Extract the game

The disc's files go into `assets\game_root` (not committed).

```powershell
python tools\fetch_thirdparty.py                         # third-party sources of the SDK (ReXGlue SDK v0.10.0)
python tools\extract_iso.py <disc image>                 # -> assets\game_root (default.xex, Data\, ...)
```

## 2. Translate the game's code to C++

```powershell
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"
powershell -File tools\codegen.ps1                       # default.xex -> app\generated (git-ignored)
python tools\renut_ketsui_hooks.py                       # the renderer's hook sites in the generated code
python tools\hand_natives.py --all                       # the hand-written natives, from the localized copies
python tools\ketsui_post_codegen.py                      # again: the localized copies' hooks go to those natives
```

`tools\codegen.ps1` reads `app\ketsui_manifest.toml` (the generator's options are in the files it includes:
`app\ketsui_overrides.toml`, `app\ketsui_perf.toml`, with the 360-exact vector options) and writes the decompressed
image of `default.xex` (`out\default_image.bin`), which the shader tools read. After the generation it runs
`tools\ketsui_post_codegen.py`: `tools\spin_forever.py` (a correctness step: guest loops that never end, which clang
would otherwise delete) and futari-nx's performance steps (`tools\post_codegen.py`), among them the localized copies of
the hottest functions (`app/src/cpu/hot/localized*.txt`, `tools/localize_native.py`). The build runs them again after
every code generation, and a second run changes nothing. `-NoPerf` runs the correctness step only; every performance
step has a CMake switch back (`KETSUI_GEN_*`, `KETSUI_SWITCH_*` in `app/CMakeLists.txt`). `-Force` regenerates after a
change of the generator itself.

`tools\hand_natives.py --all` writes `app/src/cpu/hot/h_<ADDR>.h` from those copies (git-ignored: they are made of
the generated code; its docstring is their audit). Without them the build uses the copies themselves.

## 3. The PC build

```powershell
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc -j 4"
powershell -File tools\win\boot_test.ps1                 # muted and minimized, then the end of the log
```

`out\pc\ketsui.toml` holds the PC's settings; test runs are muted and minimized. `-VideoAt <seconds>` and
`-VideoFrames` record the game image to `out\pc\captures\` (`tools/make_video.sh` makes the MP4), and `-Pad` scripts
the controller (`app/src/ketsui_input_script.h`).

## 4. The shaders

The deko3d renderer draws with the game's shaders translated ahead of time. Game data: `out/` only.

```bash
python tools/renut_dump_shaders.py      # the 88 shader containers of default.xex -> out/renut_shader_dump
# the PC build (section 3) translates them with Renut's XenosRecomp and its early alpha copies
bash tools/renut_deko3d_shaders.sh      # SPIR-V -> GLSL -> banjo-nx's and guwange-nx's passes -> every shader in the NRO
python tools/renut_release_shaders.py   # the release build's files (out/renut_switch_release)
```

Ketsui compiles 12 more shaders itself at run time, from HLSL text in its image: no game file holds them. Renut
captures a shader its cache lacks (`out/pc/shader_capture`), and the next PC build adds the captures to the dump: run
the PC build once from the boot through the menus, the attract demo and stage 1 before the steps above (until then the
draws that use them are dropped).

The released NRO carries only those 12 (`BUILT_IN` in `tools/renut_release_shaders.py`); the other 88 come from
`deko3d_glsl.bin` beside it, which the installer page makes from the player's `default.xex`
([ketsui-nx-installer](https://github.com/ChanseyIsTheBest/ketsui-nx-installer)). For a test of a release build, put
the installer's file (or `out/renut_switch_release/sd/deko3d_glsl.bin`, the Python tools' copy) next to the NRO.

## 5. The Switch NRO

```powershell
powershell -File tools\win\build_switch.ps1 -Jobs 3 [-Lto on|off] [-Pgo none|generate|use] [-ReleaseShaders on|off]
python tools\check_switch_build.py                       # start-up code at 0, no vector FMA outside vmx_madd_ps, no
                                                         # self-indexed table lookup, no fcvtas in game code
python tools\find_self_loops.py out\switch\ketsui.stripped.elf   # no branch-to-self loops in game code
python tools\plume_vma_names.py --check out\switch
```

`build_switch.ps1` configures `out\switch` the first time (`-BuildDir` builds elsewhere); each option keeps its last
setting when left out. The release 1.0.0 is `-Lto off -ReleaseShaders on`, without PGO (the game needs no more speed;
an LTO build also fused two vector multiply-adds in `sub_828C9BB8`, which `check_switch_build.py` rejects). The NRO's
name, author, version and icon (`app/ketsui_icon.jpg`) are in `app/CMakeLists.txt`. The settings compiled into the
NRO are `app/ketsui_switch_defaults.toml` (written by `tools/make_ketsui_configs.py` from the enhancement table of
`app/src/ketsui_enhancements.cpp`, with the test configs of `app/configs/`), and the player's `ketsui.toml` is
`app/ketsui_switch.toml`. Set `TMP` and `TEMP` to a folder with several GB free for an LTO link (`build_switch.ps1`
uses `out\tmp`).

### Profile-guided optimisation

1.0.0 is built without a profile, and `pgo/` is empty. To record one:

1. `build_switch.ps1 -Pgo generate -BuildDir out\switch_pgo` builds an instrumented NRO. Play it on the console: it
   writes `pgo/ketsui_pgo_<N>.stream` next to the NRO (one file per session).
2. Copy the console's streams to a folder on the PC and run `powershell -File tools\win\pgo_merge.ps1 <folder>`.
   Merge on the PC that built the instrumented NRO.
3. `build_switch.ps1 -Pgo use` builds the optimised NRO. The profile's files are named by the object's path in the
   build folder, so build in `out\switch` for them to apply; a function you change loses its part of it.

## Running in Ryujinx

Put the NRO, `deko3d_glsl.bin` (for a release build), `ketsui.toml` and `game_root` in `sdmc:/switch/ketsui-nx` of
the emulator (the SDK falls back to that folder when the emulator gives no argv).
