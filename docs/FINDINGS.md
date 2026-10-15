# Renderer findings

Steam build 11105655, exe PE timestamp `0x64493c74`, UE 4.26.2. Probe 0.1.0, run of
2026-10-15 (`QuarryDLSS_frame_2751.txt`, gameplay at 3840x2160).

## Device and output

- **Direct3D 11**, feature level 11_0. Without `-dx12` the game doesn't use D3D12.
- Swap chain: 3840x2160 `R10G10B10A2_UNORM`, flip-discard (`SwapEffect 4`), tearing
  allowed (`Flags 0x802`), windowed.
- During startup the game creates throwaway devices on the RTX 5080, the AMD iGPU and
  WARP before it creates the real device on the RTX 5080.
- Gameplay renders **letterboxed**: the view rect is 3840x1600 inside 3840x2160 render
  targets, and the final pass writes it at y offset 280. The screen percentage is 100%
  (`sg.ResolutionQuality=100`), so TAA runs without upsampling.

## Direct3D 12 (`-dx12`)

Probe 0.2 run on 2026-10-15 with `-dx12` in the launch options:

- **The D3D12 RHI works.** The game ran for about 2.5 minutes (8,700 frames) and didn't
  crash. Swap chain: 3840x2160 `R10G10B10A2_UNORM`, 3 buffers. It creates throwaway
  D3D12 devices at startup, the same way it does on D3D11.
- **Same shaders on both APIs.** The D3D12 shader keys match the D3D11 ones (for example
  `89b573cf3e394331`, `f0db50aa69e703c1`), so UE 4.26 ships SM5 DXBC for D3D12 too, not
  DXIL. The DXBC declaration parser works on both.
- Probe 0.2 found no temporal passes and no constants on D3D12, because it didn't decode
  descriptor tables. Probe 0.3 does (below).
- This matters for frame generation: with a working D3D12 path, Streamline can run
  natively. We wouldn't need the D3D11/D3D12 shared-resource bridge that Days Gone uses.

### Probe 0.3 on D3D12: bindings and jitter

Run of 2026-10-15 with `-dx12` (`QuarryDLSS_cbuffers_9300.txt`, gameplay, camera moving):

- The descriptor-heap mirroring works. The probe finds the same temporal passes with the
  same shader keys as on D3D11, including the main TAA `2f606fcaa114ea67` with the same
  slots: t0 exposure, t1 colour, t2 depth, t4 stencil, t5 history, u0 output. Its
  first sighting (frame 67, loading) had a 1x1 dummy in t3 (velocity), so velocity is
  only bound once the game writes it. The D3D11 gameplay frame had the real
  RGBA16_UNORM velocity there.
- The D3D12 adapter is identified: RTX 5080. As on D3D11, the game also creates
  throwaway devices on the AMD iGPU and WARP.

**Main TAA constants** (`2f606fcaa114ea67`):

- `cb0` (29 float4) holds the TAA pass parameters:
  - row 3 = InputSceneColorSize (3840, 2160, 1/3840, 1/2160);
  - row 4 = (half texel, max viewport UV): 0.7405 = 1599.5/2160, the letterboxed view;
  - row 0.y = 0.04, the current-frame weight;
  - **row 28.xy = jitter in pixels** (`TemporalJitterPixels`), with y pointing down the
    screen.
- `cb1` (145 float4 declared) is UE's View uniform buffer:
  - **row 126 = `View.TemporalAAJitter`**: .xy is this frame's jitter and .zw last
    frame's, in clip units. Clip = (px × 2/3840, −py × 2/1600), divided by the 3840x1600
    view rect, not the 3840x2160 texture.
  - rows 28-31 = ViewToClip (the projection). It's reversed-Z with an infinite far plane,
    the near plane at 10 cm (row 31.z), focal length 1.6667 (about 62° horizontal FOV)
    and aspect 3840/1600. Jitter is folded into row 30.xy.
  - rows 0-3 = TranslatedWorldToClip, rows 4-7 = WorldToClip. Rows 73, 85 and 90
    carry the previous frame's jitter, so they're the previous-frame matrices.

**Jitter sequence:** Halton(2,3) minus 0.5, **8 phases**, indices 1-8 then repeating:
(0, −0.167), (−0.25, 0.167), (0.25, −0.389), (−0.375, −0.056), (0.125, 0.278),
(−0.125, −0.278), (0.375, 0.056), (−0.4375, 0.389). That's UE's default
`r.TemporalAASamples=8`. DLSS wants more phases at lower render scales, so the mod has
to raise this, as Days Gone does.

**Feeding DLSS:** jitter offset = `cb0` row 28.xy (pixels). Check the sign once in-game
with the DLSS jitter debug overlay. Depth is reversed-Z (`DepthInverted`), with near =
10 and an infinite far plane. Motion vectors are UE's velocity buffer, which only covers
moving objects, plus camera motion reconstructed from depth and the previous-frame
matrices in `cb1`.

