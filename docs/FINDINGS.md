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

## Main TAA pass

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
