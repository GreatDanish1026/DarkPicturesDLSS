# QuarryDLSS

Work towards DLSS for **The Quarry** (PC, Steam build 11105655, Unreal Engine 4.26),
modelled on [days-gone-dlss](https://github.com/teemupynnonen/days-gone-dlss).

The current build is **step 1, the probe**: a ReShade add-on that changes nothing
in the game. It only reports:

- which graphics API the game runs on (Direct3D 11 or 12), the GPU and the swap chain;
- every *temporal* pass, meaning a shader that reads its own output from the previous
  frame. The main TAA pass is one of these, along with Unreal's temporal SSR, depth of
  field and light shafts;
- the **main TAA guess** (the temporal pass with the largest output), with its render
  resolution, output resolution and every texture it reads and writes;
- optionally, a full list of one frame's post-process passes;
- optionally, the constant buffers of the large temporal passes over 8 frames, to find
  the TAA jitter.

Only the texture, UAV and constant-buffer slots a shader declares are listed (read
from its DXBC), because the D3D11 RHI leaves earlier views bound between passes.

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
5. Press **Ctrl+F11** (or the overlay button) once in a busy scene to dump a frame.
   Then, while the camera moves, press **Ctrl+F10** to dump 8 frames of TAA constants.
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