### Render scale can't be set from Engine.ini [SystemSettings]

Run of 2026-10-15 (`-dx12`) with `r.ScreenPercentage=67` and `r.TemporalAA.Upsampling=1`
under `[SystemSettings]` in the user `Engine.ini`. **No effect:** the view rect stayed at
3840x1600 (main TAA still dispatched 480x200 groups, and `cb0` still had a 3840x2160
input with 0.7405 max V). The game sets the screen percentage itself. Its own settings
(`[/Script/SMGRuntime.GamePCSettings]` in GameUserSettings.ini) include
`bUseDynamicResolution`, `VRSQuality` and `RTReflectionsQuality` / `bRTAmbientOcclusionEnabled`,
so the D3D12 build also has ray-traced options.

Next try: the same cvars under `[ConsoleVariables]`, which is applied at a higher
priority. If the game still overrides them, the mod has to set the screen percentage
in-engine every frame (as Days Gone writes the cvar data directly), which it needs to
do anyway.

## Main TAA pass (Direct3D 11)

UE 4.26's standard compute TAA (`TemporalAA.usf`, `FTemporalAACS`), dispatched as
8x8 thread groups (480x200 for 3840x1600). Identify it by **position**, not by hash: it
runs right after the translucency-composite PS `04346c4e90a9704d`, and its output feeds
the bloom/eye-adaptation downsample (`ba3e1c97e7c22acb`) and the tonemapper
(`4b7e40416e1f163b`). Two permutations seen so far:

| Shader | When |
|---|---|
| `2f606fcaa114ea67` | 3840x2160 gameplay |
| `446e38a56d9893ac` | 1280x720 (the window briefly resized) |

Bindings (`2f606fcaa114ea67`):

| Slot | Texture | Meaning |
|---|---|---|
| t0 | 1x1 RGBA32F | eye adaptation (exposure) |
| t1 | 3840x2160 RGBA16F | **scene colour** (render res; output of `04346c4e90a9704d`) |
| t2 | 3840x2160 R32F view of R32G8X24 | **scene depth** (reversed-Z, UE4) |
| t3 | 3840x2160 RGBA16_UNORM | **velocity** (written by geometry; 4.26 packs depth into it) |
| t4 | 3840x2160 X32_G8 view of the depth | stencil (responsive AA) |
| t5 | 3840x2160 RGBA16F | **history** (its own output last frame) |
| u0 | 3840x2160 RGBA16F | **output**, read by the downsample and tonemapper |

Slots above t5 are probably stale bindings that the D3D11 RHI left in place (see Probe
issues).

## Other temporal passes

- `b569efd647217872` (CS, full res): has the same shape as TAA (eye adaptation, colour,
  depth, velocity, history), but runs much earlier, after the full-screen PS
  `cd75f114186bff1a` that reads the GBuffer. It's probably a second TemporalAA.usf
  permutation (SSR/SSGI denoise, or a mod: `NativeSSGI_P.pak` and
  `BetterReflections_P.pak` are installed). It's **not** the main TAA, because it runs
  before translucency and before the downsample.
- `5019389874611ed9` (CS, half res, R11G11B10F + RG8 history): looks like the
  SSAO/SSGI denoiser.
- `81326cac0579e50c` (PS, half res, RGBA16F + RG16F history): probably DOF or
  separate-translucency temporal.
- `885dce43623dfdaa` / `2a455ff261684512` (CS, 3D dispatch, 960x540): volumetric fog
  temporal reprojection.
- `6ab2395f4c09edf2` / `799ce21f5644c2ac` (buffers only): GPU particle simulation.

## Probe issues (all addressed in probe 0.2; waiting on a game run)

- D3D11 SRV slots stay bound after a pass, so every pass lists stale inputs. Decode the
  shader's own `dcl_resource` / `dcl_uav` declarations from its DXBC and list only the
  slots it declares.
- An output that the same shader wrote last frame through pooled ping-pong is
  mislabelled "HISTORY". That label should only apply to inputs.
- Jitter isn't captured yet. It's in the view uniform buffer (`View.TemporalAAJitter`)
  and in the TAA constant buffer. Dump the main TAA's constant buffers for a few frames.

## Next experiments

- Set `r.ScreenPercentage=67` and `r.TemporalAA.Upsampling=1` (Engine.ini
  `[SystemSettings]`) to check that the main TAA becomes the upscaler, with render-res
  inputs and an output-res result. DLSS has to replace exactly that pass.
- A `-dx12` run, to see if the D3D12 RHI starts.
- Repeat with the remaining `~mods` paks removed. `NativeSSGI_P.pak`,
  `BetterReflections_P.pak` and `zzz_NoCameraEffects_P.pak` all change post-processing.
