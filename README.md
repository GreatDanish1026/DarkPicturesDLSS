# QuarryDLSS

DLSS for **The Quarry** (PC, Steam build 11105655, Unreal Engine 4.26), following the
approach of [days-gone-dlss](https://github.com/teemupynnonen/days-gone-dlss).

Current build: **0.5.0, first DLSS test** (Direct3D 12 only, launch with `-dx12`).
It's still the probe, plus:

- the render scale is forced to 67% with TAA upsampling (the overlay slider changes it);
- the game's TAAU compute pass is replaced by **DLSS Super Resolution**: a small pass
  turns UE's velocity and depth into DLSS motion vectors, then NGX writes DLSS's output
  into the TAAU's own output texture. If anything fails, the game's TAAU runs as normal;
- overlay controls: DLSS on/off, jitter sign flips, "motion vectors include jitter".

What the probe reports, and how the game was mapped, is in `docs/FINDINGS.md`.

## Build

```powershell
.\build.ps1            # -> bin\QuarryDLSS.addon64
.\build.ps1 -Test      # also builds and runs the DXBC parser tests
.\build.ps1 -Install   # also copies it to the game's SMG026\Binaries\Win64
```

Needs Visual Studio 2022 or newer with the C++ x64 tools. MinGW builds crash inside
ReShade: it returns API structs with a different calling convention than MSVC.

## Test run

1. Install [ReShade](https://reshade.me) 6.8+ **with full add-on support** for
   `SMG026\Binaries\Win64\TheQuarry-Win64-Shipping.exe` (not the `TheQuarry.exe`
   launcher). Pick DirectX 10/11/12; you don't need any effect packs.
2. Copy `bin\QuarryDLSS.addon64` next to that exe (or run `.\build.ps1 -Install`).
3. For a clean result, move `BetterAntiAliasing_P.pak` and `zzzz_PerformanceTuning_P.pak`
   out of `SMG026\Content\Paks\~mods` while testing. They likely change TAA settings.
4. Start the game and load into gameplay. Press **Home**, then open the
   **Add-ons → QuarryDLSS probe** section to see the API and the main TAA guess.
5. Press **Ctrl+F9** (or the overlay button) once in a busy scene to dump a frame.
   Then, while the camera moves, press **Ctrl+F10** to dump 8 frames of TAA constants.
   Probe 0.4+ forces the render scale to 67% with TAA upsampling (the game ignores
   Engine.ini for this); untick it or move the slider in the overlay.
6. Optional: try a second run with `-dx12` in the Steam launch options, to see whether
   the Direct3D 12 renderer starts.

Send back these files from `SMG026\Binaries\Win64`:

- `QuarryDLSS.log` (and `QuarryDLSS.prev.log` from the `-dx12` run)
- `QuarryDLSS_frame_<n>.txt`
- `QuarryDLSS_cbuffers_<n>.txt`
- `ReShade.log`

Then step 2 can map the engine hooks (the temporal upscaler slot, jitter, camera) for this build.

## Limits of the probe

- On Direct3D 12 (`-dx12`), bindings are rebuilt by mirroring the game's descriptor heaps
  (view creation and CopyDescriptors) and root signatures, and constant buffers are read
  from the game's mapped upload memory. If the overlay's D3D12 line shows 0 heaps or 0
  descriptor writes, that mirroring isn't working and the probe will find no temporal passes.
- Passes are identified by the first 8 bytes of the shader's DXBC digest. These stay the
  same between runs of the same game build.
