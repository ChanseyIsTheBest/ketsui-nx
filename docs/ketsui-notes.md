# Ketsui: Kizuna Jigoku Tachi Extra: notes for the port

ketsui-nx is guwange-nx's tree (itself futari-nx's) for Ketsui's Xbox 360 version: the same SDK, the same post-codegen
steps, the 360-exact VMX code generation (app/ketsui_perf.toml) and Renut's Direct3D-level renderer.

## The executable

| | |
|---|---|
| Title | `465607D1`, media `36631DF6` |
| `default.xex` | 15,646,720 bytes, version 0.0.0.1, built 2010-01-28 |
| Image | base `0x82000000`, size `0x013A0000`, code `0x82230000`-`0x82D49CC8` |
| XDK | 2.0.9328 (Futari's) |
| Modules | `default.xex` only: no game cores |

Codegen overrides (app/ketsui_overrides.toml): setjmp `0x82CCCD80` and longjmp `0x82CCC940`; two functions the
generator does not find on its own, `0x82C6B120` (size `0x24`) and `0x82BEB468` (size `0x8`); and 70 functions reached
only through code pointers (`tools/pointer_fixpoint.py --module default`, run with futari-nx's generator: the boot
stopped at "Call to invalid or unregistered function at 0x829F1DF0" before).

## Saves (XamContentResolve)

Ketsui keeps one save package, `Save.dat` (content type 1, displayed as "セーブデータ"), holding one file `Save.dat`
(2,048 bytes). Saving works through XamContentCreate as in the other ports. Loading did not: every boot said "ゲームデータが見つかりません" (game data not found)
and offered to make a new save. Before it opens the package's contents, Ketsui resolves the package's own path with
`XamContentResolve`, opens it (`NtCreateFile`), asks its size (`NtQueryInformationFile`, FileNetworkOpenInformation)
and loads only when the size is 61,440 bytes (its callers of `sub_8223FAC8`: `cmplwi r3,61440`). The SDK had no
XamContentResolve, and keeps a package as a folder of its files, not as an STFS file.

The SDK now has both halves (xenia-edge's semantics, which the user found to be the emulator where loading works):

- `XamContentResolve` (sdk/src/kernel/xam/xam_content.cpp) writes `\Device\XamContentPackages\<xuid 016X>\<title
  08X>\<type 08X>\<file name>` (the xuid for a saved game of a signed-in user, else 0) and registers that key with the
  host folder of the package (`ContentManager::PackageHostPath`).
- The device `\Device\XamContentPackages` (registered at the first resolve) shows each registered package as a
  read-only file whose size is the one its STFS package would have on the console
  (`ContentManager::StfsPackageSize`: the 0xB000-byte header, the file table and the data in 4 KB blocks, and two
  copies of each level of hash tables, each covering 170 blocks), present only while its folder exists. Each folder
  level above it resolves as a directory: `NtCreateFile` resolves the parent and asks it for the file by name (the
  first version, a device that knew only whole keys, failed with "VFS: entry not found" and 0xC000000F). A package
  whose folder is gone is moved out of its directory, not destroyed (a file opened on it stays valid).

Ketsui's save gives exactly 61,440: 0xB000 + (1 table block + 1 data block + 2 x 1 hash table) x 0x1000. Reading the
package file gives no bytes; Ketsui only asks its size. Since then the title shows "ゲームデータ読み込み中" (loading
game data) with its progress bar, then WELCOME and the menu.

## Direct3D, for the native renderer

Ketsui links Futari's XDK, so its D3DDevice is Futari's word for word: Renut's renderer keeps Futari's device offsets.
Its tree (app/src/renut_engine) is futari-nx's renderer plus guwange-nx's generic changes (its hooks for the vertex and
index buffer path, SetFVF, the async locks; the 16-bit texture paths; the bound fetch specialisation; the latency trace
and the late input's notes), without guwange-nx's layout step (XDK 2.0.11164 inserts a word at +0x359C). The split was
made mechanically: guwange-nx's tools/renut_guwange_layout.py applied to futari-nx's tree gives the layout-only
version, and its difference from guwange-nx's tree is the generic part, applied to futari-nx's tree. That generic part
names no device offset above +0x359C (the one it adds, the FVF declaration +12368, is below).

Only the guest addresses differ from Futari's (tools/renut_ketsui_layout.py, applied): each found by pairing Futari's
Direct3D with Ketsui's by exact instruction shape (tools/ketsui_futari_pairs.py: immediates and branch displacements
aside, unique matches only; 452 of Futari's 889 functions in 0x82080000-0x820B0000 pair) and reading the lis +
displacement or bl at the same instruction of both; where it is data, its bytes were compared (the format table, the
full viewport, the D3DCAPS9 and the global qword equal Futari's; the state tables have Futari's layout and defaults).

The hooks (app/src/renut_engine/ketsui_d3d_functions.txt, tools/renut_ketsui_hooks.py writes futari_renut_hooks.h):

- Futari's 69 mapped functions, each the exact-shape pair of Futari's (d3d_match 1.00 against Futari's default.xex for
  all). `D3DVertexShader_Bind` is not in Ketsui (no function of its shape; Futari's calls were its game's).
- 15 functions Ketsui calls that Futari's list does not map, as guwange-nx mapped them: DrawVertices, the vertex and
  index buffers (create, lock, unlock), ConstantBuffer_Unlock, GetDisplayMode, Acquire/ReleaseThreadOwnership,
  Get2D/3DSurfaceDesc, InsertCallback, Present, SetFVF (d3d_match 1.00 against Guwange's).
- DrawIndexedVerticesUP (d3d_match 0.97 against N&B's; neither Futari nor Guwange links it).
- Left to Ketsui's own code, as Futari leaves its copies: the render and sampler state setters and getters, GetViewport,
  SetClipPlane, the gamma setter, D3D_IsFusion: they only read or write device fields the renderer keeps as Direct3D
  does.

Two pitfalls on the way:

- The post-codegen direct calls (`__imp__sub_X` for every function without a hook) bypass a hook site. The tree's
  tools/project.py counted every `futari_*` file of app/ as another port's unbuilt source, the renderer's
  futari_renut_hooks.h too, so its 85 sites were not seen as hooks: the game ran on, drawing nothing (no "Guest D3D
  device created"). Now app/src/renut_engine is exempt; the post-codegen run put 711 calls back on their hooks.
- The SDK's capture (ketsui_video_s) reads the emulated GPU's presenter; with Renut it takes Renut's own frame capture
  (renut::gpu::CaptureFrame), so a Renut run without a single Present records nothing (an empty MP4).

## Shaders

`default.xex` holds 88 shader containers in `.rdata` (71 vertex, 17 pixel: `tools/renut_dump_shaders.py`). Ketsui
compiles the rest itself at run time from HLSL text in its image (seven `main()` entry points next to `ps_3_0` /
`vs_3_0` profile strings, e.g. `float4 Diffuse : register(c0); float4 main( void ) : COLOR {return Diffuse;}`): 12
more containers, which no game file holds (one has its microcode's last 64 bytes in the image, the rest nowhere). Renut
captures a container its cache lacks (`out/pc/shader_capture`; the draws with it are dropped until the next build) and
the PC build copies the captures into the dump (`app/cmake/collect_captured_shaders.cmake`). Before they were in the
cache the menus drew nothing ("0 draws" with Renut). A run from boot through the menus, the attract mode and stage 1
captured no further one. 100 shaders in all, every one translated for deko3d (`tools/renut_deko3d_shaders.sh`: 76
vertex, 24 pixel, 0 failed). For the release the 12 compiled ones go into the NRO (as guwange-nx's 9).

## The frame, and the Swap's fence at the vblank

Ketsui's main loop (`sub_8223F908`, called by the entry point) runs a frame and Swaps, then polls Direct3D's busy test
until the frame's fence is written:

```
loop: sub_829E1030 ... sub_829DCC48 (the frame's render target and Clear), the game step when due (822394D0 ...), the
      draws, Resolve, Swap
      while (sub_828E0058(device)) Sleep(0)      // KickOff when the ring holds anything, then (tag - done) > 2
```

On the console the Swap's fence is written after the flip, at the vblank, so the loop runs once per vblank. Renut
wrote it as soon as its render thread had read the frame, and both PC renderers ran the loop as fast as the host
presented (90-140 Swaps a second minimized on the PC): every Swap's image hashed, about half of them repeated the one
before (291 of 600 with Renut, 429 of 600 emulated), the game's own step running at its own rate. The speed was right
(the 30 s countdown of the player select counts in real time), the pacing was not.

`banjo_renut_swap_at_vblank` (on): a Swap's fence completes at the first guest vblank after the Swap, and never before
the render thread has reached it (so the game cannot rewrite what the frame still reads); fences queued after it
complete after it; released by the vblank thread after Direct3D's vblank handler (the game's vblank callback) has run.
Not in lockstep runs (`vblank_per_swap`). The first try waited for the vblank after the render thread's arrival: 47-51
fps on the PC, a frame whose render thread lagged waited a whole period more.

The game's busy poll then flooded the render thread: every poll's KickOff queued a fence, thousands a frame behind the
held Swap fence (main thread 97 %, render thread 97 %, 44-47 fps). `banjo_renut_kick_idle_wait` (on): a KickOff with no
new work since the last fence while a Swap fence waits for its vblank first waits up to 2 ms for a fence to complete.
An attempt to make the poll see an empty segment instead (the ring's segment word +15136 set at each rewind, so the
test `m_pRing != [+15136] - 4` says nothing to kick) hung the boot: Renut runs the game's Direct3D callbacks
(InsertCallback) from KickOff and the fence tests, and a fence waiting on a callback was never written.

Gameplay with both: 60.00 game frames a second, each step drawn once, the render thread 0.8-1.7 ms a frame on the PC
(3 ms before, with the repeats).

## Late input

The user (2026-10-27): "apply mushi and guwange's late input". With the frame locked to the vblank, Ketsui's frame is
the shape futari-nx's and guwange-nx's late input was made for (the trace: guest vblank, Clear 0.07 ms later, the
frame's controller read 0.08 ms, Swap 0.5 ms; then the wait), except that the game also reads its controllers while it
waits, so "the first read after the Swap" is a poll of the wait loop. `src/ketsui_late_input.cpp` (`futari_late_input`)
therefore starts a frame at each Swap fence Renut writes (the SDK's new `rex::input::SetFrameReadDelayKeyFunction`),
and measures a frame's work from its read to the render thread's arrival at its Swap's fence (not its last draw: the
next frame's Clear comes before its read, which made every frame measure a whole period and the delay 0). PC,
gameplay: the frame's read delayed 6.1 ms on average (576 of 601 frames), 3.7 ms in the next 10 s, still 60.00 fps.
On the Switch the guest vblank also follows the display's refresh, as late after it as the image still makes the next
one (`futari_vblank_display_sync`, `futari_vblank_phase_auto`: futari-nx's).

## The two renderers compared

The same scripted run recorded with the emulated GPU and with Renut (`tools/compare_video.py`, each reference frame
against the closest Renut one): boot logos, the title with the loading bar, WELCOME, the menu, the attract mode
(warning, ranking, CAVE logo), the player select and stage 1 (buildings, tanks, aircraft, bullets, explosions, the HUD,
the side art) match; the remaining differences are timing (the two runs' inputs landed at other frames).

## The company logos' black diagonal (clipping off)

The user (2026-10-28, on the console): start during the attract demo, and half the screen goes black along a diagonal.
Start ends the demo: "Presented by", then the 5pb. GAMES DIVISION #2 and CAVE logos, then the title. The logos at boot
and after a demo that runs out show it too (PC, Renut on Vulkan: white above a line from the left edge's middle to the
top-right corner, black below). A logo frame has two draws ([drawlog], `banjo_renut_diag_draw_log_s`). The white
background is the first: a triangle list of 6 screen-space vertices (vs `E08E40490633A486`), stride 20, a float3
position and a float2 texture coordinate. Its corners are (0,0) (1280,0) (0,720) / (0,720) (1280,0) (1280,720)
(`banjo_renut_diag_stream_dump_vs`, which now logs up to six guest vertices).

The game writes x, y, u and v and never the position's z: one run had 3.0 at the top-right corner, another 0. Its
D3D draws with `D3DRS_VIEWPORTENABLE` false, which sets PA_CL_CLIP_CNTL.CLIP_DISABLE (bit 16): the console does not
clip, so z does not matter (no depth target). Renut built every pipeline with depth clipping on and cut the quad where
the interpolated z left [0, 1]. `ketsui_renut_clip_disable` (on, a fix: no master turns it off) carries the bit in
the pipeline key (`kKeyClipDisable`, PipelineKey::reserved bit 3, kept in the pipeline list's records). Such pipelines
clamp depth instead of clipping it, plume's `depthClipEnabled = false`: Vulkan `depthClampEnable`, deko3d's rasterizer
`depthClampEnable`. PC check, one run with start pressed in the demo: every logo screen whole, at boot and after start.

## Frame pacing

A real-time PC run of stage 1 with every Swap's image hashed (2026-10-28) shows the game draws a new image at every
Swap, 60 a second: no repeated image in ~4,500 comparable Swaps. The only held images are the death and continue
screens' own animation (a 15 fps loop, a 2-image blink). So uneven pacing on the console in stage 1 comes from the port,
not from slowdown in the game. Later, heavier stretches were not measured.

Console logs (2026-10-28, the 1.0.0 NRO; out\pc\runlogs\switch_run1 and the second run's ketsui_002.log): stage 1 at
55-57 fps handheld and 57-58 fps docked with an overclock (GPU 1305 MHz), though the GPU took 1-2.6 ms a frame and the
render thread 3.5-6 ms, and no core passed 43 % (the user). About 35 images a second were not finished on the GPU at
the first refresh after their present (`futari_vblank_phase_auto` stuck at 0), and late input never delayed a read.
(The `[latency]` line's "input -> swap" is not the frame's CPU time: the frame's first controller read is a poll in
the wait loop of the frame before.)

The latency trace (`ketsui_diag_latency_trace_s = 90`) showed the cause: the game's frame took under 2 ms of CPU, but
it started 0.8, 2.3, 1.8 and 6.3 ms after the guest vblank (the PC: 0.05-0.3 ms). Ketsui's frame-end poll
(`while (sub_828E0058(device)) Sleep(0)`) kicks at every turn and then tests that kick's own fence. After the vblank no
Swap fence waits, so `banjo_renut_kick_idle_wait` does not apply. The queued fence was written by the render thread
after the test, and the loop went round again, kicking, until the render thread won the race. Frames that started late
presented close to the next refresh. Some missed it, and with 2 images the next frame's GPU work then waited for the
display (the "late" state, every image a refresh later), until a frame without a Swap drained the queue. Each change of
state was a hitch.

- `banjo_renut_kick_idle_fence` (on): a KickOff with nothing queued since the newest fence, once every render-thread
  item is done and no fence is pending, writes its fence at once (`render_thread::TryCompleteIdleFence`). Same
  fences, the same order, written when the render thread would reach them.
- `banjo_renut_kick_busy_wait` (on): such a KickOff while the render thread still works (no Swap fence waiting) first
  waits up to 2 ms for a fence, as `banjo_renut_kick_idle_wait` does while one waits. The spin had queued two items
  for the render thread at every turn.
- `ketsui_late_input_display_guard` (on): with frames now starting at the vblank, late input finds slack, but its
  frame ends at the render thread's arrival at the Swap fence, before the present and the GPU's last work. An image not
  finished at its refresh adds 1 ms to its margin (up to `ketsui_late_input_max_us`). 180 in time in a row take 0.25
  ms off.

PC (real time, stage 1): every frame's poll fence written at once (one a frame), the frame's Clear 0.02 ms after the
vblank, polls queued behind render-thread work down from 2,600-92,000 to 1-60 per 10 s, the render thread 0.35-1.08
ms a frame (0.56-1.43 before), 60.00 fps. The `[latency]` lines report the polls ("polls (banjo_renut_kick_idle_fence)")
and the guard ("display guard +N ms, M images late at their refresh").

Console, the fixed NRO (2026-10-28 18:03, handheld, out\pc\runlogs\switch_run3): stage 1 at 60.00 fps (590-601 game
frames per 10 s; 55-57 before), every frame's poll fence written at once, the frame's Clear 0.02 ms after the guest
vblank, the frame 1.8 ms to its Swap, presented 3.6-4.1 ms after it. The user: fixed. One thing left: every image is
still "not finished at its refresh" (600 of 600 per 10 s). Presented about 11 ms before the refresh with 2.6 ms of GPU
work, it waits for its swapchain image: with 2 images that is the one the display shows until the image before is
latched, at that same refresh. The queue stays one image deep from boot on, so each image is shown one refresh later
than it could be: smooth, but about 17 ms of input lag. The display guard then sits at +12 ms and late input never
delays.

`ketsui_renut_queue_drain` (on; the user: "keep double buffering and empty the queue once"): the vblank thread counts
new images in a row not finished at their refresh. After 60 (a second), it has the next Swap's fence held one vblank
more, once (`render_thread::HoldNextSwapFence`), so the game's next frame starts a refresh later. The image before
still makes its refresh and the queue empties, so every image after that is shown at the refresh after its frame
started. Not again for 5 s; when the queue fills again within twice that, the gap doubles (up to 5 minutes: real
lateness, which a drain cannot help). The 10 s `[latency]` lines count the drains ("queue drains in all") and the
holds ("Swap fences held a vblank more"). `ketsui_diag_swap_hold_every_s` holds one on a timer for a PC test (the
PC's timer vblank has no display to drain). PC, a hold every 10 s, stage 1: one hold per 10 s, no stall, the game
went on.

Console, the drain NRO (2026-10-28 18:46, handheld, out\pc\runlogs\switch_run4): three drains, all in the boot,
title and first loading (each a fill of the queue there), then none. Late images per 10 s went 77, 86, 62, 0, 64 (the
load), then 0 for the rest of the run. Stage 1 at 60.00 fps. Present to the refresh that shows the image: 10.0-11.0
ms (max 11.6); input to that refresh 28.1-28.8 ms. Before, the few measurable images took 26.7 and 46.0 ms (every other
image was superseded in the probe): a refresh less. Late input works again: the display guard came down from +12 to
+6.75 ms in 80 s (0.25 ms per 180 images in time), 409 of 600 reads delayed in the last 10 s. The phase stays 0 for 5
minutes after the last miss (`futari_vblank_phase_auto`'s hold), then may move later.

## The 1.0.0 release build

The user (2026-10-28): the final build, on GitHub with a web installer (ketsui-nx-installer). Built with
`-ReleaseShaders on`: the NRO carries the 12 shaders Ketsui compiles at run time, and the installer makes the other 88
from the player's `default.xex`. The page's GLSL equals the native chain's for all 88, 24 byte for byte, the rest but
for DXC's numbering of temporaries. The first try had LTO on, and `tools/check_switch_build.py` failed it: two vector
fmla in `sub_828C9BB8` (the hand-written native's hooked function), whose generated, localized and hand-written texts
have no `vmx_madd_ps`. That is contraction past `-ffp-contract=off` at the link; the non-LTO builds had none. The user:
"the game isn't intensive at all we don't need LTO": 1.0.0 is built without LTO (and without PGO).
